/**
 * @file src/mic_redirect.cpp
 * @brief Definitions for receiving client microphone audio and playing it into a host virtual microphone.
 */
// standard includes
#include <array>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "config.h"
#include "logging.h"
#include "mic_mixer.h"
#include "mic_redirect.h"
#include "network.h"
#include "platform/common.h"
#include "utility.h"

using namespace std::literals;

namespace asio = boost::asio;
using asio::ip::udp;

namespace mic_redirect {
  namespace {
    constexpr std::uint8_t packet_type_opus = 0x61;
    constexpr auto frame_duration = 20ms;
    constexpr auto device_retry_interval = 2s;

#pragma pack(push, 1)

    /**
     * @brief Header in front of every client microphone datagram. Multi-byte fields are little-endian.
     */
    struct packet_header_t {
      std::uint8_t flags;  ///< Unused, always zero.
      std::uint8_t packet_type;  ///< `packet_type_opus` for Opus audio.
      std::uint16_t sequence_number;  ///< Per-packet sequence number.
      std::uint32_t timestamp;  ///< Client capture time in milliseconds.
      std::uint32_t ssrc;  ///< Constant stream identifier.
    };

#pragma pack(pop)

    struct session_t {
      std::uint32_t id;
      boost::asio::ip::address client_address;
      crypto::cipher::cbc_t cipher;
      std::uint32_t key_id;
    };

    struct counters_t {
      std::uint64_t accepted {0};
      std::uint64_t malformed {0};
      std::uint64_t unknown_sender {0};
      std::uint64_t rejected {0};
      std::uint64_t dropped_by_device {0};
    };
  }  // namespace

  struct receiver_t::impl_t {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work {io.get_executor()};
    udp::socket sock {io};
    asio::steady_timer mix_timer {io};
    std::jthread thread;

    // Everything below is only touched on the I/O thread.
    std::vector<session_t> sessions;
    mic_mixer::mixer_t mixer;
    std::unique_ptr<platf::virtual_mic_t> device;
    std::chrono::steady_clock::time_point next_device_attempt;
    std::chrono::steady_clock::time_point next_mix;

    udp::endpoint peer;
    std::array<std::uint8_t, 2048> recv_buffer;
    std::vector<std::uint8_t> plaintext;
    crypto::aes_t iv = crypto::aes_t(16);
    counters_t counters;

    void receive() {
      sock.async_receive_from(asio::buffer(recv_buffer), peer, [this](const boost::system::error_code &ec, std::size_t bytes) {
        if (ec == asio::error::operation_aborted) {
          return;
        }
        if (ec) {
          BOOST_LOG(debug) << "Microphone socket receive error: "sv << ec.message();
        } else {
          handle_packet(bytes);
        }
        receive();
      });
    }

    void handle_packet(std::size_t bytes) {
      if (sessions.empty()) {
        return;
      }

      if (bytes <= sizeof(packet_header_t)) {
        ++counters.malformed;
        return;
      }

      packet_header_t header;
      std::memcpy(&header, recv_buffer.data(), sizeof(header));
      if (header.packet_type != packet_type_opus) {
        ++counters.malformed;
        return;
      }

      const auto sequence_number = util::endian::little(header.sequence_number);
      const auto timestamp = util::endian::little(header.timestamp);
      const std::string_view ciphertext {reinterpret_cast<const char *>(recv_buffer.data()) + sizeof(header), bytes - sizeof(header)};

      const auto source = net::normalize_address(peer.address());
      bool known_sender = false;
      for (auto &session : sessions) {
        if (session.client_address != source) {
          continue;
        }
        known_sender = true;

        // Several sessions can share an address, so the session is identified by whose key decrypts the packet.
        const auto iv_sequence = util::endian::big<std::uint32_t>(session.key_id + sequence_number);
        std::memcpy(iv.data(), &iv_sequence, sizeof(iv_sequence));
        std::fill(iv.begin() + sizeof(iv_sequence), iv.end(), 0);

        if (session.cipher.decrypt(ciphertext, plaintext, &iv) != 0 || !mic_mixer::is_valid_opus_packet(plaintext.data(), plaintext.size())) {
          continue;
        }

        mixer.push_packet(session.id, plaintext.data(), plaintext.size(), sequence_number, timestamp);
        ++counters.accepted;
        return;
      }

      if (known_sender) {
        ++counters.rejected;
      } else {
        ++counters.unknown_sender;
      }
    }

    void ensure_device() {
      if (device || std::chrono::steady_clock::now() < next_device_attempt) {
        return;
      }

      device = platf::virtual_mic();
      if (!device) {
        next_device_attempt = std::chrono::steady_clock::now() + device_retry_interval;
      }
    }

    void schedule_mix() {
      mix_timer.expires_at(next_mix);
      mix_timer.async_wait([this](const boost::system::error_code &ec) {
        if (ec || sessions.empty()) {
          return;
        }

        // If this thread was starved, skip the missed slots instead of playing them late.
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_mix + frame_duration) {
          const auto missed = static_cast<std::size_t>((now - next_mix) / frame_duration);
          mixer.skip_playout_frames(missed);
          next_mix += frame_duration * missed;
        }

        ensure_device();
        const auto frame = mixer.mix_next_frame();
        if (device && !frame.empty()) {
          const auto written = device->write(frame.data(), frame.size());
          if (written == 0) {
            ++counters.dropped_by_device;
          } else if (written < 0) {
            BOOST_LOG(warning) << "Virtual microphone failed; recreating it"sv;
            device.reset();
            next_device_attempt = std::chrono::steady_clock::now() + device_retry_interval;
          }
        }

        next_mix += frame_duration;
        schedule_mix();
      });
    }

    void log_counters() {
      const auto mixer_stats = mixer.take_stats();
      BOOST_LOG(info) << "Microphone stats: accepted="sv << counters.accepted
                      << " rejected="sv << counters.rejected
                      << " unknown_sender="sv << counters.unknown_sender
                      << " malformed="sv << counters.malformed
                      << " late="sv << mixer_stats.late_packets
                      << " duplicate="sv << mixer_stats.duplicate_packets
                      << " overflow="sv << mixer_stats.buffer_overflow_packets
                      << " reanchors="sv << mixer_stats.timeline_reanchors
                      << " concealed="sv << mixer_stats.plc_frames
                      << " decode_failures="sv << mixer_stats.decode_failures
                      << " skipped="sv << mixer_stats.skipped_playout_frames
                      << " device_drops="sv << counters.dropped_by_device;
      counters = {};
    }
  };

  receiver_t::receiver_t(std::unique_ptr<impl_t> impl):
      impl_ {std::move(impl)} {
  }

  receiver_t::~receiver_t() {
    impl_->work.reset();
    impl_->io.stop();
    if (impl_->thread.joinable()) {
      impl_->thread.join();
    }
  }

  std::unique_ptr<receiver_t> receiver_t::start(std::uint16_t port) {
    auto impl = std::make_unique<impl_t>();

    auto address_family = net::get_effective_address_family(net::af_from_enum_string(config::sunshine.address_family));
    auto protocol = address_family == net::IPV4 ? udp::v4() : udp::v6();

    boost::system::error_code ec;
    const auto bind_addr_str = net::get_bind_address(address_family);
    const auto bind_addr = asio::ip::make_address(bind_addr_str, ec);
    if (ec) {
      BOOST_LOG(error) << "Invalid bind address for the microphone socket: "sv << bind_addr_str << " - "sv << ec.message();
      return nullptr;
    }

    impl->sock.open(protocol, ec);
    if (ec) {
      BOOST_LOG(error) << "Couldn't open microphone socket: "sv << ec.message();
      return nullptr;
    }

    impl->sock.bind(udp::endpoint(bind_addr, port), ec);
    if (ec) {
      BOOST_LOG(error) << "Couldn't bind microphone socket to port ["sv << port << "]: "sv << ec.message();
      return nullptr;
    }

    impl->receive();
    impl->thread = std::jthread {[io = &impl->io]() {
      platf::set_thread_name("mic_redirect");
      io->run();
    }};

    return std::unique_ptr<receiver_t>(new receiver_t(std::move(impl)));
  }

  void receiver_t::add_session(std::uint32_t session_id, const boost::asio::ip::address &client_address, const crypto::aes_t &key, std::uint32_t key_id) {
    asio::post(impl_->io, [impl = impl_.get(), session_id, client_address = net::normalize_address(client_address), key, key_id]() {
      if (!impl->mixer.add_source(session_id)) {
        BOOST_LOG(error) << "Couldn't create a microphone decoder for session "sv << session_id;
        return;
      }

      impl->sessions.push_back({session_id, client_address, crypto::cipher::cbc_t {key, true}, key_id});
      BOOST_LOG(info) << "Microphone redirection enabled for "sv << client_address.to_string();

      if (impl->sessions.size() == 1) {
        impl->next_device_attempt = {};
        impl->ensure_device();
        impl->next_mix = std::chrono::steady_clock::now() + frame_duration;
        impl->schedule_mix();
      }
    });
  }

  void receiver_t::remove_session(std::uint32_t session_id) {
    asio::post(impl_->io, [impl = impl_.get(), session_id]() {
      const auto erased = std::erase_if(impl->sessions, [session_id](const session_t &session) {
        return session.id == session_id;
      });
      if (!erased) {
        return;
      }

      impl->mixer.remove_source(session_id);
      if (impl->sessions.empty()) {
        impl->mix_timer.cancel();
        impl->mixer.clear();
        impl->device.reset();
        impl->log_counters();
      }
    });
  }
}  // namespace mic_redirect
