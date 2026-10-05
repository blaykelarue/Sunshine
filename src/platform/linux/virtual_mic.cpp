/**
 * @file src/platform/linux/virtual_mic.cpp
 * @brief Definitions for the PipeWire virtual microphone used by microphone redirection.
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

// lib includes
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/ringbuffer.h>

// local includes
#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace {
  constexpr std::uint32_t sample_rate = 48000;
  constexpr std::uint32_t sample_size = sizeof(std::int16_t);
  // Power of two so the free-running 32-bit ring indices stay continuous when they wrap.
  constexpr std::uint32_t ring_size = 32768;  // ~341 ms of mono S16 audio
  constexpr std::uint32_t ring_mask = ring_size - 1;
  // Audio queued in addition to one graph cycle before playback (re)starts, which absorbs the
  // mismatch between the 20 ms network frames and the PipeWire graph quantum.
  constexpr std::uint32_t prebuffer_bytes = sample_rate / 50 * sample_size;
  // Upper bound on queued audio. Anything older is discarded, e.g. after nothing recorded for a while.
  constexpr std::uint32_t max_queued_bytes = sample_rate / 10 * sample_size;

  /**
   * @brief Virtual microphone backed by a PipeWire stream node of class `Audio/Source`.
   *
   * Applications see the node as a regular input device. Samples are handed from the writer thread
   * to the PipeWire realtime thread through a lock-free single-producer/single-consumer ring.
   */
  class pipewire_mic_t: public platf::virtual_mic_t {
  public:
    pipewire_mic_t() = default;
    pipewire_mic_t(const pipewire_mic_t &) = delete;
    pipewire_mic_t &operator=(const pipewire_mic_t &) = delete;

    ~pipewire_mic_t() override {
      if (loop) {
        pw_thread_loop_stop(loop);
      }
      if (stream) {
        pw_stream_destroy(stream);
      }
      if (loop) {
        pw_thread_loop_destroy(loop);
      }
    }

    /**
     * @brief Create the PipeWire node and wait until the server accepted it.
     *
     * @return 0 on success, -1 on failure.
     */
    int init() {
      pw_init(nullptr, nullptr);

      loop = pw_thread_loop_new("sunshine-mic", nullptr);
      if (!loop) {
        BOOST_LOG(error) << "Couldn't create PipeWire thread loop for the virtual microphone"sv;
        return -1;
      }

      auto props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,
        "Audio",
        PW_KEY_MEDIA_CATEGORY,
        "Capture",
        PW_KEY_MEDIA_ROLE,
        "Communication",
        PW_KEY_MEDIA_CLASS,
        "Audio/Source",
        PW_KEY_NODE_NAME,
        "sunshine-mic",
        PW_KEY_NODE_DESCRIPTION,
        "Sunshine Microphone",
        nullptr
      );

      static constexpr pw_stream_events events = [] {
        pw_stream_events e {};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = &pipewire_mic_t::on_state_changed;
        e.process = &pipewire_mic_t::on_process;
        return e;
      }();

      // The stream takes ownership of props.
      stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "Sunshine Microphone", props, &events, this);
      if (!stream) {
        BOOST_LOG(error) << "Couldn't create PipeWire virtual microphone stream"sv;
        return -1;
      }

      spa_audio_info_raw audio_info {};
      audio_info.format = SPA_AUDIO_FORMAT_S16;
      audio_info.rate = sample_rate;
      audio_info.channels = 1;
      audio_info.position[0] = SPA_AUDIO_CHANNEL_MONO;

      std::array<std::uint8_t, 1024> pod_buffer;
      spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer.data(), pod_buffer.size());
      const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &audio_info)};

      // No AUTOCONNECT: the node is a source that recording applications link to, not a playback stream.
      const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
      if (pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
        BOOST_LOG(error) << "Couldn't connect PipeWire virtual microphone stream"sv;
        return -1;
      }

      if (pw_thread_loop_start(loop) < 0) {
        BOOST_LOG(error) << "Couldn't start PipeWire thread loop for the virtual microphone"sv;
        return -1;
      }

      pw_thread_loop_lock(loop);
      while (state == PW_STREAM_STATE_CONNECTING || state == PW_STREAM_STATE_UNCONNECTED) {
        if (pw_thread_loop_timed_wait(loop, 5) != 0) {
          break;
        }
      }
      const auto final_state = state;
      pw_thread_loop_unlock(loop);

      if (final_state != PW_STREAM_STATE_PAUSED && final_state != PW_STREAM_STATE_STREAMING) {
        BOOST_LOG(error) << "PipeWire virtual microphone failed to start: "sv << pw_stream_state_as_string(final_state);
        return -1;
      }

      BOOST_LOG(info) << "Created virtual microphone [Sunshine Microphone]"sv;
      return 0;
    }

    int write(const std::int16_t *samples, std::size_t frame_count) override {
      if (failed.load(std::memory_order_relaxed)) {
        return -1;
      }

      const auto bytes = static_cast<std::uint32_t>(frame_count * sample_size);
      std::uint32_t index;
      const auto filled = spa_ringbuffer_get_write_index(&ring, &index);
      if (filled < 0 || static_cast<std::uint32_t>(filled) + bytes > ring_size) {
        return 0;
      }

      spa_ringbuffer_write_data(&ring, ring_data.data(), ring_size, index & ring_mask, samples, bytes);
      spa_ringbuffer_write_update(&ring, index + bytes);
      return static_cast<int>(frame_count);
    }

  private:
    static void on_state_changed(void *userdata, pw_stream_state, pw_stream_state new_state, const char *error_message) {
      auto self = static_cast<pipewire_mic_t *>(userdata);
      self->state = new_state;
      if (new_state == PW_STREAM_STATE_ERROR) {
        BOOST_LOG(error) << "PipeWire virtual microphone error: "sv << (error_message ? error_message : "unknown");
        self->failed.store(true, std::memory_order_relaxed);
      }
      pw_thread_loop_signal(self->loop, false);
    }

    /**
     * @brief Fill one graph cycle from the ring. Runs on the PipeWire realtime thread.
     */
    static void on_process(void *userdata) {
      auto self = static_cast<pipewire_mic_t *>(userdata);

      auto pw_buf = pw_stream_dequeue_buffer(self->stream);
      if (!pw_buf) {
        return;
      }

      auto &data = pw_buf->buffer->datas[0];
      auto dst = static_cast<std::uint8_t *>(data.data);
      if (!dst) {
        pw_stream_queue_buffer(self->stream, pw_buf);
        return;
      }

      auto wanted_bytes = data.maxsize - data.maxsize % sample_size;
      if (pw_buf->requested) {
        wanted_bytes = std::min<std::uint32_t>(wanted_bytes, pw_buf->requested * sample_size);
      }

      std::uint32_t index;
      auto available = static_cast<std::uint32_t>(std::max(spa_ringbuffer_get_read_index(&self->ring, &index), 0));

      if (available > max_queued_bytes) {
        // Drop stale audio so the microphone does not lag behind the client.
        const auto skip = available - prebuffer_bytes;
        index += skip;
        available -= skip;
        spa_ringbuffer_read_update(&self->ring, index);
      }

      if (!self->primed && available >= wanted_bytes + prebuffer_bytes) {
        self->primed = true;
      }

      std::uint32_t read_bytes = 0;
      if (self->primed) {
        read_bytes = std::min(available, wanted_bytes);
        spa_ringbuffer_read_data(&self->ring, self->ring_data.data(), ring_size, index & ring_mask, dst, read_bytes);
        spa_ringbuffer_read_update(&self->ring, index + read_bytes);

        // Underrun: play what is left and wait for the prebuffer to refill.
        if (read_bytes < wanted_bytes) {
          self->primed = false;
        }
      }
      std::memset(dst + read_bytes, 0, wanted_bytes - read_bytes);

      data.chunk->offset = 0;
      data.chunk->stride = sample_size;
      data.chunk->size = wanted_bytes;
      pw_stream_queue_buffer(self->stream, pw_buf);
    }

    pw_thread_loop *loop {nullptr};
    pw_stream *stream {nullptr};
    pw_stream_state state {PW_STREAM_STATE_UNCONNECTED};  ///< Written on the PipeWire thread under the loop lock.
    std::atomic_bool failed {false};

    spa_ringbuffer ring {};  ///< Zeroed, equivalent to SPA_RINGBUFFER_INIT().
    std::array<std::uint8_t, ring_size> ring_data {};
    bool primed {false};  ///< Only touched by the realtime thread.
  };
}  // namespace

namespace platf {
  std::unique_ptr<virtual_mic_t> virtual_mic() {
    auto mic = std::make_unique<pipewire_mic_t>();
    if (mic->init()) {
      return nullptr;
    }
    return mic;
  }
}  // namespace platf
