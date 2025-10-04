/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
// standard includes
#include <libavutil/log.h>
#include <thread>

// lib includes
#include <opus/opus_multistream.h>

#ifndef restrict
  #define restrict __restrict__
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_id.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

// local includes
#include "audio.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

namespace audio {
  using namespace std::literals;
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;

  static int start_audio_control(audio_ctx_t &ctx);
  static void stop_audio_control(audio_ctx_t &);
  static void apply_surround_params(stream_config_t &stream, const stream_params_t &params);

  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // corresponding bitrate adjustment logic in rtsp_stream::cmd_announce()
  stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51,
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51,
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71,
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71,
      2048000,
    },
  };

  void free_ctx(AVCodecContext *ctx) {
    avcodec_free_context(&ctx);
  }

  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  void free_packet(AVPacket *packet) {
    av_packet_free(&packet);
  }

  void free_fifo(AVAudioFifo *fifo) {
    av_audio_fifo_free(fifo);
  }

  void free_swr_ctx(SwrContext *ctx) {
    swr_free(&ctx);
  }

  void free_swr_buffer(uint8_t **buffer) {
    if (&buffer[0]) {
      av_freep(&buffer[0]);
    }
    av_freep(&buffer);
  }

  void opusEncode(sample_queue_t samples, stream_config_t stream, config_t config, auto &packets, void *channel_data) {
    opus_t opus {opus_multistream_encoder_create(
      stream.sampleRate,
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      stream.mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr
    )};

    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));

    BOOST_LOG(info) << "Opus initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};

      int bytes = opus_multistream_encode_float(opus.get(), sample->data(), frame_size, std::begin(packet), packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));
    }
  }

  std::string get_libav_channel_spec(int count, const uint8_t *mapping) {
    switch (count) {
      case 2:
        return std::format("{}+{}", platf::speaker::speakerName(mapping[0]), platf::speaker::speakerName(mapping[1]));
      case 6:
        return std::format("{}+{}+{}+{}+{}+{}", platf::speaker::speakerName(mapping[0]), platf::speaker::speakerName(mapping[1]), platf::speaker::speakerName(mapping[2]), platf::speaker::speakerName(mapping[3]), platf::speaker::speakerName(mapping[4]), platf::speaker::speakerName(mapping[5]));
      case 8:
        return std::format("{}+{}+{}+{}+{}+{}+{}+{}", platf::speaker::speakerName(mapping[0]), platf::speaker::speakerName(mapping[1]), platf::speaker::speakerName(mapping[2]), platf::speaker::speakerName(mapping[3]), platf::speaker::speakerName(mapping[4]), platf::speaker::speakerName(mapping[5]), platf::speaker::speakerName(mapping[6]), platf::speaker::speakerName(mapping[7]));
      default:
        return "";
    }
  }

  bool initEncoder(avcodec_ctx_t &ctx, const AVCodec *codec, int bitrate, int samplerate, int framesize, int channels, bool customsurround, const uint8_t *mapping) {
    codec = avcodec_find_encoder(AV_CODEC_ID_AC3);
    if (codec == nullptr) {
      BOOST_LOG(error) << "Could not find AC3 encoder";
      return false;
    }

    ctx = avcodec_ctx_t {avcodec_alloc_context3(codec)};
    ctx->bit_rate = bitrate;
    ctx->sample_rate = samplerate;
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    ctx->time_base = (AVRational) {1, samplerate};
    ctx->frame_size = 1536;
    if (customsurround) {
      av_channel_layout_from_string(&ctx->ch_layout, get_libav_channel_spec(channels, mapping).c_str());
    } else {
      av_channel_layout_default(&ctx->ch_layout, channels);
    }
    int ret;
    if ((ret = avcodec_open2(ctx.get(), codec, nullptr)) < 0) {
      BOOST_LOG(error) << "Could not initialize libav AC3 encoder: "sv << av_err2str(ret);
      return false;
    }
    return true;
  }

  int doAC3Encode(avcodec_ctx_t &enc_ctx, avcodec_audio_fifo_t &queue, unsigned char *data, int frame_size = 1536) {
    avcodec_frame_t decodedFrame {av_frame_alloc()};
    if (!decodedFrame.get()) {
      BOOST_LOG(error) << "Failed to allocate frame"sv;
      return -1;
    }
    decodedFrame->nb_samples = frame_size;
    av_channel_layout_copy(&decodedFrame->ch_layout, &enc_ctx->ch_layout);
    decodedFrame->format = enc_ctx->sample_fmt;
    decodedFrame->sample_rate = enc_ctx->sample_rate;
    int ret = 0;

    if ((ret = av_frame_get_buffer(decodedFrame.get(), 0)) < 0) {
      BOOST_LOG(error) << "Failed to allocate frame buffers: "sv << av_err2str(ret);
      return -1;
    }

    avcodec_packet_t outPacket {av_packet_alloc()};
    if (!outPacket.get()) {
      BOOST_LOG(error) << "Could not allocate packet"sv;
      return -1;
    }

    if (av_audio_fifo_read(queue.get(), (void **) decodedFrame->data, frame_size) < frame_size) {
      BOOST_LOG(error) << "Failed to retreive frame from FIFO"sv;
      return -1;
    }

    uint8_t buffer[frame_size];
    int bytes = 0;
    ret = avcodec_send_frame(enc_ctx.get(), decodedFrame.get());
    while (ret >= 0) {
      ret = avcodec_receive_packet(enc_ctx.get(), outPacket.get());
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      } else if (ret < 0) {
        BOOST_LOG(error) << "Couldn't get encoded packet: "sv << av_err2str(ret);
        return -1;
      }

      // write data to packet
      memcpy(buffer + bytes, &outPacket->data[0], outPacket->size);
      bytes += outPacket->size;
    }
    memcpy(data, buffer, bytes);
    return bytes;
  }

  int enqueueSamples(avcodec_audio_fifo_t &queue, uint8_t **samples, const int frame_size = 1536) {
    int returnCode;
    if ((returnCode = av_audio_fifo_realloc(queue.get(), av_audio_fifo_size(queue.get()) + frame_size)) < 0) {
      BOOST_LOG(error) << "Failed to reallocate FIFO: "sv << av_err2str(returnCode);
      return returnCode;
    }
    if (av_audio_fifo_write(queue.get(), (void **) samples, frame_size) < frame_size) {
      BOOST_LOG(error) << "Failed to write to FIFO: "sv;
      return -1;
    }
    return 0;
  }

  void ac3Encode(sample_queue_t samples, stream_config_t stream, config_t config, auto &packets, void *channel_data) {
    avcodec_ctx_t encodeContext;
    swr_ctx_t resampler {swr_alloc()};
    const AVCodec *outCodec;
    auto outputFormat = AV_SAMPLE_FMT_FLTP;

    AVChannelLayout channelLayout;
    if (config.channels == 2) {
      channelLayout = AV_CHANNEL_LAYOUT_STEREO;
    } else if (config.channels == 6) {
      channelLayout = AV_CHANNEL_LAYOUT_5POINT1;
    } else if (config.channels == 8) {
      channelLayout = AV_CHANNEL_LAYOUT_7POINT1;
    }
    av_opt_set_chlayout(resampler.get(), "in_chlayout", &channelLayout, 0);
    av_opt_set_int(resampler.get(), "in_sample_rate", stream.sampleRate, 0);
    av_opt_set_sample_fmt(resampler.get(), "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0), 0;

    av_opt_set_chlayout(resampler.get(), "out_chlayout", &channelLayout, 0);
    av_opt_set_int(resampler.get(), "out_sample_rate", stream.sampleRate, 0);
    av_opt_set_sample_fmt(resampler.get(), "out_sample_fmt", outputFormat, 0);

    int returnCode = 0;
    if ((returnCode = swr_init(resampler.get())) < 0) {
      BOOST_LOG(error) << "Failed to init resampler";
      return;
    }
    swr_buffer_t src_data, dst_data;
    int dst_linesize;
    int src_nb_samples = 1536, dst_nb_samples, max_dst_nb_samples;
    max_dst_nb_samples = dst_nb_samples =
      av_rescale_rnd(src_nb_samples, stream.sampleRate, stream.sampleRate, AV_ROUND_UP);
    if ((returnCode = av_samples_alloc_array_and_samples(&dst_data, &dst_linesize, config.channels, dst_nb_samples, outputFormat, 0)) < 0) {
      BOOST_LOG(error) << "Failed to allocate resampler output buffers: "sv << av_err2str(returnCode);
      return;
    }

    avcodec_audio_fifo_t queue {av_audio_fifo_alloc(outputFormat, config.channels, 1)};
    if (!queue.get()) {
      BOOST_LOG(error) << "Failed to allocate fifo queue"sv;
      return;
    }

    auto frame_size = 1536;

    if (!initEncoder(encodeContext, outCodec, stream.bitrate, stream.sampleRate, frame_size, stream.channelCount, config.flags[config_t::CUSTOM_SURROUND_PARAMS], stream.mapping)) {
      BOOST_LOG(error) << "Shutting down AC3 encoder";
      return;
    }

    BOOST_LOG(info) << "libavcodec AC3 encoder initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    while (auto sample = samples->pop()) {
      buffer_t packet {1536};
      dst_nb_samples = av_rescale_rnd(swr_get_delay(resampler.get(), stream.sampleRate) + src_nb_samples, stream.sampleRate, stream.sampleRate, AV_ROUND_UP);
      if (dst_nb_samples > max_dst_nb_samples) {
        av_freep(&dst_data.get()[0]);
        returnCode = av_samples_alloc(dst_data.get(), nullptr, config.channels, dst_nb_samples, outputFormat, 0);
        if (returnCode < 0) {
          break;
        }
        max_dst_nb_samples = dst_nb_samples;
      }

      // Convert our raw LPCM to correct input format for AC3
      returnCode = swr_convert(resampler.get(), dst_data.get(), dst_nb_samples, (const uint8_t **) &sample, src_nb_samples);
      if (returnCode < 0) {
        BOOST_LOG(error) << "Failed to resample audio: "sv << av_err2str(returnCode);
        return;
      }

      if ((returnCode = enqueueSamples(queue, dst_data.get(), frame_size)) < 0) {
        BOOST_LOG(error) << "Failed to add to queue: "sv << av_err2str(returnCode);
        return;
      }

      int bytes = doAC3Encode(encodeContext, queue, std::begin(packet));
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio"sv;
        packets->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));
    }
  }

  void encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    // Encoding takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    if (config.encoding == OPUS) {
      BOOST_LOG(debug) << "Audio encoding as: OPUS"sv;
      opusEncode(samples, stream, config, packets, channel_data);
    } else if (config.encoding == AC3) {
      BOOST_LOG(debug) << "Audio encoding as: AC3"sv;
      ac3Encode(samples, stream, config, packets, channel_data);
    } else {
      BOOST_LOG(error) << "Encoding not supported";
      throw "Encoding not supported";
    }
  }

  void capture(safe::mail_t mail, config_t config, void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    if (!config::audio.stream) {
      shutdown_event->view();
      return;
    }
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    auto ref = get_audio_ctx_ref();
    if (!ref) {
      return;
    }

    auto init_failure_fg = util::fail_guard([&shutdown_event]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;

      // Wait for shutdown to be signalled if we fail init.
      // This allows streaming to continue without audio.
      shutdown_event->view();
    });

    auto &control = ref->control;
    if (!control) {
      return;
    }

    // Order of priority:
    // 1. Virtual sink
    // 2. Audio sink
    // 3. Host
    std::string *sink = &ref->sink.host;
    if (!config::audio.sink.empty()) {
      sink = &config::audio.sink;
    }

    // Prefer the virtual sink if host playback is disabled or there's no other sink
    if (ref->sink.null && (!config.flags[config_t::HOST_AUDIO] || sink->empty())) {
      auto &null = *ref->sink.null;
      switch (stream.channelCount) {
        case 2:
          sink = &null.stereo;
          break;
        case 6:
          sink = &null.surround51;
          break;
        case 8:
          sink = &null.surround71;
          break;
      }
    }

    // Only the first to start a session may change the default sink
    if (!ref->sink_flag->exchange(true, std::memory_order_acquire)) {
      // If the selected sink is different than the current one, change sinks.
      ref->restore_sink = ref->sink.host != *sink;
      if (ref->restore_sink) {
        if (control->set_sink(*sink)) {
          return;
        }
      }
    }

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    auto mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size);
    if (!mic) {
      return;
    }

    // Audio is initialized, so we don't want to print the failure message
    init_failure_fg.disable();

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    auto samples = std::make_shared<sample_queue_t::element_type>(30);
    std::thread thread {encodeThread, samples, config, channel_data};

    auto fg = util::fail_guard([&]() {
      samples->stop();
      thread.join();

      shutdown_event->view();
    });

    int samples_per_frame = frame_size * stream.channelCount;

    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);
      switch (status) {
        case platf::capture_e::ok:
          break;
        case platf::capture_e::timeout:
          continue;
        case platf::capture_e::reinit:
          BOOST_LOG(info) << "Reinitializing audio capture"sv;
          mic.reset();
          do {
            mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size);
            if (!mic) {
              BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
            }
          } while (!mic && !shutdown_event->view(5s));
          continue;
        default:
          return;
      }

      samples->raise(std::move(sample_buffer));
    }
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    static auto control_shared {safe::make_shared<audio_ctx_t>(start_audio_control, stop_audio_control)};
    return control_shared.ref();
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  int map_stream(int channels, bool quality) {
    int shift = quality ? 1 : 0;
    switch (channels) {
      case 2:
        return STEREO + shift;
      case 6:
        return SURROUND51 + shift;
      case 8:
        return SURROUND71 + shift;
    }
    return STEREO;
  }

  int start_audio_control(audio_ctx_t &ctx) {
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);

    // The default sink has not been replaced yet.
    ctx.restore_sink = false;

    if (!(ctx.control = platf::audio_control())) {
      return 0;
    }

    auto sink = ctx.control->sink_info();
    if (!sink) {
      // Let the calling code know it failed
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);

    fg.disable();
    return 0;
  }

  void stop_audio_control(audio_ctx_t &ctx) {
    // restore audio-sink if applicable
    if (!ctx.restore_sink) {
      return;
    }

    // Change back to the host sink, unless there was none
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (!sink.empty()) {
      // Best effort, it's allowed to fail
      ctx.control->set_sink(sink);
    }
  }

  void apply_surround_params(stream_config_t &stream, const stream_params_t &params) {
    stream.channelCount = params.channelCount;
    stream.streams = params.streams;
    stream.coupledStreams = params.coupledStreams;
    stream.mapping = params.mapping;
  }
}  // namespace audio
