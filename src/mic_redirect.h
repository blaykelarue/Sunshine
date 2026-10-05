/**
 * @file src/mic_redirect.h
 * @brief Declarations for receiving client microphone audio and playing it into a host virtual microphone.
 *
 * Wire format (compatible with VoidLink and the AlkaidLab/foundation-sunshine fork): the client sends one
 * UDP datagram per 20 ms Opus frame to the port announced in the `streamid=mic` RTSP SETUP response.
 * Each datagram is a 12-byte header (flags, packet type 0x61, little-endian 16-bit sequence number,
 * little-endian 32-bit millisecond timestamp, SSRC) followed by the Opus frame encrypted with AES-128-CBC
 * and PKCS#7 padding. The key is the session's remote input key, and the IV is the big-endian 32-bit sum
 * of the session's key ID and the sequence number, zero-padded to 16 bytes.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>

// lib includes
#include <boost/asio/ip/address.hpp>

// local includes
#include "crypto.h"

namespace mic_redirect {
  /**
   * @brief Owns the microphone UDP socket, the per-client decoders and the host virtual microphone.
   *
   * All state is confined to an internal I/O thread; the public members only post work to it.
   * The virtual microphone exists while at least one session with microphone redirection is registered.
   */
  class receiver_t {
  public:
    /**
     * @brief Bind the microphone socket and start the receive thread.
     *
     * @param port UDP port to listen on.
     * @return The running receiver, or nullptr when the socket could not be bound.
     */
    static std::unique_ptr<receiver_t> start(std::uint16_t port);

    ~receiver_t();
    receiver_t(const receiver_t &) = delete;
    receiver_t &operator=(const receiver_t &) = delete;

    /**
     * @brief Accept microphone packets for a streaming session.
     *
     * @param session_id Stream session ID.
     * @param client_address Address the client connected from; packets from other addresses are ignored.
     * @param key Session remote input AES key.
     * @param key_id Session key ID (first four bytes of the remote input IV, big-endian).
     */
    void add_session(std::uint32_t session_id, const boost::asio::ip::address &client_address, const crypto::aes_t &key, std::uint32_t key_id);

    /**
     * @brief Stop accepting microphone packets for a session.
     *
     * @param session_id Stream session ID passed to add_session().
     */
    void remove_session(std::uint32_t session_id);

  private:
    struct impl_t;
    explicit receiver_t(std::unique_ptr<impl_t> impl);
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace mic_redirect
