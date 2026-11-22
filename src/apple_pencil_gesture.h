/**
 * @file src/apple_pencil_gesture.h
 * @brief Wire layout of the Apple Pencil double-tap and squeeze packet that VoidLink sends.
 */
#pragma once

// standard includes
#include <cstdint>

// lib includes
extern "C" {
#include <moonlight-common-c/src/Input.h>
}

namespace input::apple_pencil_gesture {
  /**
   * @brief Packet type of SS_APPLE_PENCIL_SIDE_BUTTON_PACKET in VoidLink's moonlight-common-c fork (voidlink-c).
   *
   * VoidLink sends it only after the host advertises x-ss-general.applePencilSideButtons, and only when
   * its own double-tap or squeeze shortcuts are turned off.
   */
  constexpr std::uint32_t magic = 0x5500000D;

  /**
   * @brief Gesture event types (LI_APPLE_PENCIL_* in voidlink-c).
   */
  enum event_e : std::uint8_t {
    double_tap = 0,  ///< The Pencil was double-tapped.
    squeeze_begin = 1,  ///< A Pencil Pro squeeze started.
    squeeze_change = 2,  ///< A Pencil Pro squeeze is ongoing.
    squeeze_end = 4,  ///< A Pencil Pro squeeze ended.
    squeeze_cancel = 8,  ///< A Pencil Pro squeeze was cancelled.
  };

#pragma pack(push, 1)

  /**
   * @brief One Pencil gesture.
   */
  struct packet_t {
    NV_INPUT_HEADER header;  ///< Packet header.
    std::uint8_t eventType;  ///< One of `event_e`.
    netfloat x;  ///< Normalized horizontal position of the Pencil (its hover point or last touch).
    netfloat y;  ///< Normalized vertical position of the Pencil.
  };

#pragma pack(pop)

  static_assert(sizeof(packet_t) == 17);
}  // namespace input::apple_pencil_gesture
