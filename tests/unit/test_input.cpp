/**
 * @file tests/unit/test_input.cpp
 * @brief Tests for retained stream input and virtual gamepad lifecycle behavior.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// moonlight-common-c includes
extern "C" {
#include <moonlight-common-c/src/Input.h>
}

// local includes
#include "src/apple_pencil_gesture.h"
#include "src/config.h"
#include "src/globals.h"
#include "src/input.h"
#include "src/pen_barrel_roll.h"
#include "src/platform/virtualhid_input.h"
#include "src/touchpad_wire.h"
#include "src/utility.h"

// lib includes
#include <boost/endian/conversion.hpp>

namespace {
  /**
   * @brief Protocol metadata for a fixed-size input packet.
   */
  struct input_packet_spec_t {
    std::uint32_t magic;  ///< Packet type identifier.
    std::size_t size;  ///< Complete fixed packet size.
  };

  /**
   * @brief Create raw input bytes with a caller-selected header and buffer size.
   *
   * @param magic Packet type identifier.
   * @param declared_size Packet size declared after the size field.
   * @param actual_size Number of bytes available in the packet buffer.
   * @return Raw packet bytes.
   */
  std::vector<std::uint8_t> make_input_packet(std::uint32_t magic, std::uint32_t declared_size, std::size_t actual_size) {
    std::vector<std::uint8_t> packet(actual_size);
    const NV_INPUT_HEADER header {
      util::endian::big(declared_size),
      util::endian::little(magic),
    };
    if (!packet.empty()) {
      std::memcpy(packet.data(), &header, std::min(packet.size(), sizeof(header)));
    }
    return packet;
  }

  /**
   * @brief Create a valid pen packet for one client pen event.
   *
   * @param event_type Moonlight touch event type.
   * @return Raw packet bytes.
   */
  std::vector<std::uint8_t> make_pen_packet(std::uint8_t event_type) {
    auto packet = make_input_packet(SS_PEN_MAGIC, sizeof(SS_PEN_PACKET) - sizeof(std::uint32_t), sizeof(SS_PEN_PACKET));
    packet[offsetof(SS_PEN_PACKET, eventType)] = event_type;
    return packet;
  }

  /**
   * @brief Create a valid touchpad frame packet in which every contact has the same event.
   *
   * @param event_type Moonlight touch event type for each contact.
   * @param pointer_ids Contact identifiers, at most five.
   * @param button_state Pressed touchpad buttons.
   * @return Raw packet bytes.
   */
  std::vector<std::uint8_t> make_touchpad_frame(std::uint8_t event_type, std::initializer_list<std::uint32_t> pointer_ids, std::uint8_t button_state = 0) {
    using frame_t = input::touchpad_wire::frame_packet_t;
    auto packet = make_input_packet(input::touchpad_wire::frame_magic, sizeof(frame_t) - sizeof(std::uint32_t), sizeof(frame_t));
    frame_t frame {};
    std::memcpy(&frame, packet.data(), sizeof(frame));
    frame.contactCount = static_cast<std::uint8_t>(pointer_ids.size());
    frame.buttonState = button_state;
    std::size_t i = 0;
    for (const auto id : pointer_ids) {
      frame.contacts[i].eventType = event_type;
      frame.contacts[i].pointerId = util::endian::little(id);
      frame.contacts[i].x = util::endian::little<std::uint16_t>(32768);
      ++i;
    }
    std::memcpy(packet.data(), &frame, sizeof(frame));
    return packet;
  }

  /**
   * @brief Create a valid Apple Pencil gesture packet.
   *
   * @param event_type Gesture event type.
   * @param x Normalized horizontal Pencil position.
   * @param y Normalized vertical Pencil position.
   * @return Raw packet bytes.
   */
  std::vector<std::uint8_t> make_pencil_gesture(std::uint8_t event_type, float x = 0.25F, float y = 0.5F) {
    using packet_t = input::apple_pencil_gesture::packet_t;
    auto packet = make_input_packet(input::apple_pencil_gesture::magic, sizeof(packet_t) - sizeof(std::uint32_t), sizeof(packet_t));
    packet[offsetof(packet_t, eventType)] = event_type;
    boost::endian::endian_store<float, sizeof(float), boost::endian::order::little>(packet.data() + offsetof(packet_t, x), x);
    boost::endian::endian_store<float, sizeof(float), boost::endian::order::little>(packet.data() + offsetof(packet_t, y), y);
    return packet;
  }

  /**
   * @brief Give a stream a 1000x1000 touch port, so absolute pen input reaches the virtual pen.
   *
   * @param mail Mailbox the stream input was allocated with.
   */
  void raise_square_touch_port(const safe::mail_t &mail) {
    input::touch_port_t touch_port {};
    touch_port.width = touch_port.height = touch_port.logical_width = touch_port.logical_height = 1000;
    touch_port.env_width = touch_port.env_height = touch_port.env_logical_width = touch_port.env_logical_height = 1000;
    touch_port.scalar_inv = touch_port.scalar_tpcoords = 1.0F;
    mail->event<input::touch_port_t>(mail::touch_port)->raise(touch_port);
  }

  /**
   * @brief Fixture that installs a fake global virtual input backend.
   */
  class InputGamepadSessionTest: public ::testing::Test {
  protected:
    /**
     * @brief Preserve configuration and install observable fake devices.
     */
    void SetUp() override {
      original_input_ = config::input;
      config::input.controller = true;
      config::input.gamepad = "xseries";

      auto platform_input = platf::input();
      ASSERT_TRUE(platform_input);
      auto &context = platf::virtualhid::get_input_context(platform_input);
      context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      context_ = &context;
      runtime_ = context.runtime.get();
      ASSERT_NE(runtime_, nullptr);
      input::testing::set_platform_input(std::move(platform_input));
    }

    /**
     * @brief Destroy retained test sessions and restore configuration.
     */
    void TearDown() override {
      input::terminate_gamepads();
      input::testing::set_platform_input({});
      context_ = nullptr;
      runtime_ = nullptr;
      config::input = std::move(original_input_);
    }

    /**
     * @brief Access the fake runtime installed for the current test.
     *
     * @return Fake libvirtualhid runtime.
     */
    lvh::Runtime &runtime() const {
      return *runtime_;
    }

    /**
     * @brief Access the shared libvirtualhid input context installed for the test.
     *
     * @return Fake input context.
     */
    platf::virtualhid::input_context_t &context() const {
      return *context_;
    }

  private:
    platf::virtualhid::input_context_t *context_ = nullptr;  ///< Fake input context installed in the global backend.
    lvh::Runtime *runtime_ = nullptr;  ///< Fake runtime installed in the global input backend.
    config::input_t original_input_;  ///< Input configuration restored after each test.
  };
}  // namespace

TEST(InputPacketValidationTest, RejectsEveryBufferShorterThanTheHeader) {
  for (std::size_t actual_size = 0; actual_size < sizeof(NV_INPUT_HEADER); ++actual_size) {
    const auto packet = make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t), actual_size);
    EXPECT_FALSE(input::testing::is_valid_input_packet(packet)) << "actual_size=" << actual_size;
  }
}

TEST(InputPacketValidationTest, RejectsDeclaredSizesOutsideTheAvailableBuffer) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  for (std::uint32_t declared_size = 0; declared_size < sizeof(std::uint32_t); ++declared_size) {
    const auto packet = make_input_packet(unknown_magic, declared_size, sizeof(NV_INPUT_HEADER));
    EXPECT_FALSE(input::testing::is_valid_input_packet(packet)) << "declared_size=" << declared_size;
  }

  const auto truncated_packet = make_input_packet(unknown_magic, sizeof(std::uint32_t) + 1, sizeof(NV_INPUT_HEADER));
  EXPECT_FALSE(input::testing::is_valid_input_packet(truncated_packet));

  const auto overflowing_size = make_input_packet(
    unknown_magic,
    std::numeric_limits<std::uint32_t>::max(),
    sizeof(NV_INPUT_HEADER)
  );
  EXPECT_FALSE(input::testing::is_valid_input_packet(overflowing_size));
}

TEST(InputPacketValidationTest, ValidatesUnicodePacketBoundsWithoutOverflow) {
  const auto empty_packet = make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER));
  EXPECT_TRUE(input::testing::is_valid_input_packet(empty_packet));

  constexpr std::uint32_t maximum_declared_size = sizeof(std::uint32_t) + UTF8_TEXT_EVENT_MAX_COUNT;
  const auto maximum_packet = make_input_packet(
    UTF8_TEXT_EVENT_MAGIC,
    maximum_declared_size,
    sizeof(std::uint32_t) + maximum_declared_size
  );
  EXPECT_TRUE(input::testing::is_valid_input_packet(maximum_packet));

  const auto oversized_text = make_input_packet(
    UTF8_TEXT_EVENT_MAGIC,
    maximum_declared_size + 1,
    sizeof(std::uint32_t) + maximum_declared_size + 1
  );
  EXPECT_FALSE(input::testing::is_valid_input_packet(oversized_text));

  const auto truncated_text = make_input_packet(UTF8_TEXT_EVENT_MAGIC, maximum_declared_size, sizeof(NV_INPUT_HEADER));
  EXPECT_FALSE(input::testing::is_valid_input_packet(truncated_text));
}

TEST(InputPacketValidationTest, EnforcesEveryFixedPacketSize) {
  constexpr std::array packet_specs {
    input_packet_spec_t {MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET)},
    input_packet_spec_t {MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET)},
    input_packet_spec_t {SCROLL_MAGIC_GEN5, sizeof(NV_SCROLL_PACKET)},
    input_packet_spec_t {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET)},
    input_packet_spec_t {KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET)},
    input_packet_spec_t {KEY_UP_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET)},
    input_packet_spec_t {MULTI_CONTROLLER_MAGIC_GEN5, sizeof(NV_MULTI_CONTROLLER_PACKET)},
    input_packet_spec_t {SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET)},
    input_packet_spec_t {SS_PEN_MAGIC, sizeof(SS_PEN_PACKET)},
    input_packet_spec_t {input::pen_barrel_roll::magic, sizeof(input::pen_barrel_roll::packet_t)},
    input_packet_spec_t {input::touchpad_wire::magic, sizeof(input::touchpad_wire::packet_t)},
    input_packet_spec_t {input::touchpad_wire::frame_magic, sizeof(input::touchpad_wire::frame_packet_t)},
    input_packet_spec_t {input::apple_pencil_gesture::magic, sizeof(input::apple_pencil_gesture::packet_t)},
    input_packet_spec_t {SS_CONTROLLER_ARRIVAL_MAGIC, sizeof(SS_CONTROLLER_ARRIVAL_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_TOUCH_MAGIC, sizeof(SS_CONTROLLER_TOUCH_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_MOTION_MAGIC, sizeof(SS_CONTROLLER_MOTION_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_BATTERY_MAGIC, sizeof(SS_CONTROLLER_BATTERY_PACKET)},
  };

  for (const auto &[magic, size] : packet_specs) {
    const auto declared_size = static_cast<std::uint32_t>(size - sizeof(std::uint32_t));
    const auto valid_packet = make_input_packet(magic, declared_size, size);
    EXPECT_TRUE(input::testing::is_valid_input_packet(valid_packet)) << "magic=" << magic;

    const auto trailing_bytes = make_input_packet(magic, declared_size, size + 8);
    EXPECT_TRUE(input::testing::is_valid_input_packet(trailing_bytes)) << "magic=" << magic;

    const auto wrong_declared_size = make_input_packet(magic, declared_size - 1, size);
    EXPECT_FALSE(input::testing::is_valid_input_packet(wrong_declared_size)) << "magic=" << magic;

    const auto oversized_declared_size = make_input_packet(magic, declared_size + 1, size + 1);
    EXPECT_FALSE(input::testing::is_valid_input_packet(oversized_declared_size)) << "magic=" << magic;

    const auto truncated_packet = make_input_packet(magic, declared_size, size - 1);
    EXPECT_FALSE(input::testing::is_valid_input_packet(truncated_packet)) << "magic=" << magic;
  }
}

TEST(InputPacketValidationTest, PreservesUnknownPacketHandlingWithinDeclaredBounds) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  const auto packet = make_input_packet(unknown_magic, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER));
  EXPECT_TRUE(input::testing::is_valid_input_packet(packet));
}

TEST_F(InputGamepadSessionTest, RejectsMalformedBatchablePacketsAtQueueIngress) {
  ASSERT_FALSE(task_pool.running());
  const std::shared_ptr<input::input_t> empty_input;
  EXPECT_EQ(input::testing::queued_input_packet_count(empty_input), 0);

  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "packet-validation-client");
  ASSERT_NE(stream_input, nullptr);

  auto short_header = make_input_packet(SS_PEN_MAGIC, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER) - 1);
  input::passthrough(stream_input, std::move(short_header));
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);

  constexpr std::array batchable_packet_specs {
    input_packet_spec_t {MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {SCROLL_MAGIC_GEN5, sizeof(NV_SCROLL_PACKET)},
    input_packet_spec_t {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET)},
    input_packet_spec_t {MULTI_CONTROLLER_MAGIC_GEN5, sizeof(NV_MULTI_CONTROLLER_PACKET)},
    input_packet_spec_t {SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET)},
    input_packet_spec_t {SS_PEN_MAGIC, sizeof(SS_PEN_PACKET)},
    input_packet_spec_t {input::pen_barrel_roll::magic, sizeof(input::pen_barrel_roll::packet_t)},
    input_packet_spec_t {input::touchpad_wire::frame_magic, sizeof(input::touchpad_wire::frame_packet_t)},
    input_packet_spec_t {SS_CONTROLLER_TOUCH_MAGIC, sizeof(SS_CONTROLLER_TOUCH_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_MOTION_MAGIC, sizeof(SS_CONTROLLER_MOTION_PACKET)},
  };

  std::size_t valid_packet_count = 0;
  for (const auto &[magic, size] : batchable_packet_specs) {
    const auto declared_size = static_cast<std::uint32_t>(size - sizeof(std::uint32_t));

    auto truncated_packet = make_input_packet(magic, declared_size, size - 1);
    input::passthrough(stream_input, std::move(truncated_packet));
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), valid_packet_count) << "magic=" << magic;

    auto valid_packet = make_input_packet(magic, declared_size, size);
    input::passthrough(stream_input, std::move(valid_packet));
    ++valid_packet_count;
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), valid_packet_count) << "magic=" << magic;

    auto oversized_declared_size = make_input_packet(magic, declared_size + 1, size + 1);
    input::passthrough(stream_input, std::move(oversized_declared_size));
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), valid_packet_count) << "magic=" << magic;
  }
}

TEST_F(InputGamepadSessionTest, KeepsQueuedPenContactSamplesAndCoalescesHover) {
  ASSERT_FALSE(task_pool.running());
  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "pen-batching-client");
  ASSERT_NE(stream_input, nullptr);

  for (int sample = 0; sample < 3; ++sample) {
    input::passthrough(stream_input, make_pen_packet(LI_TOUCH_EVENT_MOVE));
  }
  for (const std::size_t remaining : std::array<std::size_t, 3> {2, 1, 0}) {
    input::testing::process_next_input_packet(stream_input);
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), remaining);
  }

  for (int sample = 0; sample < 3; ++sample) {
    input::passthrough(stream_input, make_pen_packet(LI_TOUCH_EVENT_HOVER));
  }
  input::passthrough(stream_input, make_pen_packet(LI_TOUCH_EVENT_DOWN));
  input::testing::process_next_input_packet(stream_input);
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 1);
}

TEST_F(InputGamepadSessionTest, CoalescesOnlyMatchingTouchpadMoveFrames) {
  ASSERT_FALSE(task_pool.running());
  config::input.native_pen_touch = true;
  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "touchpad-batching-client");
  ASSERT_NE(stream_input, nullptr);

  // Queued moves of the same fingers merge, but a lift is always delivered.
  for (int sample = 0; sample < 3; ++sample) {
    input::passthrough(stream_input, make_touchpad_frame(LI_TOUCH_EVENT_MOVE, {1, 2}));
  }
  input::passthrough(stream_input, make_touchpad_frame(LI_TOUCH_EVENT_UP, {1}));
  input::testing::process_next_input_packet(stream_input);
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 1);
  input::testing::process_next_input_packet(stream_input);
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);

  // Different fingers, or a click starting, keep frames apart.
  input::passthrough(stream_input, make_touchpad_frame(LI_TOUCH_EVENT_MOVE, {1, 2}));
  input::passthrough(stream_input, make_touchpad_frame(LI_TOUCH_EVENT_MOVE, {1, 3}));
  input::passthrough(stream_input, make_touchpad_frame(LI_TOUCH_EVENT_MOVE, {1, 3}, input::touchpad_wire::button_primary));
  for (const std::size_t remaining : std::array<std::size_t, 3> {2, 1, 0}) {
    input::testing::process_next_input_packet(stream_input);
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), remaining);
  }
}

TEST_F(InputGamepadSessionTest, PencilDoubleTapClicksAPenSideButtonAtThePencil) {
  ASSERT_FALSE(task_pool.running());
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto stream_input = input::alloc(mail, "pencil-double-tap-client");
  ASSERT_NE(stream_input, nullptr);
  raise_square_touch_port(mail);
  auto &client = platf::virtualhid::get_client_context(input::testing::client_input(stream_input));
  ASSERT_NE(client.pen, nullptr);

  // Ignored while disabled
  config::input.apple_pencil_gesture = "disabled";
  input::passthrough(stream_input, make_pencil_gesture(input::apple_pencil_gesture::double_tap));
  input::testing::process_next_input_packet(stream_input);
  EXPECT_EQ(client.pen->submit_count(), 0U);

  // The out-of-range pen hovers at the Pencil, clicks its first side button, then leaves again
  config::input.apple_pencil_gesture = "pen_button_1";
  input::passthrough(stream_input, make_pencil_gesture(input::apple_pencil_gesture::double_tap, 0.25F, 0.5F));
  input::testing::process_next_input_packet(stream_input);
  EXPECT_EQ(client.pen->submit_count(), 6U);  // hover, press + move, release + move, leave
  EXPECT_TRUE(client.pressed_pen_buttons.empty());
  const auto last = client.pen->last_submitted_tool();
  EXPECT_EQ(last.transition, lvh::PointerTransition::leave);
  EXPECT_FLOAT_EQ(last.x, 0.25F);
  EXPECT_FLOAT_EQ(last.y, 0.5F);
}

TEST_F(InputGamepadSessionTest, PencilSqueezeHoldsAPenSideButtonThroughRealPenInput) {
  ASSERT_FALSE(task_pool.running());
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto stream_input = input::alloc(mail, "pencil-squeeze-client");
  ASSERT_NE(stream_input, nullptr);
  raise_square_touch_port(mail);
  auto &client = platf::virtualhid::get_client_context(input::testing::client_input(stream_input));
  config::input.apple_pencil_gesture = "pen_button_2";

  input::passthrough(stream_input, make_pencil_gesture(input::apple_pencil_gesture::squeeze_begin));
  input::testing::process_next_input_packet(stream_input);
  EXPECT_EQ(client.pressed_pen_buttons, (std::set<lvh::PenButton> {lvh::PenButton::secondary}));

  // Real pen packets carry no buttons, but the squeeze keeps the side button down
  input::passthrough(stream_input, make_pen_packet(LI_TOUCH_EVENT_HOVER));
  input::testing::process_next_input_packet(stream_input);
  EXPECT_EQ(client.pressed_pen_buttons, (std::set<lvh::PenButton> {lvh::PenButton::secondary}));

  // A squeeze change does nothing; the end releases the button and leaves the real pen in range
  input::passthrough(stream_input, make_pencil_gesture(input::apple_pencil_gesture::squeeze_change));
  input::passthrough(stream_input, make_pencil_gesture(input::apple_pencil_gesture::squeeze_end));
  input::testing::process_next_input_packet(stream_input);
  input::testing::process_next_input_packet(stream_input);
  EXPECT_TRUE(client.pressed_pen_buttons.empty());
  EXPECT_EQ(client.pen->last_submitted_tool().transition, lvh::PointerTransition::update);
}

TEST_F(InputGamepadSessionTest, PencilGesturesPressTheConfiguredKeyCombo) {
  ASSERT_FALSE(task_pool.running());
  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "pencil-key-client");
  ASSERT_NE(stream_input, nullptr);
  std::vector<input::testing::keyboard_event_t> keys;
  input::testing::set_keyboard_sink([&keys](const auto &event) {
    keys.push_back(event);
  });
  config::input.keybindings.clear();
  config::input.key_repeat_delay = std::chrono::milliseconds::zero();  // a held squeeze must not schedule key repeats
  config::input.apple_pencil_gesture = "key";
  config::input.apple_pencil_gesture_keys = {0x11, 0x5A};

  // A double-tap presses the combo in order and releases it in reverse
  input::passthrough(stream_input, make_pencil_gesture(input::apple_pencil_gesture::double_tap));
  input::testing::process_next_input_packet(stream_input);
  const auto summary = [&keys] {
    std::vector<std::pair<std::uint16_t, bool>> out;
    for (const auto &key : keys) {
      out.emplace_back(key.key_code, key.release);
    }
    keys.clear();
    return out;
  };
  EXPECT_EQ(summary(), (std::vector<std::pair<std::uint16_t, bool>> {{0x11, false}, {0x5A, false}, {0x5A, true}, {0x11, true}}));

  // A squeeze holds the combo until it ends
  input::passthrough(stream_input, make_pencil_gesture(input::apple_pencil_gesture::squeeze_begin));
  input::testing::process_next_input_packet(stream_input);
  EXPECT_EQ(summary(), (std::vector<std::pair<std::uint16_t, bool>> {{0x11, false}, {0x5A, false}}));
  input::passthrough(stream_input, make_pencil_gesture(input::apple_pencil_gesture::squeeze_cancel));
  input::testing::process_next_input_packet(stream_input);
  EXPECT_EQ(summary(), (std::vector<std::pair<std::uint16_t, bool>> {{0x5A, true}, {0x11, true}}));

  input::testing::set_keyboard_sink({});
  input::testing::reset_keyboard_state();
}

TEST_F(InputGamepadSessionTest, AdvertisesPencilGesturesOnlyWhenTheyCanBeHandled) {
  config::input.apple_pencil_gesture = "disabled";
  EXPECT_FALSE(input::apple_pencil_gestures_enabled());

  config::input.apple_pencil_gesture = "key";
  config::input.apple_pencil_gesture_keys.clear();
  EXPECT_FALSE(input::apple_pencil_gestures_enabled());
  config::input.apple_pencil_gesture_keys = {0x45};
  EXPECT_TRUE(input::apple_pencil_gestures_enabled());
  config::input.keyboard = false;
  EXPECT_FALSE(input::apple_pencil_gestures_enabled());

  config::input.apple_pencil_gesture = "pen_button_1";
  config::input.mouse = false;
  EXPECT_FALSE(input::apple_pencil_gestures_enabled());
}

TEST_F(InputGamepadSessionTest, ReusesGamepadsAcrossPauseAndDestroysThemOnTermination) {
  const std::string session_id = "paired-client-certificate";
  auto first_mail = std::make_shared<safe::mail_raw_t>();
  auto first = input::alloc(first_mail, session_id);
  ASSERT_NE(first, nullptr);
  const auto active_devices_before_gamepad = runtime().active_device_count();

  const platf::gamepad_arrival_t metadata {LI_CTYPE_XBOX, 0, 0};
  const auto original_id = input::testing::alloc_gamepad(first, 0, metadata);
  ASSERT_GE(original_id, 0);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  const std::weak_ptr<input::input_t> paused = first;
  first.reset();
  EXPECT_FALSE(paused.expired());
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  auto resumed_mail = std::make_shared<safe::mail_raw_t>();
  auto resumed = input::alloc(resumed_mail, session_id);
  ASSERT_NE(resumed, nullptr);
  EXPECT_EQ(paused.lock(), resumed);
  EXPECT_EQ(input::testing::gamepad_id(resumed, 0), original_id);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  input::terminate_gamepads(session_id);
  EXPECT_EQ(input::testing::gamepad_id(resumed, 0), -1);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad);

  auto replacement = input::alloc(std::make_shared<safe::mail_raw_t>(), session_id);
  EXPECT_NE(replacement, resumed);
}

TEST_F(InputGamepadSessionTest, RefreshesSharedVirtualInputAfterLicenseStateChanges) {
  ASSERT_NE(context().keyboard, nullptr);
  ASSERT_NE(context().mouse, nullptr);
  const auto original_keyboard_id = context().keyboard->device_id();
  const auto original_mouse_id = context().mouse->device_id();
  const auto active_devices = runtime().active_device_count();

  input::refresh_virtual_input();

  ASSERT_NE(context().keyboard, nullptr);
  ASSERT_NE(context().mouse, nullptr);
  EXPECT_NE(context().keyboard->device_id(), original_keyboard_id);
  EXPECT_NE(context().mouse->device_id(), original_mouse_id);
  EXPECT_EQ(runtime().active_device_count(), active_devices);
}
