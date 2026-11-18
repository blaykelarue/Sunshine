/**
 * @file src/pen_barrel_roll.h
 * @brief Wire layout of the VoidLink pen packet that carries an Apple Pencil Pro barrel roll.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>

// lib includes
extern "C" {
#include <moonlight-common-c/src/Input.h>
}

namespace input::pen_barrel_roll {
  /**
   * @brief Packet type of SS_PEN_BARREL_ROLL_PACKET in VoidLink's moonlight-common-c fork (voidlink-c).
   *
   * VoidLink sends it in place of SS_PEN_PACKET once the host advertises x-ss-general.penBarrelRoll.
   */
  constexpr std::uint32_t magic = 0x5500000B;

  /**
   * @brief Barrel roll value the client sends when the pencil cannot report one.
   */
  constexpr std::uint16_t unknown = 0xFFFF;

#pragma pack(push, 1)

  /**
   * @brief SS_PEN_PACKET followed by the barrel roll.
   */
  struct packet_t {
    SS_PEN_PACKET pen;  ///< Standard pen packet, including its header.
    std::uint16_t barrelRoll;  ///< Little-endian twist in degrees, or `unknown`.
    std::uint16_t reserved;  ///< Zero padding.
  };

#pragma pack(pop)

  static_assert(sizeof(SS_PEN_PACKET) == 36);
  static_assert(offsetof(packet_t, barrelRoll) == 36);
  static_assert(sizeof(packet_t) == 40);
}  // namespace input::pen_barrel_roll
