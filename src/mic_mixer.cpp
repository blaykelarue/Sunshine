/**
 * @file src/mic_mixer.cpp
 * @brief Definitions for the per-client microphone jitter buffer, Opus decoder and mixer.
 *
 * Adapted from the microphone redirection mixer in AlkaidLab/foundation-sunshine (GPL-3.0).
 */
// standard includes
#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>
#include <utility>

// lib includes
#include <opus/opus.h>

// local includes
#include "mic_mixer.h"

namespace mic_mixer {
  namespace {
    constexpr int channels = 1;
    // Bounded packet capacity used to absorb short arrival bursts. The playout delay stays at jitter_buffer_frames.
    constexpr std::size_t max_buffered_packets = 8;
    // How far ahead of the playout clock a packet may land before it is treated as a discontinuity.
    constexpr std::int64_t max_future_frames = static_cast<std::int64_t>(max_buffered_packets * 2);
    // Ring slots; must exceed the furthest slot a packet can be queued at (max_future_frames + jitter_buffer_frames).
    constexpr std::size_t slot_ring_size = 32;
    static_assert(slot_ring_size > max_future_frames + jitter_buffer_frames);

    constexpr std::int64_t timestamp_discontinuity_ms = 200;
    constexpr std::size_t max_consecutive_plc_frames = jitter_buffer_frames;
    constexpr std::int64_t overflow_recovery_window_frames = 250;
    constexpr std::int64_t overflow_reanchor_cooldown_frames = 100;
    constexpr std::size_t overflow_recovery_threshold = 10;

    struct opus_decoder_deleter_t {
      void operator()(OpusDecoder *decoder) const noexcept {
        opus_decoder_destroy(decoder);
      }
    };

    using opus_decoder_t = std::unique_ptr<OpusDecoder, opus_decoder_deleter_t>;

    struct queued_packet_t {
      std::int64_t slot {-1};  ///< Playout slot this entry holds, or -1 when empty.
      std::size_t size {0};
      std::array<std::uint8_t, max_packet_size> payload;
    };

    struct source_t {
      opus_decoder_t decoder;
      std::optional<std::uint16_t> max_sequence;
      std::int64_t max_extended_sequence {0};
      std::optional<std::uint32_t> max_timestamp_ms;

      std::int64_t anchor_playout_slot {0};

      std::optional<std::uint16_t> expected_restart_sequence;
      std::array<queued_packet_t, slot_ring_size> packets;
      std::size_t queued_packets {0};
      std::array<std::int16_t, frame_samples> decode_buffer {};
      bool playout_started {false};
      std::size_t consecutive_plc_frames {0};
      std::size_t overflow_events {0};
      std::int64_t overflow_window_start_slot {-1};
      std::int64_t last_reanchor_slot {-1};

      queued_packet_t &entry(std::int64_t slot) {
        return packets[static_cast<std::size_t>(slot) % slot_ring_size];
      }

      void drop(queued_packet_t &packet) {
        packet.slot = -1;
        --queued_packets;
      }

      void drop_before(std::int64_t slot) {
        for (auto &packet : packets) {
          if (packet.slot >= 0 && packet.slot < slot) {
            drop(packet);
          }
        }
      }

      void drop_all() {
        for (auto &packet : packets) {
          packet.slot = -1;
        }
        queued_packets = 0;
      }
    };

    std::int32_t sequence_distance(std::uint16_t newer, std::uint16_t older) noexcept {
      const auto distance = static_cast<std::uint16_t>(newer - older);
      return distance < 0x8000u ? static_cast<std::int32_t>(distance) : static_cast<std::int32_t>(distance) - 0x10000;
    }

    std::int64_t timestamp_distance(std::uint32_t newer, std::uint32_t older) noexcept {
      const auto distance = static_cast<std::uint32_t>(newer - older);
      return distance < 0x80000000u ? static_cast<std::int64_t>(distance) : static_cast<std::int64_t>(distance) - 0x100000000LL;
    }

    void reset_decoder(source_t &source) {
      opus_decoder_ctl(source.decoder.get(), OPUS_RESET_STATE);
    }

    void reset_timeline(source_t &source, std::uint16_t sequence_number, std::optional<std::uint32_t> timestamp_ms, std::int64_t playout_slot) {
      reset_decoder(source);
      source.max_sequence = sequence_number;
      source.max_extended_sequence = 0;
      source.max_timestamp_ms = timestamp_ms;
      source.anchor_playout_slot = playout_slot;
      source.expected_restart_sequence.reset();
      source.drop_all();
      source.playout_started = false;
      source.consecutive_plc_frames = 0;
      source.overflow_events = 0;
      source.overflow_window_start_slot = -1;
      source.last_reanchor_slot = -1;
    }

    bool queue_packet(source_t &source, stats_t &stats, std::int64_t playout_slot, std::int64_t current_playout_slot, const std::uint8_t *data, std::size_t size) {
      auto &packet = source.entry(playout_slot);
      if (packet.slot == playout_slot) {
        ++stats.duplicate_packets;
        return false;
      }

      // Entries left over from slots that were already played are stale.
      if (packet.slot >= 0) {
        source.drop(packet);
      }

      packet.slot = playout_slot;
      packet.size = size;
      std::copy_n(data, size, packet.payload.begin());
      ++source.queued_packets;

      if (source.queued_packets <= max_buffered_packets) {
        // Overflow events stay counted within the active window even when the queue briefly drains,
        // otherwise repeated short bursts never reach the recovery threshold.
        return true;
      }

      // Live input prefers the packets closest to the playout clock, so the furthest one is dropped.
      ++stats.buffer_overflow_packets;
      if (source.overflow_events == 0) {
        source.overflow_window_start_slot = current_playout_slot;
      }
      ++source.overflow_events;

      auto furthest = std::max_element(source.packets.begin(), source.packets.end(), [](const auto &left, const auto &right) {
        return left.slot < right.slot;
      });
      const auto kept = furthest->slot != playout_slot;
      source.drop(*furthest);
      return kept;
    }

    bool decode_frame(source_t &source, const queued_packet_t *packet) {
      const auto decoded_samples = opus_decode(
        source.decoder.get(),
        packet ? packet->payload.data() : nullptr,
        packet ? static_cast<opus_int32>(packet->size) : 0,
        source.decode_buffer.data(),
        static_cast<int>(frame_samples),
        0
      );
      return decoded_samples == static_cast<int>(frame_samples);
    }
  }  // namespace

  struct mixer_t::impl_t {
    std::unordered_map<source_id_t, source_t> sources;
    std::int64_t next_playout_slot {0};
    std::array<std::int32_t, frame_samples> mix_sums {};
    std::array<std::int16_t, frame_samples> mixed {};
    stats_t stats;
  };

  mixer_t::mixer_t():
      impl_ {std::make_unique<impl_t>()} {
  }

  mixer_t::~mixer_t() = default;

  bool is_valid_opus_packet(const std::uint8_t *data, std::size_t size) {
    if (!data || size == 0 || size > max_packet_size) {
      return false;
    }
    return opus_packet_get_nb_samples(data, static_cast<opus_int32>(size), sample_rate) == static_cast<int>(frame_samples);
  }

  bool mixer_t::add_source(source_id_t source_id) {
    if (impl_->sources.contains(source_id)) {
      return true;
    }

    int error = OPUS_OK;
    opus_decoder_t decoder {opus_decoder_create(sample_rate, channels, &error)};
    if (!decoder || error != OPUS_OK) {
      return false;
    }

    auto [it, inserted] = impl_->sources.try_emplace(source_id);
    it->second.decoder = std::move(decoder);
    return true;
  }

  void mixer_t::remove_source(source_id_t source_id) {
    impl_->sources.erase(source_id);
  }

  void mixer_t::clear() {
    impl_->sources.clear();
    impl_->next_playout_slot = 0;
  }

  bool mixer_t::push_packet(source_id_t source_id, const std::uint8_t *data, std::size_t size, std::uint16_t sequence_number, std::optional<std::uint32_t> timestamp_ms) {
    auto source_it = impl_->sources.find(source_id);
    if (source_it == impl_->sources.end() || !is_valid_opus_packet(data, size)) {
      return false;
    }

    auto &source = source_it->second;
    const auto restart_slot = impl_->next_playout_slot + static_cast<std::int64_t>(jitter_buffer_frames);
    if (!source.max_sequence) {
      reset_timeline(source, sequence_number, timestamp_ms, restart_slot);
      return queue_packet(source, impl_->stats, source.anchor_playout_slot, impl_->next_playout_slot, data, size);
    }

    const auto distance = sequence_distance(sequence_number, *source.max_sequence);
    if (distance == 0) {
      ++impl_->stats.duplicate_packets;
      return false;
    }

    const auto extended_sequence = source.max_extended_sequence + distance;
    const auto target_slot = source.anchor_playout_slot + extended_sequence;

    if (distance < 0 && target_slot < impl_->next_playout_slot) {
      // Source restart detection as in RFC 3550: the first unexpected step backwards only records the
      // next expected sequence number, and a following consecutive packet confirms the sender restarted.
      // A single late packet therefore never resets the decoder.
      if (!source.expected_restart_sequence || sequence_number != *source.expected_restart_sequence) {
        source.expected_restart_sequence = static_cast<std::uint16_t>(sequence_number + 1);
        ++impl_->stats.late_packets;
        return false;
      }

      ++impl_->stats.timeline_reanchors;
      reset_timeline(source, sequence_number, timestamp_ms, restart_slot);
      return queue_packet(source, impl_->stats, source.anchor_playout_slot, impl_->next_playout_slot, data, size);
    }

    if (distance > 0) {
      bool timestamp_discontinuous = false;
      if (timestamp_ms && source.max_timestamp_ms) {
        const auto packet_time_delta = timestamp_distance(*timestamp_ms, *source.max_timestamp_ms);
        const auto timestamp_error = packet_time_delta - static_cast<std::int64_t>(distance) * 20;
        timestamp_discontinuous = packet_time_delta < 0 || timestamp_error > timestamp_discontinuity_ms || timestamp_error < -timestamp_discontinuity_ms;
      }
      const auto too_far_ahead = target_slot > impl_->next_playout_slot + max_future_frames;
      const auto inactive_timeline_expired = !source.playout_started && target_slot < impl_->next_playout_slot;

      if (timestamp_discontinuous || too_far_ahead || inactive_timeline_expired) {
        // After a client pause, clock jump, long loss burst or an expired idle timeline, do not chase
        // the old timeline; buffer again from the current host playout clock.
        ++impl_->stats.timeline_reanchors;
        reset_timeline(source, sequence_number, timestamp_ms, restart_slot);
        return queue_packet(source, impl_->stats, source.anchor_playout_slot, impl_->next_playout_slot, data, size);
      }

      source.max_sequence = sequence_number;
      source.max_extended_sequence = extended_sequence;
      source.max_timestamp_ms = timestamp_ms;
    }
    // An in-order or reordered packet that still fits an unplayed slot belongs to the current timeline,
    // so any restart candidate created by an earlier late packet is cancelled.
    source.expected_restart_sequence.reset();

    if (target_slot < impl_->next_playout_slot) {
      ++impl_->stats.late_packets;
      return false;
    }

    if (source.overflow_window_start_slot >= 0 && impl_->next_playout_slot - source.overflow_window_start_slot > overflow_recovery_window_frames) {
      source.overflow_events = 0;
      source.overflow_window_start_slot = -1;
    }

    const auto overflow_window_active = source.overflow_window_start_slot >= 0 &&
                                        impl_->next_playout_slot - source.overflow_window_start_slot <= overflow_recovery_window_frames;
    if (source.overflow_events >= overflow_recovery_threshold && overflow_window_active && (source.last_reanchor_slot < 0 || impl_->next_playout_slot - source.last_reanchor_slot > overflow_reanchor_cooldown_frames)) {
      // The client is persistently ahead of the host clock; drop the backlog and buffer again.
      ++impl_->stats.timeline_reanchors;
      reset_timeline(source, sequence_number, timestamp_ms, restart_slot);
      source.last_reanchor_slot = impl_->next_playout_slot;
      return queue_packet(source, impl_->stats, source.anchor_playout_slot, impl_->next_playout_slot, data, size);
    }

    return queue_packet(source, impl_->stats, target_slot, impl_->next_playout_slot, data, size);
  }

  std::span<const std::int16_t> mixer_t::mix_next_frame() {
    const auto playout_slot = impl_->next_playout_slot++;
    impl_->mix_sums.fill(0);
    std::int32_t source_count = 0;

    for (auto &[source_id, source] : impl_->sources) {
      source.drop_before(playout_slot);

      queued_packet_t *packet = &source.entry(playout_slot);
      if (packet->slot != playout_slot) {
        packet = nullptr;
      }

      if (!packet) {
        if (!source.playout_started) {
          continue;
        }
        if (source.consecutive_plc_frames >= max_consecutive_plc_frames) {
          // A source without data must stop counting towards the mix average, or it would lower the
          // volume of every other active client.
          reset_decoder(source);
          source.playout_started = false;
          source.consecutive_plc_frames = 0;
          continue;
        }
      }

      if (!decode_frame(source, packet)) {
        ++impl_->stats.decode_failures;
        if (packet) {
          source.drop(*packet);
        }
        reset_decoder(source);
        source.playout_started = false;
        continue;
      }

      if (packet) {
        source.drop(*packet);
        source.playout_started = true;
        source.consecutive_plc_frames = 0;
      } else {
        ++impl_->stats.plc_frames;
        ++source.consecutive_plc_frames;
      }

      ++source_count;
      for (std::size_t i = 0; i < frame_samples; ++i) {
        impl_->mix_sums[i] += source.decode_buffer[i];
      }
    }

    if (source_count == 0) {
      return {};
    }

    for (std::size_t i = 0; i < frame_samples; ++i) {
      impl_->mixed[i] = static_cast<std::int16_t>(std::clamp<std::int32_t>(impl_->mix_sums[i] / source_count, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
    }

    return impl_->mixed;
  }

  void mixer_t::skip_playout_frames(std::size_t frame_count) {
    if (frame_count == 0) {
      return;
    }

    impl_->next_playout_slot += static_cast<std::int64_t>(frame_count);
    impl_->stats.skipped_playout_frames += frame_count;
    for (auto &[source_id, source] : impl_->sources) {
      source.drop_before(impl_->next_playout_slot);
      reset_decoder(source);
      source.playout_started = false;
      source.consecutive_plc_frames = 0;
      source.overflow_events = 0;
      source.overflow_window_start_slot = -1;
      source.last_reanchor_slot = -1;
    }
  }

  stats_t mixer_t::take_stats() {
    return std::exchange(impl_->stats, {});
  }
}  // namespace mic_mixer
