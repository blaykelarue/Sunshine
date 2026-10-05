/**
 * @file src/mic_mixer.h
 * @brief Declarations for the per-client microphone jitter buffer, Opus decoder and mixer.
 *
 * Adapted from the microphone redirection mixer in AlkaidLab/foundation-sunshine (GPL-3.0).
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace mic_mixer {
  using source_id_t = std::uint32_t;  ///< Identifier of one microphone source, normally the stream session ID.

  constexpr std::uint32_t sample_rate = 48000;  ///< Sample rate of decoded and mixed microphone audio.
  constexpr std::size_t frame_samples = sample_rate / 50;  ///< Mono samples in one 20 ms frame.
  constexpr std::size_t jitter_buffer_frames = 2;  ///< Frames buffered before a new timeline starts playing.
  constexpr std::size_t max_packet_size = 1400;  ///< Largest Opus payload accepted from a client.

  /**
   * @brief Diagnostic counters accumulated by the mixer.
   */
  struct stats_t {
    std::uint64_t duplicate_packets {0};  ///< Packets dropped because their slot was already filled.
    std::uint64_t late_packets {0};  ///< Packets dropped because their slot had already been played.
    std::uint64_t buffer_overflow_packets {0};  ///< Packets dropped because the per-source queue was full.
    std::uint64_t timeline_reanchors {0};  ///< Times a source timeline restarted at the current playout clock.
    std::uint64_t plc_frames {0};  ///< Frames synthesized by Opus packet loss concealment.
    std::uint64_t decode_failures {0};  ///< Frames that failed to decode.
    std::uint64_t skipped_playout_frames {0};  ///< Playout slots skipped because the host clock fell behind.
  };

  /**
   * @brief Check that a payload is a single 20 ms Opus packet.
   *
   * @param data Payload bytes.
   * @param size Payload length in bytes.
   * @return `true` when the payload parses as Opus and decodes to exactly one 20 ms frame.
   */
  bool is_valid_opus_packet(const std::uint8_t *data, std::size_t size);

  /**
   * @brief Places each source's packets on a shared playout clock and mixes them into mono PCM.
   *
   * Each source gets its own Opus decoder and sequence-number timeline. Packets are buffered
   * `jitter_buffer_frames` ahead of the playout clock, reordered by sequence number, and missing
   * frames are concealed for a short time before the source is treated as silent.
   *
   * The mixer is not synchronized; every member must be called from the same thread.
   */
  class mixer_t {
  public:
    mixer_t();
    ~mixer_t();

    mixer_t(const mixer_t &) = delete;
    mixer_t &operator=(const mixer_t &) = delete;

    /**
     * @brief Add a source with a fresh decoder. Adding an existing source is a no-op.
     *
     * @param source_id Source identifier.
     * @return `false` if the Opus decoder could not be created.
     */
    bool add_source(source_id_t source_id);

    /**
     * @brief Remove a source and drop its queued packets.
     *
     * @param source_id Source identifier.
     */
    void remove_source(source_id_t source_id);

    /**
     * @brief Remove every source and reset the playout clock.
     */
    void clear();

    /**
     * @brief Queue one Opus packet on a source's timeline.
     *
     * @param source_id Source the packet belongs to; it must have been added.
     * @param data Opus payload.
     * @param size Payload length in bytes.
     * @param sequence_number Client packet sequence number (wraps at 16 bits).
     * @param timestamp_ms Client capture timestamp in milliseconds, if the packet carries one.
     * @return `true` if the packet was queued for playout.
     */
    bool push_packet(source_id_t source_id, const std::uint8_t *data, std::size_t size, std::uint16_t sequence_number, std::optional<std::uint32_t> timestamp_ms);

    /**
     * @brief Advance the playout clock by one frame and mix every active source.
     *
     * @return The mixed 20 ms mono frame, or an empty span when no source produced audio.
     *         The span stays valid until the next call on this mixer.
     */
    std::span<const std::int16_t> mix_next_frame();

    /**
     * @brief Advance the playout clock without decoding, after the caller fell behind real time.
     *
     * @param frame_count Number of 20 ms slots to skip.
     */
    void skip_playout_frames(std::size_t frame_count);

    /**
     * @brief Return the counters accumulated since the previous call and reset them.
     */
    stats_t take_stats();

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace mic_mixer
