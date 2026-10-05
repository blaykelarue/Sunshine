/**
 * @file tests/unit/test_mic_mixer.cpp
 * @brief Tests for the microphone redirection jitter buffer and mixer.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

// lib includes
#include <opus/opus.h>

// local includes
#include <src/mic_mixer.h>

namespace {
  /**
   * @brief Encode consecutive 20 ms frames of a sine tone.
   */
  std::vector<std::vector<std::uint8_t>> encode_tone(std::size_t frame_count) {
    int error = OPUS_OK;
    auto encoder = opus_encoder_create(mic_mixer::sample_rate, 1, OPUS_APPLICATION_VOIP, &error);
    EXPECT_EQ(error, OPUS_OK);

    std::vector<std::vector<std::uint8_t>> frames;
    std::array<std::int16_t, mic_mixer::frame_samples> pcm;
    std::array<std::uint8_t, mic_mixer::max_packet_size> packet;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
      for (std::size_t i = 0; i < pcm.size(); ++i) {
        const auto t = static_cast<double>(frame * pcm.size() + i) / mic_mixer::sample_rate;
        pcm[i] = static_cast<std::int16_t>(12000 * std::sin(2 * std::numbers::pi * 440 * t));
      }
      const auto bytes = opus_encode(encoder, pcm.data(), static_cast<int>(pcm.size()), packet.data(), static_cast<opus_int32>(packet.size()));
      EXPECT_GT(bytes, 0);
      frames.emplace_back(packet.begin(), packet.begin() + bytes);
    }

    opus_encoder_destroy(encoder);
    return frames;
  }

  bool push(mic_mixer::mixer_t &mixer, const std::vector<std::uint8_t> &frame, std::uint16_t sequence_number) {
    return mixer.push_packet(1, frame.data(), frame.size(), sequence_number, std::nullopt);
  }
}  // namespace

TEST(MicMixerTests, NewSourceStartsPlayingAfterJitterBuffer) {
  const auto frames = encode_tone(1);
  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));

  ASSERT_TRUE(push(mixer, frames[0], 100));

  for (std::size_t slot = 0; slot < mic_mixer::jitter_buffer_frames; ++slot) {
    EXPECT_TRUE(mixer.mix_next_frame().empty()) << "slot " << slot;
  }
  EXPECT_EQ(mixer.mix_next_frame().size(), mic_mixer::frame_samples);
}

TEST(MicMixerTests, ReorderedPacketsAcrossSequenceWrapStayOnOneTimeline) {
  const auto frames = encode_tone(4);
  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));

  // 65534, then 0 and 65535 swapped, then 1: four consecutive frames across the 16-bit wrap.
  ASSERT_TRUE(push(mixer, frames[0], 65534));
  ASSERT_TRUE(push(mixer, frames[2], 0));
  ASSERT_TRUE(push(mixer, frames[1], 65535));
  ASSERT_TRUE(push(mixer, frames[3], 1));

  for (std::size_t slot = 0; slot < mic_mixer::jitter_buffer_frames; ++slot) {
    mixer.mix_next_frame();
  }
  for (int frame = 0; frame < 4; ++frame) {
    EXPECT_EQ(mixer.mix_next_frame().size(), mic_mixer::frame_samples) << "frame " << frame;
  }

  const auto stats = mixer.take_stats();
  EXPECT_EQ(stats.timeline_reanchors, 0);
  EXPECT_EQ(stats.late_packets, 0);
  EXPECT_EQ(stats.plc_frames, 0);
}

TEST(MicMixerTests, DuplicateAndLatePacketsAreDroppedWithoutRestartingTimeline) {
  const auto frames = encode_tone(4);
  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));

  ASSERT_TRUE(push(mixer, frames[0], 10));
  ASSERT_TRUE(push(mixer, frames[1], 11));
  EXPECT_FALSE(push(mixer, frames[1], 11));

  // Play through the jitter buffer and both frames, then deliver frame 10 again: it is late.
  for (std::size_t slot = 0; slot < mic_mixer::jitter_buffer_frames + 2; ++slot) {
    mixer.mix_next_frame();
  }
  EXPECT_FALSE(push(mixer, frames[0], 10));
  EXPECT_TRUE(push(mixer, frames[2], 12));
  EXPECT_EQ(mixer.mix_next_frame().size(), mic_mixer::frame_samples);

  const auto stats = mixer.take_stats();
  EXPECT_EQ(stats.duplicate_packets, 1);
  EXPECT_EQ(stats.late_packets, 1);
  EXPECT_EQ(stats.timeline_reanchors, 0);
}

TEST(MicMixerTests, MissingFramesAreConcealedBrieflyThenSourceGoesSilent) {
  const auto frames = encode_tone(1);
  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));

  ASSERT_TRUE(push(mixer, frames[0], 0));
  for (std::size_t slot = 0; slot < mic_mixer::jitter_buffer_frames; ++slot) {
    mixer.mix_next_frame();
  }
  ASSERT_FALSE(mixer.mix_next_frame().empty());

  // The sender stopped: a short gap is concealed, after which the source no longer contributes.
  for (std::size_t frame = 0; frame < mic_mixer::jitter_buffer_frames; ++frame) {
    EXPECT_FALSE(mixer.mix_next_frame().empty()) << "concealed frame " << frame;
  }
  EXPECT_TRUE(mixer.mix_next_frame().empty());
  EXPECT_EQ(mixer.take_stats().plc_frames, mic_mixer::jitter_buffer_frames);
}

TEST(MicMixerTests, RejectsPayloadsThatAreNotSingle20msOpusFrames) {
  const auto frames = encode_tone(1);
  EXPECT_TRUE(mic_mixer::is_valid_opus_packet(frames[0].data(), frames[0].size()));
  EXPECT_FALSE(mic_mixer::is_valid_opus_packet(frames[0].data(), 0));

  // TOC byte 0x00 = SILK narrowband, one 10 ms frame: valid Opus but the wrong duration.
  const std::array<std::uint8_t, 3> ten_ms {0x00, 0xff, 0xfe};
  EXPECT_FALSE(mic_mixer::is_valid_opus_packet(ten_ms.data(), ten_ms.size()));
}
