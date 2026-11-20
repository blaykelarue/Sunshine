/**
 * @file src/touchpad_wire.h
 * @brief Wire layout of the precision touchpad packets that VoidLink sends in its touchpad mode.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>

// lib includes
extern "C" {
#include <moonlight-common-c/src/Input.h>
}

namespace input::touchpad_wire {
  /**
   * @brief Packet type of SS_TOUCHPAD_PACKET (one contact) in VoidLink's moonlight-common-c fork (voidlink-c).
   */
  constexpr std::uint32_t magic = 0x55000009;

  /**
   * @brief Packet type of SS_TOUCHPAD_FRAME_PACKET (all contacts of one moment).
   */
  constexpr std::uint32_t frame_magic = 0x5500000A;

  /**
   * @brief Most contacts one frame packet carries.
   */
  constexpr std::size_t max_frame_contacts = 5;

  /**
   * @brief `buttonState` bit for the primary (click) button.
   */
  constexpr std::uint8_t button_primary = 0x01;

#pragma pack(push, 1)

  /**
   * @brief One touchpad contact.
   */
  struct packet_t {
    NV_INPUT_HEADER header;  ///< Packet header.
    std::uint8_t eventType;  ///< LI_TOUCH_EVENT_* value.
    std::uint8_t buttonState;  ///< Pressed buttons, see `button_primary`.
    std::uint16_t rotation;  ///< Little-endian orientation in degrees, or LI_ROT_UNKNOWN.
    std::uint32_t pointerId;  ///< Little-endian contact identifier.
    std::uint16_t deviceWidthMm;  ///< Little-endian physical width, or 0 when unknown.
    std::uint16_t deviceHeightMm;  ///< Little-endian physical height, or 0 when unknown.
    netfloat x;  ///< Normalized horizontal position.
    netfloat y;  ///< Normalized vertical position.
    netfloat pressure;  ///< Normalized pressure.
    netfloat contactAreaMajor;  ///< Normalized contact area, major axis.
    netfloat contactAreaMinor;  ///< Normalized contact area, minor axis.
  };

  /**
   * @brief One contact inside a frame packet.
   */
  struct frame_contact_t {
    std::uint8_t eventType;  ///< LI_TOUCH_EVENT_* value.
    std::uint8_t reserved;  ///< Zero.
    std::uint16_t pressure;  ///< Little-endian normalized pressure, 0..65535.
    std::uint32_t pointerId;  ///< Little-endian contact identifier.
    std::uint16_t x;  ///< Little-endian normalized horizontal position, 0..65535.
    std::uint16_t y;  ///< Little-endian normalized vertical position, 0..65535.
  };

  /**
   * @brief Every changed contact of one moment, so multi-finger gestures stay in step.
   */
  struct frame_packet_t {
    NV_INPUT_HEADER header;  ///< Packet header.
    std::uint8_t contactCount;  ///< Valid entries in `contacts`.
    std::uint8_t buttonState;  ///< Pressed buttons, see `button_primary`.
    std::uint16_t rotation;  ///< Little-endian orientation in degrees, or LI_ROT_UNKNOWN.
    std::uint16_t deviceWidthMm;  ///< Little-endian physical width, or 0 when unknown.
    std::uint16_t deviceHeightMm;  ///< Little-endian physical height, or 0 when unknown.
    frame_contact_t contacts[max_frame_contacts];  ///< Contacts; entries past `contactCount` are zero.
  };

#pragma pack(pop)

  static_assert(sizeof(packet_t) == 40);
  static_assert(sizeof(frame_contact_t) == 12);
  static_assert(sizeof(frame_packet_t) == 76);
}  // namespace input::touchpad_wire
