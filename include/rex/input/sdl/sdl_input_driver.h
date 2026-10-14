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

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/input/input_driver.h>
#include <rex/input/sdl/wheel_mapping.h>

#include <SDL3/SDL.h>

#define HID_SDL_THUMB_THRES 0x4E00
#define HID_SDL_TRIGG_THRES 0x1F
#define HID_SDL_REPEAT_DELAY 400
#define HID_SDL_REPEAT_RATE 100

namespace rex::input::sdl {

class SDLInputDriver final : public InputDriver, public rex::ui::WindowListener {
 public:
  explicit SDLInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~SDLInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;
  void OnWindowAvailable(rex::ui::Window* window) override;

 private:
  enum class RepeatState {
    Idle,       // no buttons pressed or repeating has ended
    Waiting,    // a button is held and the delay is awaited
    Repeating,  // actively repeating at a rate
  };
  struct KeystrokeState {
    uint64_t buttons;
    RepeatState repeat_state;
    // the button number that was pressed last:
    uint8_t repeat_butt_idx;
    // the last time (ms) a down (and/or repeat) event for that button was send:
    uint32_t repeat_time;
  };

  struct ControllerState {
    SDL_Gamepad* sdl;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed;
    bool is_active;
    DeviceId id;
    // Per pad rather than per guest user, so it survives reassignment.
    KeystrokeState keystroke;
  };

  // A joystick opened raw for the wheel: the steering base or a device one of
  // the pedal or button bindings names (a separate pedal set, a shifter).
  struct WheelJoystick {
    SDL_Joystick* sdl = nullptr;
    SDL_JoystickID instance_id = 0;
    std::string name;
    std::string path;
    uint16_t vendor = 0;
    uint16_t product = 0;
    // Last values logged by wheel_log_inputs.
    std::vector<int16_t> logged_axes;
    std::vector<bool> logged_buttons;
  };

  // The binding cvars, parsed again only when their text changes.
  struct WheelBindings {
    std::string throttle_text, brake_text, clutch_text, buttons_text;
    std::optional<WheelAxisBinding> throttle, brake, clutch;
    std::vector<WheelButtonBinding> buttons;
  };

  // The one guest wheel, built from the steering base plus whatever devices
  // the bindings name. It exists while the steering base is connected.
  struct WheelState {
    DeviceId id = DeviceId::kInvalid;
    SDL_JoystickID steering = 0;
    SDL_Haptic* haptic = nullptr;
    // The joystick the force feedback goes to: the steering base, or the
    // device wheel_ffb_device names.
    SDL_JoystickID haptic_source = 0;
    int constant_effect = -1;
    int spring_effect = -1;
    int16_t last_force = 0;
    int last_spring = 0;
    uint32_t haptic_features = 0;
    // The guest's last motor values, for wheel_log_inputs.
    uint16_t logged_left = 0;
    uint16_t logged_right = 0;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed = true;
    bool is_active = false;
    KeystrokeState keystroke;
  };

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

  void HandleEvent(const SDL_Event& event);
  std::unique_lock<std::mutex> DrainAndLock();
  void ProcessEventLocked(const SDL_Event& event);
  void OnControllerDeviceAddedLocked(const SDL_Event& event);
  void OnControllerDeviceRemovedLocked(const SDL_Event& event);
  void OnControllerDeviceAxisMotionLocked(const SDL_Event& event);
  void OnControllerDeviceButtonChangedLocked(const SDL_Event& event);

  // Wheel support (FB-2): raw SDL joysticks rather than gamepads, so axes keep
  // their own ranges and force feedback can use the haptic subsystem.
  bool IsSteeringCandidate(SDL_JoystickID instance_id);
  bool IsWheelPart(SDL_JoystickID instance_id);
  void OnJoystickAddedLocked(const SDL_Event& event);
  void OnJoystickRemovedLocked(const SDL_Event& event);
  WheelJoystick* FindWheelJoystick(std::string_view device_match, bool steering_default);
  void OpenWheelHapticLocked(WheelState& wheel, const WheelJoystick& joystick);
  void CloseWheelHapticLocked(WheelState& wheel);
  bool IsFfbTarget(const WheelJoystick& joystick) const;
  void CloseWheelLocked();
  void RefreshWheelBindingsLocked();
  void RefreshWheelStateLocked();
  void LogWheelInputsLocked();
  uint8_t ReadWheelPedalLocked(const std::optional<WheelAxisBinding>& binding);
  X_RESULT SetWheelVibrationLocked(const X_INPUT_VIBRATION& vibration);
  void UpdateWheelSpringLocked();

  inline uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const;
  std::optional<size_t> GetControllerIndexFromInstanceID(SDL_JoystickID instance_id);
  ControllerState* FindController(DeviceId id);
  DeviceId AllocateDeviceId();
  bool TestSDLVersion() const;
  void UpdateXCapabilities(ControllerState& state);
  void QueueControllerUpdate();

  rex::ui::Window* attached_window_ = nullptr;
  bool sdl_events_initialized_;
  bool SDL_Gamepad_initialized_;
  std::atomic<int> sdl_events_unflushed_;
  std::atomic<bool> sdl_pumpevents_queued_;
  // steady_clock nanoseconds of the last queued pump (QueueControllerUpdate).
  std::atomic<int64_t> sdl_pumpevents_last_ns_{0};
  // Appended in connection order, never re-sorted, and unbounded: the
  // assignment decides how many the guest sees.
  std::vector<ControllerState> controllers_;
  std::vector<WheelJoystick> wheel_joysticks_;
  std::optional<WheelState> wheel_;
  WheelBindings wheel_bindings_;
  bool sdl_haptic_initialized_ = false;
  std::mutex controllers_mutex_;
  std::mutex event_queue_mutex_;
  std::vector<SDL_Event> pending_events_;
  // Never rewound, so a handle held past a disconnect resolves to nothing.
  uint64_t next_device_id_ = 1;
};

}  // namespace rex::input::sdl
