/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/ffb_trace.h>
#include <rex/input/flags.h>
#include <rex/input/pad_remap.h>
#include <rex/input/sdl/joystick_mapping.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DEFINE_STRING(hid_mappings_file, "gamecontrollerdb.txt", "Input",
                      "Path to SDL gamecontroller mappings file");

// Racing wheels (FB-2). The guest sees one XInput wheel: steering on the left
// stick X, throttle on the right trigger, brake on the left trigger.
REXCVAR_DEFINE_BOOL(wheel_enabled, true, "Input",
                    "Use racing wheels as an XInput wheel (subtype 2) instead of a gamepad");
REXCVAR_DEFINE_STRING(wheel_device, "", "Input",
                      "The wheel base: empty for the first device SDL reports as a wheel, "
                      "\"vvvv:pppp\" (USB IDs in hex) or part of its name");
REXCVAR_DEFINE_INT32(wheel_steering_axis, 0, "Input", "Steering axis on the wheel base")
    .range(0, 15)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(wheel_steering_invert, false, "Input", "Reverse the steering axis")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_steering_scale, 100, "Input",
                     "Steering sensitivity in percent; 200 reaches full lock at half the "
                     "wheel's rotation")
    .range(10, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_steering_deadzone, 0, "Input",
                     "Steering deadzone in percent of half travel")
    .range(0, 50)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_throttle, "", "Input",
                      "Throttle axis: \"[device|][+|-]axisN\"; device as in wheel_device "
                      "(empty: the wheel base), + or - forces the pedal direction, which is "
                      "otherwise detected from its resting value")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_brake, "", "Input", "Brake axis, as wheel_throttle")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_clutch, "", "Input",
                      "Clutch axis, as wheel_throttle; past half travel it presses "
                      "wheel_clutch_button")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_clutch_button, "LB", "Input",
                      "Pad button the clutch pedal presses (the title's clutch is LB)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_pedal_deadzone, 3, "Input", "Pedal deadzone in percent of travel")
    .range(0, 50)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_buttons, "UP=h0.up,DOWN=h0.down,LEFT=h0.left,RIGHT=h0.right",
                      "Input",
                      "Wheel buttons as pad controls: \"NAME=[device|]N\" pairs separated by "
                      "commas, N a button number or hN.up/down/left/right for a hat; names as "
                      "in pad_remap (A, B, X, Y, LB, RB, LT, RT, LS, RS, BACK, START, UP, ...)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_ffb_mode, "difference", "Input",
                      "Force feedback: difference (the title's two motor values as a signed "
                      "constant force, right minus left), left-signed or right-signed (one "
                      "motor alone, 0x8000 = no force), rumble, or off")
    .allowed({"difference", "left-signed", "right-signed", "rumble", "off"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_ffb_strength, 25, "Input",
                     "Force feedback strength in percent of what the title asks for")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(wheel_ffb_invert, false, "Input", "Reverse the force feedback direction")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_ffb_centering, 0, "Input",
                     "Host centring spring in percent, added to the title's forces")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(wheel_log_inputs, false, "Input",
                    "Log wheel axis, button and hat changes and the title's motor values, to "
                    "find the numbers for the wheel bindings")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::input::sdl {

namespace {

// SDL clamps to SDL_MAX_RUMBLE_DURATION_MS, which is not a public constant.
constexpr uint32_t kRumbleDurationMs = 0xFFFF;

}  // namespace

SDLInputDriver::SDLInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      sdl_events_initialized_(false),
      SDL_Gamepad_initialized_(false),
      sdl_events_unflushed_(0),
      sdl_pumpevents_queued_(false),
      controllers_(),
      controllers_mutex_() {}

SDLInputDriver::~SDLInputDriver() {}

X_STATUS SDLInputDriver::Setup() {
  if (!TestSDLVersion()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

void SDLInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window && !attached_window_) {
    attached_window_ = window;
    window->AddListener(this);
    window->app_context().CallInUIThreadSynchronous([this]() {
      // Initialize SDL events subsystem
      if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        REXLOG_ERROR("SDL: Failed to init events subsystem: {}", SDL_GetError());
        return;
      }
      sdl_events_initialized_ = true;
      pending_events_.reserve(64);

      // With an event watch we will always get notified, even if the event queue
      // is full, which can happen if another subsystem does not clear its events.
      SDL_AddEventWatch(
          [](void* userdata, SDL_Event* event) -> bool {
            if (!userdata || !event) {
              assert_always();
              return false;
            }

            const auto type = event->type;
            if (type < SDL_EVENT_JOYSTICK_AXIS_MOTION || type >= SDL_EVENT_FINGER_DOWN) {
              return false;
            }

            // If another part of rex uses another SDL subsystem that generates
            // events, this may seem like a bad idea. They will however not
            // subscribe to controller events so we get away with that.
            const auto driver = static_cast<SDLInputDriver*>(userdata);
            driver->HandleEvent(*event);

            return false;
          },
          this);

      // Initialize game controller subsystem
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        REXLOG_ERROR("SDL: Failed to init gamecontroller subsystem: {}", SDL_GetError());
        return;
      }
      SDL_Gamepad_initialized_ = true;

      // Force feedback for wheels. Optional: without it wheels still steer.
      if (SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
        sdl_haptic_initialized_ = true;
      } else {
        REXLOG_WARN("SDL: haptic subsystem unavailable, wheels get no force feedback: {}",
                    SDL_GetError());
      }

      // Load custom controller mappings if available
      if (!REXCVAR_GET(hid_mappings_file).empty()) {
        std::filesystem::path mappings_path(REXCVAR_GET(hid_mappings_file));
        if (!std::filesystem::exists(mappings_path)) {
          REXLOG_WARN("SDL GameControllerDB: file '{}' does not exist.",
                      REXCVAR_GET(hid_mappings_file));
        } else {
          auto mappings_result =
              SDL_AddGamepadMappingsFromFile(REXCVAR_GET(hid_mappings_file).c_str());
          if (mappings_result < 0) {
            REXLOG_ERROR("SDL GameControllerDB: error loading file '{}': {}.",
                         REXCVAR_GET(hid_mappings_file), mappings_result);
          } else {
            REXLOG_INFO("SDL GameControllerDB: loaded {} mappings.", mappings_result);
          }
        }
      }
      // The player's own mappings (the mapping assistant) replace the
      // database's for the same controller.
      const std::string user_mappings = REXCVAR_GET(hid_user_mappings_file);
      if (!user_mappings.empty() && std::filesystem::exists(rex::to_path(user_mappings))) {
        const int loaded = SDL_AddGamepadMappingsFromFile(user_mappings.c_str());
        if (loaded < 0) {
          REXLOG_ERROR("SDL: error loading user mappings '{}': {}", user_mappings, SDL_GetError());
        } else {
          REXLOG_INFO("SDL: loaded {} user mappings.", loaded);
        }
      }
      REXLOG_INFO("SDL input driver initialized successfully");
    });
  }
}

void SDLInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    attached_window_->RemoveListener(this);
    if (sdl_pumpevents_queued_) {
      attached_window_->app_context().CallInUIThreadSynchronous(
          [this]() { attached_window_->app_context().ExecutePendingFunctionsFromUIThread(); });
    }
    for (auto& controller : controllers_) {
      SDL_CloseGamepad(controller.sdl);
    }
    controllers_.clear();
    CloseWheelLocked();
    for (auto& joystick : wheel_joysticks_) {
      SDL_CloseJoystick(joystick.sdl);
    }
    wheel_joysticks_.clear();
    if (sdl_haptic_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_HAPTIC);
      sdl_haptic_initialized_ = false;
    }
    if (SDL_Gamepad_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
      SDL_Gamepad_initialized_ = false;
    }
    if (sdl_events_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      sdl_events_initialized_ = false;
    }
    attached_window_ = nullptr;
  }
}

void SDLInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {}

void SDLInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {}

void SDLInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  // Polling can start before the window exists and the subsystems are up.
  if (!sdl_events_initialized_ || !SDL_Gamepad_initialized_) {
    return;
  }

  auto guard = DrainAndLock();

  for (const auto& controller : controllers_) {
    DeviceInfo info;
    info.id = controller.id;
    const char* name = SDL_GetGamepadName(controller.sdl);
    info.name = name ? name : "";
    char guid_text[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(controller.sdl)), guid_text,
                     static_cast<int>(sizeof(guid_text)));
    info.guid = guid_text;
    info.synthetic = false;
    out.push_back(info);
  }

  if (wheel_) {
    DeviceInfo info;
    info.id = wheel_->id;
    for (const auto& joystick : wheel_joysticks_) {
      if (joystick.instance_id == wheel_->steering) {
        info.name = joystick.name;
        char guid_text[33] = {};
        SDL_GUIDToString(SDL_GetJoystickGUID(joystick.sdl), guid_text,
                         static_cast<int>(sizeof(guid_text)));
        info.guid = guid_text;
      }
    }
    info.synthetic = false;
    out.push_back(info);
  }
}

X_RESULT SDLInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (!out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    if (wheel_ && wheel_->id == id) {
      std::memcpy(out_caps, &wheel_->caps, sizeof(*out_caps));
      return X_ERROR_SUCCESS;
    }
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Unfortunately drivers can't present all information immediately (e.g.
  // battery information) so this needs to be refreshed every time.
  UpdateXCapabilities(*controller);

  std::memcpy(out_caps, &controller->caps, sizeof(*out_caps));

  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller && wheel_ && wheel_->id == id) {
    RefreshWheelStateLocked();
    WheelState& wheel = *wheel_;
    if ((is_active != wheel.is_active) || (is_active && wheel.state_changed)) {
      wheel.state.packet_number++;
      wheel.is_active = is_active;
      wheel.state_changed = false;
    }
    std::memcpy(out_state, &wheel.state, sizeof(*out_state));
    if (!is_active) {
      std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
    }
    return X_ERROR_SUCCESS;
  }
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Make sure packet_number is only incremented by 1, even if there have been
  // multiple updates between GetState calls. Also track `is_active` to
  // increment the packet number if it changed.
  if ((is_active != controller->is_active) || (is_active && controller->state_changed)) {
    controller->state.packet_number++;
    controller->is_active = is_active;
    controller->state_changed = false;
  }
  std::memcpy(out_state, &controller->state, sizeof(*out_state));
  if (!is_active) {
    // Simulate an "untouched" controller. When we become active again the
    // pressed buttons aren't lost and will be visible again.
    std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller && wheel_ && wheel_->id == id) {
    return vibration ? SetWheelVibrationLocked(*vibration) : X_ERROR_SUCCESS;
  }
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // XInput vibration holds until the guest changes it, but SDL rumble expires,
  // and a zero duration expires on the next SDL_UpdateJoysticks. Arm it for
  // SDL's maximum instead; each call cancels the previous effect anyway.
  return SDL_RumbleGamepad(controller->sdl, vibration->left_motor_speed,
                           vibration->right_motor_speed, kRumbleDurationMs)
             ? X_ERROR_SUCCESS
             : X_ERROR_FUNCTION_FAILED;
}

X_RESULT SDLInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  // TODO(JoelLinn): Figure out the flags
  // https://github.com/evilC/UCR/blob/0489929e2a8e39caa3484c67f3993d3fba39e46f/Libraries/XInput.ahk#L85-L98
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  // The order of this list is also the order in which events are send if
  // multiple buttons change at once.
  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
      // 00 - True buttons from xinput button field
      rex::ui::VirtualKey::kXInputPadDpadUp,
      rex::ui::VirtualKey::kXInputPadDpadDown,
      rex::ui::VirtualKey::kXInputPadDpadLeft,
      rex::ui::VirtualKey::kXInputPadDpadRight,
      rex::ui::VirtualKey::kXInputPadStart,
      rex::ui::VirtualKey::kXInputPadBack,
      rex::ui::VirtualKey::kXInputPadLThumbPress,
      rex::ui::VirtualKey::kXInputPadRThumbPress,
      rex::ui::VirtualKey::kXInputPadLShoulder,
      rex::ui::VirtualKey::kXInputPadRShoulder,
      rex::ui::VirtualKey::kNone, /* Guide has no VK */
      rex::ui::VirtualKey::kNone, /* Unknown */
      rex::ui::VirtualKey::kXInputPadA,
      rex::ui::VirtualKey::kXInputPadB,
      rex::ui::VirtualKey::kXInputPadX,
      rex::ui::VirtualKey::kXInputPadY,
      // 16 - Fake buttons generated from analog inputs
      rex::ui::VirtualKey::kXInputPadLTrigger,
      rex::ui::VirtualKey::kXInputPadRTrigger,
      // 18
      rex::ui::VirtualKey::kXInputPadLThumbUp,
      rex::ui::VirtualKey::kXInputPadLThumbDown,
      rex::ui::VirtualKey::kXInputPadLThumbRight,
      rex::ui::VirtualKey::kXInputPadLThumbLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
      // 26
      rex::ui::VirtualKey::kXInputPadRThumbUp,
      rex::ui::VirtualKey::kXInputPadRThumbDown,
      rex::ui::VirtualKey::kXInputPadRThumbRight,
      rex::ui::VirtualKey::kXInputPadRThumbLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
  };

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  const X_INPUT_GAMEPAD* gamepad = nullptr;
  KeystrokeState* last_state = nullptr;
  if (controller) {
    gamepad = &controller->state.gamepad;
    last_state = &controller->keystroke;
  } else if (wheel_ && wheel_->id == id) {
    RefreshWheelStateLocked();
    gamepad = &wheel_->state.gamepad;
    last_state = &wheel_->keystroke;
  } else {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // If input is not active (e.g. due to a dialog overlay), force buttons to
  // "unpressed". The algorithm will automatically send UP events when
  // 'is_active()' goes low and DOWN events when it goes high again.
  const uint64_t curr_butts =
      is_active ? (static_cast<uint64_t>(static_cast<uint16_t>(gamepad->buttons)) |
                   AnalogToKeyfield(*gamepad))
                : uint64_t(0);
  KeystrokeState& last = *last_state;

  // Handle repeating
  auto guest_now = rex::chrono::Clock::QueryGuestUptimeMillis();
  static_assert(HID_SDL_REPEAT_DELAY >= HID_SDL_REPEAT_RATE);
  if (last.repeat_state == RepeatState::Waiting &&
      (last.repeat_time + HID_SDL_REPEAT_DELAY < guest_now)) {
    last.repeat_state = RepeatState::Repeating;
  }
  if (last.repeat_state == RepeatState::Repeating &&
      (last.repeat_time + HID_SDL_REPEAT_RATE < guest_now)) {
    last.repeat_time = guest_now;
    rex::ui::VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
    assert_true(vk != rex::ui::VirtualKey::kNone);
    out_keystroke->virtual_key = uint16_t(vk);
    out_keystroke->unicode = 0;
    // InputSystem stamps the guest user this device is assigned to.
    out_keystroke->user_index = 0;
    out_keystroke->hid_code = 0;
    out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
    return X_ERROR_SUCCESS;
  }

  auto butts_changed = curr_butts ^ last.buttons;
  if (!butts_changed) {
    return X_ERROR_EMPTY;
  }

  // First try to clear buttons with up events. This is to match xinput
  // behavior when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
  // up before THUMB_LEFT is down.
  for (auto [clear_pass, pass] = std::tuple{true, 0}; pass < 2; clear_pass = false, pass++) {
    for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
      auto fbutton = uint64_t(1) << i;
      if (!(butts_changed & fbutton)) {
        continue;
      }
      rex::ui::VirtualKey vk = kVkLookup.at(i);
      if (vk == rex::ui::VirtualKey::kNone) {
        continue;
      }

      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = 0;
      out_keystroke->hid_code = 0;

      bool is_pressed = curr_butts & fbutton;
      if (clear_pass && !is_pressed) {
        // up
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
        last.buttons &= ~fbutton;
        last.repeat_state = RepeatState::Idle;
        return X_ERROR_SUCCESS;
      }
      if (!clear_pass && is_pressed) {
        // down
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
        last.buttons |= fbutton;
        last.repeat_state = RepeatState::Waiting;
        last.repeat_butt_idx = i;
        last.repeat_time = guest_now;
        return X_ERROR_SUCCESS;
      }
    }
  }
  return X_ERROR_EMPTY;
}

void SDLInputDriver::HandleEvent(const SDL_Event& event) {
  // This callback will likely run on the thread that posts the event, which
  // may be a dedicated thread SDL has created for the joystick subsystem.

  // Event queue should never be (this) full
  assert(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) < 0xFFFF);

  // The queue could grow up to 3.5MB since it is never polled.
  if (++sdl_events_unflushed_ > 64) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    sdl_events_unflushed_ = 0;
  }

  // Buffer only - no controllers_mutex_ acquisition here.
  // This breaks the lock ordering inversion between controllers_mutex_ and
  // SDL's internal joystick lock that caused deadlocks.
  std::lock_guard<std::mutex> guard(event_queue_mutex_);
  pending_events_.push_back(event);
}

std::unique_lock<std::mutex> SDLInputDriver::DrainAndLock() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> guard(event_queue_mutex_);
    events.swap(pending_events_);
  }
  std::unique_lock<std::mutex> guard(controllers_mutex_);
  for (const auto& event : events) {
    ProcessEventLocked(event);
  }
  return guard;
}

void SDLInputDriver::ProcessEventLocked(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      OnControllerDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnControllerDeviceRemovedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnControllerDeviceAxisMotionLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnControllerDeviceButtonChangedLocked(event);
      break;
    case SDL_EVENT_JOYSTICK_ADDED:
      OnJoystickAddedLocked(event);
      break;
    case SDL_EVENT_JOYSTICK_REMOVED:
      OnJoystickRemovedLocked(event);
      break;
    default:
      break;
  }
}

void SDLInputDriver::OnControllerDeviceAddedLocked(const SDL_Event& event) {
  // A wheel base, pedal set or shifter is read raw for the wheel; as a
  // gamepad it would steer with a stick-shaped axis and get plain rumble.
  if (IsWheelPart(event.gdevice.which)) {
    REXLOG_INFO("SDL: \"{}\" is used by the racing wheel, not as a gamepad.",
                SDL_GetGamepadNameForID(event.gdevice.which));
    return;
  }
  const auto controller = SDL_OpenGamepad(event.gdevice.which);
  if (!controller) {
    assert_always();
    return;
  }
  REXLOG_INFO(
      "SDL OnControllerDeviceAdded: \"{}\", "
      "JoystickType({}), "
      "GameControllerType({}), "
      "VendorID(0x{:04X}), "
      "ProductID(0x{:04X})",
      SDL_GetGamepadName(controller),
      static_cast<int>(SDL_GetJoystickType(SDL_GetGamepadJoystick(controller))),
      static_cast<int>(SDL_GetGamepadType(controller)), SDL_GetGamepadVendor(controller),
      SDL_GetGamepadProduct(controller));

  // SDL_GetGamepadPlayerIndex is deliberately ignored: on Windows it reports
  // the XInput user index the OS assigned, which is unrelated to connection
  // order and leaves non-XInput pads unnumbered.
  ControllerState state = {};
  state.sdl = controller;
  state.id = AllocateDeviceId();
  state.state_changed = true;  // XInput starts with packet_number = 1
  UpdateXCapabilities(state);
  controllers_.push_back(state);

  const int ordinal = static_cast<int>(controllers_.size()) - 1;
  SDL_SetGamepadPlayerIndex(controller, ordinal);

  REXLOG_INFO("SDL OnControllerDeviceAdded: connection order {}, device {}.", ordinal,
              static_cast<uint64_t>(state.id));
}

void SDLInputDriver::OnControllerDeviceRemovedLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (!idx) {
    REXLOG_WARN("SDL OnControllerDeviceRemoved: Ignored. Unknown device.");
    return;
  }
  SDL_CloseGamepad(controllers_.at(*idx).sdl);
  controllers_.erase(controllers_.begin() + static_cast<ptrdiff_t>(*idx));

  // LEDs only. Guest user assignment does not shift, because InputSystem holds
  // each device's ordinal.
  for (size_t i = 0; i < controllers_.size(); i++) {
    SDL_SetGamepadPlayerIndex(controllers_[i].sdl, static_cast<int>(i));
  }
  REXLOG_INFO("SDL OnControllerDeviceRemoved: {} device(s) remain.", controllers_.size());
}

void SDLInputDriver::OnControllerDeviceAxisMotionLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gaxis.which);
  if (!idx) {
    // The pad can be removed between the event being posted and drained.
    return;
  }
  auto& pad = controllers_.at(*idx).state.gamepad;
  switch (event.gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      pad.thumb_lx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFTY:
      pad.thumb_ly = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      pad.thumb_rx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      pad.thumb_ry = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      pad.left_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      pad.right_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    default:
      assert_always();
      break;
  }
  controllers_.at(*idx).state_changed = true;
}

void SDLInputDriver::OnControllerDeviceButtonChangedLocked(const SDL_Event& event) {
  // Define a lookup table to map between SDL and XInput button codes.
  // These need to be in the order of the SDL_GamepadButton enum.
  static constexpr std::array<std::underlying_type<X_INPUT_GAMEPAD_BUTTON>::type, 21>
      xbutton_lookup = {
          // Standard buttons:
          X_INPUT_GAMEPAD_A,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_BACK,
          X_INPUT_GAMEPAD_GUIDE,
          X_INPUT_GAMEPAD_START,
          X_INPUT_GAMEPAD_LEFT_THUMB,
          X_INPUT_GAMEPAD_RIGHT_THUMB,
          X_INPUT_GAMEPAD_LEFT_SHOULDER,
          X_INPUT_GAMEPAD_RIGHT_SHOULDER,
          X_INPUT_GAMEPAD_DPAD_UP,
          X_INPUT_GAMEPAD_DPAD_DOWN,
          X_INPUT_GAMEPAD_DPAD_LEFT,
          X_INPUT_GAMEPAD_DPAD_RIGHT,
          // There are additional buttons only available on some controllers.
          // For now just assign sensible defaults
          // Misc:
          X_INPUT_GAMEPAD_GUIDE,
          // Xbox Elite paddles:
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_A,
          // PS touchpad button
          X_INPUT_GAMEPAD_GUIDE,
      };
  static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0);
  static_assert(SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14);

  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (!idx) {
    // The pad can be removed between the event being posted and drained.
    return;
  }
  auto& controller = controllers_.at(*idx);

  uint16_t xbuttons = controller.state.gamepad.buttons;
  // Lookup the XInput button code.
  if (event.gbutton.button >= xbutton_lookup.size()) {
    // A newer SDL Version may have added new buttons.
    REXLOG_INFO("SDL HID: Unknown button was pressed: {}.", event.gbutton.button);
    return;
  }
  auto xbutton = xbutton_lookup.at(event.gbutton.button);
  // Pressed or released?
  if (event.gbutton.down) {
    if (xbutton == X_INPUT_GAMEPAD_GUIDE && !REXCVAR_GET(guide_button)) {
      return;
    }
    xbuttons |= xbutton;
  } else {
    xbuttons &= ~xbutton;
  }
  controller.state.gamepad.buttons = xbuttons;
  controller.state_changed = true;
}

namespace {

bool NameHas(std::string_view name, std::string_view word) {
  std::string lower(name);
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower.find(word) != std::string::npos;
}

std::string JoystickNameForID(SDL_JoystickID instance_id) {
  const char* name = SDL_GetJoystickNameForID(instance_id);
  return name ? name : "";
}

}  // namespace

bool SDLInputDriver::IsSteeringCandidate(SDL_JoystickID instance_id) {
  if (!REXCVAR_GET(wheel_enabled)) {
    return false;
  }
  const std::string name = JoystickNameForID(instance_id);
  const uint16_t vendor = SDL_GetJoystickVendorForID(instance_id);
  const uint16_t product = SDL_GetJoystickProductForID(instance_id);
  const std::string& match = REXCVAR_GET(wheel_device);
  if (!match.empty()) {
    return WheelDeviceMatches(match, name, vendor, product);
  }
  if (SDL_GetJoystickTypeForID(instance_id) == SDL_JOYSTICK_TYPE_WHEEL) {
    return true;
  }
  // Bases SDL does not type as a wheel; their pedal sets and shifters share
  // the vendor ID, so those are left to the bindings.
  return IsKnownWheelVendor(vendor) && !NameHas(name, "pedal") && !NameHas(name, "shifter") &&
         !NameHas(name, "handbrake");
}

bool SDLInputDriver::IsWheelPart(SDL_JoystickID instance_id) {
  if (!REXCVAR_GET(wheel_enabled)) {
    return false;
  }
  if (IsSteeringCandidate(instance_id)) {
    return true;
  }
  RefreshWheelBindingsLocked();
  const std::string name = JoystickNameForID(instance_id);
  const uint16_t vendor = SDL_GetJoystickVendorForID(instance_id);
  const uint16_t product = SDL_GetJoystickProductForID(instance_id);
  auto names = [&](const std::string& device) {
    return !device.empty() && WheelDeviceMatches(device, name, vendor, product);
  };
  const auto& b = wheel_bindings_;
  if ((b.throttle && names(b.throttle->device)) || (b.brake && names(b.brake->device)) ||
      (b.clutch && names(b.clutch->device))) {
    return true;
  }
  return std::any_of(b.buttons.begin(), b.buttons.end(),
                     [&](const WheelButtonBinding& button) { return names(button.device); });
}

void SDLInputDriver::OnJoystickAddedLocked(const SDL_Event& event) {
  const SDL_JoystickID which = event.jdevice.which;
  if (!IsWheelPart(which)) {
    return;
  }
  for (const auto& joystick : wheel_joysticks_) {
    if (joystick.instance_id == which) {
      return;
    }
  }
  SDL_Joystick* sdl = SDL_OpenJoystick(which);
  if (!sdl) {
    REXLOG_WARN("SDL wheel: could not open \"{}\": {}", JoystickNameForID(which), SDL_GetError());
    return;
  }
  WheelJoystick joystick;
  joystick.sdl = sdl;
  joystick.instance_id = which;
  joystick.name = JoystickNameForID(which);
  joystick.vendor = SDL_GetJoystickVendor(sdl);
  joystick.product = SDL_GetJoystickProduct(sdl);
  REXLOG_INFO("SDL wheel: opened \"{}\" ({:04x}:{:04x}), type {}, {} axes, {} buttons, {} hats.",
              joystick.name, joystick.vendor, joystick.product,
              static_cast<int>(SDL_GetJoystickType(sdl)), SDL_GetNumJoystickAxes(sdl),
              SDL_GetNumJoystickButtons(sdl), SDL_GetNumJoystickHats(sdl));
  wheel_joysticks_.push_back(std::move(joystick));

  if (wheel_ || !IsSteeringCandidate(which)) {
    return;
  }
  WheelState wheel;
  wheel.id = AllocateDeviceId();
  wheel.steering = which;
  wheel.state = {};
  wheel.keystroke = {};
  auto& c = wheel.caps;
  c = {};
  c.type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  c.sub_type = 0x02;  // XINPUT_DEVSUBTYPE_WHEEL
  // The title takes its wheel path only for a wheel that reports force
  // feedback (FEATURE_BACKLOG FB-2), so the flag is set even when the base
  // has no haptic support; its forces are then dropped.
  c.flags = X_INPUT_CAPS_FFB_SUPPORTED;
  c.gamepad.buttons = 0xF3FF;
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
  OpenWheelHapticLocked(wheel, sdl);
  wheel_ = std::move(wheel);
  REXLOG_INFO("SDL wheel: \"{}\" is the guest's racing wheel, device {}.",
              wheel_joysticks_.back().name, static_cast<uint64_t>(wheel_->id));
  if (REXCVAR_GET(wheel_throttle).empty() || REXCVAR_GET(wheel_brake).empty()) {
    REXLOG_INFO(
        "SDL wheel: set wheel_throttle and wheel_brake (and wheel_clutch, wheel_buttons) for the "
        "pedals and buttons; wheel_log_inputs prints the numbers as you press them.");
  }
}

void SDLInputDriver::OnJoystickRemovedLocked(const SDL_Event& event) {
  const SDL_JoystickID which = event.jdevice.which;
  for (size_t i = 0; i < wheel_joysticks_.size(); i++) {
    if (wheel_joysticks_[i].instance_id != which) {
      continue;
    }
    if (wheel_ && wheel_->steering == which) {
      CloseWheelLocked();
      REXLOG_INFO("SDL wheel: the wheel base was disconnected.");
    }
    SDL_CloseJoystick(wheel_joysticks_[i].sdl);
    wheel_joysticks_.erase(wheel_joysticks_.begin() + static_cast<ptrdiff_t>(i));
    return;
  }
}

SDLInputDriver::WheelJoystick* SDLInputDriver::FindWheelJoystick(std::string_view device_match,
                                                                 bool steering_default) {
  for (auto& joystick : wheel_joysticks_) {
    if (device_match.empty()) {
      if (steering_default && wheel_ && joystick.instance_id == wheel_->steering) {
        return &joystick;
      }
      continue;
    }
    if (WheelDeviceMatches(device_match, joystick.name, joystick.vendor, joystick.product)) {
      return &joystick;
    }
  }
  return nullptr;
}

void SDLInputDriver::OpenWheelHapticLocked(WheelState& wheel, SDL_Joystick* joystick) {
  if (!sdl_haptic_initialized_) {
    return;
  }
  if (!SDL_IsJoystickHaptic(joystick)) {
    REXLOG_INFO("SDL wheel: no force feedback device behind the wheel; rumble only.");
    return;
  }
  wheel.haptic = SDL_OpenHapticFromJoystick(joystick);
  if (!wheel.haptic) {
    REXLOG_WARN("SDL wheel: could not open force feedback: {}", SDL_GetError());
    return;
  }
  wheel.haptic_features = SDL_GetHapticFeatures(wheel.haptic);
  if (wheel.haptic_features & SDL_HAPTIC_GAIN) {
    SDL_SetHapticGain(wheel.haptic, 100);
  }
  REXLOG_INFO("SDL wheel: force feedback \"{}\", constant force {}, spring {}.",
              SDL_GetHapticName(wheel.haptic) ? SDL_GetHapticName(wheel.haptic) : "",
              (wheel.haptic_features & SDL_HAPTIC_CONSTANT) ? "yes" : "no",
              (wheel.haptic_features & SDL_HAPTIC_SPRING) ? "yes" : "no");
}

void SDLInputDriver::CloseWheelLocked() {
  if (!wheel_) {
    return;
  }
  if (wheel_->haptic) {
    SDL_StopHapticEffects(wheel_->haptic);
    if (wheel_->constant_effect >= 0) {
      SDL_DestroyHapticEffect(wheel_->haptic, wheel_->constant_effect);
    }
    if (wheel_->spring_effect >= 0) {
      SDL_DestroyHapticEffect(wheel_->haptic, wheel_->spring_effect);
    }
    SDL_CloseHaptic(wheel_->haptic);
  }
  wheel_.reset();
}

void SDLInputDriver::RefreshWheelBindingsLocked() {
  auto& b = wheel_bindings_;
  const std::string& throttle = REXCVAR_GET(wheel_throttle);
  if (throttle != b.throttle_text) {
    b.throttle_text = throttle;
    b.throttle = ParseWheelAxisBinding(throttle);
  }
  const std::string& brake = REXCVAR_GET(wheel_brake);
  if (brake != b.brake_text) {
    b.brake_text = brake;
    b.brake = ParseWheelAxisBinding(brake);
  }
  const std::string& clutch = REXCVAR_GET(wheel_clutch);
  if (clutch != b.clutch_text) {
    b.clutch_text = clutch;
    b.clutch = ParseWheelAxisBinding(clutch);
  }
  const std::string& buttons = REXCVAR_GET(wheel_buttons);
  if (buttons != b.buttons_text) {
    b.buttons_text = buttons;
    b.buttons = ParseWheelButtonBindings(buttons);
  }
}

uint8_t SDLInputDriver::ReadWheelPedalLocked(const std::optional<WheelAxisBinding>& binding) {
  if (!binding) {
    return 0;
  }
  WheelJoystick* joystick = FindWheelJoystick(binding->device, true);
  if (!joystick || binding->axis >= SDL_GetNumJoystickAxes(joystick->sdl)) {
    return 0;
  }
  int16_t rest = -32768;
  if (binding->direction == AxisDirection::kAuto &&
      !SDL_GetJoystickAxisInitialState(joystick->sdl, binding->axis, &rest)) {
    rest = -32768;
  }
  const AxisDirection direction = ResolveAxisDirection(binding->direction, rest);
  return PedalFromAxis(SDL_GetJoystickAxis(joystick->sdl, binding->axis), direction,
                       REXCVAR_GET(wheel_pedal_deadzone));
}

void SDLInputDriver::RefreshWheelStateLocked() {
  if (!wheel_) {
    return;
  }
  RefreshWheelBindingsLocked();
  X_INPUT_GAMEPAD pad = {};

  if (WheelJoystick* base = FindWheelJoystick({}, true)) {
    const int axis = REXCVAR_GET(wheel_steering_axis);
    if (axis < SDL_GetNumJoystickAxes(base->sdl)) {
      pad.thumb_lx = SteeringFromAxis(SDL_GetJoystickAxis(base->sdl, axis),
                                      REXCVAR_GET(wheel_steering_invert),
                                      REXCVAR_GET(wheel_steering_scale),
                                      REXCVAR_GET(wheel_steering_deadzone));
    }
  }

  const auto& b = wheel_bindings_;
  uint8_t right_trigger = ReadWheelPedalLocked(b.throttle);
  uint8_t left_trigger = ReadWheelPedalLocked(b.brake);
  uint16_t buttons = 0;
  if (ReadWheelPedalLocked(b.clutch) >= 128) {
    buttons |= PadChordButtons(REXCVAR_GET(wheel_clutch_button));
  }
  for (const auto& binding : b.buttons) {
    WheelJoystick* joystick = FindWheelJoystick(binding.device, true);
    if (!joystick) {
      continue;
    }
    bool pressed = false;
    if (binding.button >= 0) {
      pressed = binding.button < SDL_GetNumJoystickButtons(joystick->sdl) &&
                SDL_GetJoystickButton(joystick->sdl, binding.button);
    } else if (binding.hat >= 0 && binding.hat < SDL_GetNumJoystickHats(joystick->sdl)) {
      pressed = (SDL_GetJoystickHat(joystick->sdl, binding.hat) & binding.hat_mask) != 0;
    }
    if (!pressed) {
      continue;
    }
    buttons |= binding.xbuttons;
    if (binding.left_trigger) left_trigger = 0xFF;
    if (binding.right_trigger) right_trigger = 0xFF;
  }
  if (!REXCVAR_GET(guide_button)) {
    buttons &= ~uint16_t(X_INPUT_GAMEPAD_GUIDE);
  }
  pad.buttons = buttons;
  pad.left_trigger = left_trigger;
  pad.right_trigger = right_trigger;
  ffb_trace::NoteWheelInput(pad.thumb_lx, right_trigger, left_trigger, buttons);

  if (std::memcmp(&pad, &wheel_->state.gamepad, sizeof(pad)) != 0) {
    wheel_->state.gamepad = pad;
    wheel_->state_changed = true;
  }
  UpdateWheelSpringLocked();
  if (REXCVAR_GET(wheel_log_inputs)) {
    LogWheelInputsLocked();
  }
}

void SDLInputDriver::LogWheelInputsLocked() {
  for (auto& joystick : wheel_joysticks_) {
    const int axes = SDL_GetNumJoystickAxes(joystick.sdl);
    const int buttons = SDL_GetNumJoystickButtons(joystick.sdl);
    if (joystick.logged_axes.size() != size_t(std::max(axes, 0))) {
      joystick.logged_axes.assign(size_t(std::max(axes, 0)), 0);
      for (int i = 0; i < axes; i++) {
        joystick.logged_axes[size_t(i)] = SDL_GetJoystickAxis(joystick.sdl, i);
      }
      joystick.logged_buttons.assign(size_t(std::max(buttons, 0)), false);
    }
    for (int i = 0; i < axes; i++) {
      const int16_t value = SDL_GetJoystickAxis(joystick.sdl, i);
      if (std::abs(int32_t(value) - int32_t(joystick.logged_axes[size_t(i)])) >= 8192) {
        REXLOG_INFO("SDL wheel input: \"{}\" axis{} = {}", joystick.name, i, value);
        joystick.logged_axes[size_t(i)] = value;
      }
    }
    for (int i = 0; i < buttons && size_t(i) < joystick.logged_buttons.size(); i++) {
      const bool down = SDL_GetJoystickButton(joystick.sdl, i);
      if (down != joystick.logged_buttons[size_t(i)]) {
        REXLOG_INFO("SDL wheel input: \"{}\" button {} {}", joystick.name, i,
                    down ? "down" : "up");
        joystick.logged_buttons[size_t(i)] = down;
      }
    }
  }
}

X_RESULT SDLInputDriver::SetWheelVibrationLocked(const X_INPUT_VIBRATION& vibration) {
  WheelState& wheel = *wheel_;
  const uint16_t left = vibration.left_motor_speed;
  const uint16_t right = vibration.right_motor_speed;
  if (REXCVAR_GET(wheel_log_inputs) && (left != wheel.logged_left || right != wheel.logged_right)) {
    REXLOG_INFO("SDL wheel: title motors left {} right {}", left, right);
    wheel.logged_left = left;
    wheel.logged_right = right;
  }

  const WheelFfbMode mode = ParseWheelFfbMode(REXCVAR_GET(wheel_ffb_mode));
  const int strength = REXCVAR_GET(wheel_ffb_strength);
  WheelJoystick* base = FindWheelJoystick({}, true);

  if (mode == WheelFfbMode::kRumble || !(wheel.haptic_features & SDL_HAPTIC_CONSTANT)) {
    if (wheel.constant_effect >= 0 && wheel.last_force != 0) {
      SDL_StopHapticEffect(wheel.haptic, wheel.constant_effect);
      wheel.last_force = 0;
    }
    ffb_trace::NoteWheelOutput(0, mode == WheelFfbMode::kOff ? "off" : "rumble");
    if (mode == WheelFfbMode::kOff || !base) {
      return X_ERROR_SUCCESS;
    }
    // A base with neither constant force nor rumble still accepts the call.
    SDL_RumbleJoystick(base->sdl, uint16_t(uint32_t(left) * uint32_t(strength) / 100),
                       uint16_t(uint32_t(right) * uint32_t(strength) / 100), kRumbleDurationMs);
    return X_ERROR_SUCCESS;
  }

  const int16_t level =
      ConstantForceForMode(mode, left, right, strength, REXCVAR_GET(wheel_ffb_invert));
  ffb_trace::NoteWheelOutput(level, WheelFfbModeName(mode));
  if (level == wheel.last_force) {
    return X_ERROR_SUCCESS;
  }
  SDL_HapticEffect effect;
  SDL_zero(effect);
  effect.type = SDL_HAPTIC_CONSTANT;
  effect.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
  effect.constant.length = SDL_HAPTIC_INFINITY;
  effect.constant.level = level;
  if (wheel.constant_effect < 0) {
    wheel.constant_effect = SDL_CreateHapticEffect(wheel.haptic, &effect);
    if (wheel.constant_effect < 0) {
      REXLOG_WARN("SDL wheel: could not create the constant force: {}", SDL_GetError());
      wheel.haptic_features &= ~uint32_t(SDL_HAPTIC_CONSTANT);
      return X_ERROR_FUNCTION_FAILED;
    }
    SDL_RunHapticEffect(wheel.haptic, wheel.constant_effect, 1);
  } else {
    if (!SDL_UpdateHapticEffect(wheel.haptic, wheel.constant_effect, &effect)) {
      return X_ERROR_FUNCTION_FAILED;
    }
    if (wheel.last_force == 0) {
      SDL_RunHapticEffect(wheel.haptic, wheel.constant_effect, 1);
    }
  }
  if (level == 0) {
    SDL_StopHapticEffect(wheel.haptic, wheel.constant_effect);
  }
  wheel.last_force = level;
  return X_ERROR_SUCCESS;
}

void SDLInputDriver::UpdateWheelSpringLocked() {
  WheelState& wheel = *wheel_;
  if (!wheel.haptic || !(wheel.haptic_features & SDL_HAPTIC_SPRING)) {
    return;
  }
  const int percent = std::clamp(REXCVAR_GET(wheel_ffb_centering), 0, 100);
  if (percent == wheel.last_spring) {
    return;
  }
  wheel.last_spring = percent;
  if (percent == 0) {
    if (wheel.spring_effect >= 0) {
      SDL_StopHapticEffect(wheel.haptic, wheel.spring_effect);
    }
    return;
  }
  SDL_HapticEffect effect;
  SDL_zero(effect);
  effect.type = SDL_HAPTIC_SPRING;
  effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
  effect.condition.length = SDL_HAPTIC_INFINITY;
  const uint16_t saturation = uint16_t(0xFFFF * percent / 100);
  const int16_t coefficient = int16_t(0x7FFF * percent / 100);
  effect.condition.right_sat[0] = saturation;
  effect.condition.left_sat[0] = saturation;
  effect.condition.right_coeff[0] = coefficient;
  effect.condition.left_coeff[0] = coefficient;
  if (wheel.spring_effect < 0) {
    wheel.spring_effect = SDL_CreateHapticEffect(wheel.haptic, &effect);
    if (wheel.spring_effect < 0) {
      REXLOG_WARN("SDL wheel: could not create the centring spring: {}", SDL_GetError());
      wheel.haptic_features &= ~uint32_t(SDL_HAPTIC_SPRING);
      return;
    }
  } else {
    SDL_UpdateHapticEffect(wheel.haptic, wheel.spring_effect, &effect);
  }
  SDL_RunHapticEffect(wheel.haptic, wheel.spring_effect, 1);
}

std::optional<size_t> SDLInputDriver::GetControllerIndexFromInstanceID(SDL_JoystickID instance_id) {
  // Loop through our controllers and try to match the given ID.
  for (size_t i = 0; i < controllers_.size(); i++) {
    auto controller = controllers_.at(i).sdl;
    auto joystick = SDL_GetGamepadJoystick(controller);
    assert(joystick);
    auto joy_instance_id = SDL_GetJoystickID(joystick);
    assert(joy_instance_id >= 0);
    if (joy_instance_id == instance_id) {
      return i;
    }
  }
  return std::nullopt;
}

SDLInputDriver::ControllerState* SDLInputDriver::FindController(DeviceId id) {
  if (id == DeviceId::kInvalid) {
    return nullptr;
  }
  for (auto& controller : controllers_) {
    if (controller.id == id) {
      return &controller;
    }
  }
  return nullptr;
}

DeviceId SDLInputDriver::AllocateDeviceId() {
  return static_cast<DeviceId>(next_device_id_++);
}

bool SDLInputDriver::TestSDLVersion() const {
  REXLOG_DEBUG("SDL: Using version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
               SDL_MICRO_VERSION);
  return true;
}

void SDLInputDriver::UpdateXCapabilities(ControllerState& state) {
  assert(state.sdl);
  uint16_t cap_flags = 0x0;

  // The RAWINPUT driver combines and enhances input from different APIs. For
  // details, see `SDL_rawinputjoystick.c`. This correlation however has latency
  // which might confuse games calling `GetCapabilities()` (The power level is
  // only available after the controller has been "touched"). Generally that
  // should not be a problem, when in doubt disable the RAWINPUT driver via hint
  // (env var).

  if (SDL_GetJoystickConnectionState(SDL_GetGamepadJoystick(state.sdl)) ==
      SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  }

  // Check if all navigational buttons are present
  static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
      SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (auto it = nav_buttons.begin(); it < nav_buttons.end(); it++) {
    if (!SDL_GamepadHasButton(state.sdl, *it)) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
      break;
    }
  }

  auto& c = state.caps;
  c.type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  c.sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  c.flags = cap_flags;
  c.gamepad.buttons = 0xF3FF | (REXCVAR_GET(guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0x0);
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
}

void SDLInputDriver::QueueControllerUpdate() {
  // Pump SDL events to ensure controller state is up to date, at most every
  // 4 ms: the title polls from several threads many times a frame, and a pump
  // queued again as soon as the last ended kept the UI thread busy for a whole
  // core on Android, where every pump goes through the activity lifecycle
  // semaphores. 250 pumps a second still outpace any display.
  constexpr int64_t kMinimumPumpIntervalNs = 4000000;
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (now - sdl_pumpevents_last_ns_.load(std::memory_order_relaxed) < kMinimumPumpIntervalNs) {
    return;
  }
  bool is_queued = false;
  sdl_pumpevents_queued_.compare_exchange_strong(is_queued, true);
  if (!is_queued) {
    sdl_pumpevents_last_ns_.store(now, std::memory_order_relaxed);
    attached_window_->app_context().CallInUIThread([this]() {
      SDL_PumpEvents();
      sdl_pumpevents_queued_ = false;
    });
  }
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
inline uint64_t SDLInputDriver::AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > HID_SDL_TRIGG_THRES) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > HID_SDL_TRIGG_THRES) << 17;

  auto thumb_x = static_cast<int16_t>(gamepad.thumb_lx);
  auto thumb_y = static_cast<int16_t>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > HID_SDL_THUMB_THRES;
    uint64_t d = thumb_y < ~HID_SDL_THUMB_THRES;
    uint64_t r = thumb_x > HID_SDL_THUMB_THRES;
    uint64_t l = thumb_x < ~HID_SDL_THUMB_THRES;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<int16_t>(gamepad.thumb_rx);
    thumb_y = static_cast<int16_t>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace rex::input::sdl
