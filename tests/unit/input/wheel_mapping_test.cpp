#include <catch2/catch_test_macros.hpp>

#include <rex/input/sdl/wheel_mapping.h>

using namespace rex::input;
using namespace rex::input::sdl;

TEST_CASE("Wheel axis bindings parse device, sign and index", "[input][wheel]") {
  auto plain = ParseWheelAxisBinding("axis2");
  REQUIRE(plain);
  CHECK(plain->device.empty());
  CHECK(plain->axis == 2);
  CHECK(plain->direction == AxisDirection::kAuto);

  auto pedals = ParseWheelAxisBinding(" SimJack | -a1 ");
  REQUIRE(pedals);
  CHECK(pedals->device == "SimJack");
  CHECK(pedals->axis == 1);
  CHECK(pedals->direction == AxisDirection::kInverted);

  auto ids = ParseWheelAxisBinding("0eb7:0020|+axis0");
  REQUIRE(ids);
  CHECK(ids->device == "0eb7:0020");
  CHECK(ids->direction == AxisDirection::kNormal);

  CHECK_FALSE(ParseWheelAxisBinding(""));
  CHECK_FALSE(ParseWheelAxisBinding("button3"));
  CHECK_FALSE(ParseWheelAxisBinding("axis"));
  CHECK_FALSE(ParseWheelAxisBinding("axis-1"));
}

TEST_CASE("Wheel button bindings map buttons and hats to pad controls", "[input][wheel]") {
  auto bindings =
      ParseWheelButtonBindings("A=0, START=9,10, UP=h0.up, LB=Shifter|b3, RT=4, NOPE=5, B=x");
  REQUIRE(bindings.size() == 6);
  CHECK(bindings[0].button == 0);
  CHECK(bindings[0].xbuttons == X_INPUT_GAMEPAD_A);
  CHECK(bindings[1].button == 9);
  CHECK(bindings[2].button == 10);
  CHECK(bindings[2].xbuttons == X_INPUT_GAMEPAD_START);
  CHECK(bindings[3].hat == 0);
  CHECK(bindings[3].hat_mask == 0x01);
  CHECK(bindings[3].xbuttons == X_INPUT_GAMEPAD_DPAD_UP);
  CHECK(bindings[4].device == "Shifter");
  CHECK(bindings[4].button == 3);
  CHECK(bindings[4].xbuttons == X_INPUT_GAMEPAD_LEFT_SHOULDER);
  CHECK(bindings[5].right_trigger);
  CHECK(bindings[5].xbuttons == 0);
  CHECK(ParseWheelButtonBindings("").empty());
}

TEST_CASE("Wheel devices match by USB IDs or name", "[input][wheel]") {
  CHECK(WheelDeviceMatches("", "Anything", 1, 2));
  CHECK(WheelDeviceMatches("0EB7:0020", "FANATEC CSL DD", 0x0EB7, 0x0020));
  CHECK_FALSE(WheelDeviceMatches("0eb7:0021", "FANATEC CSL DD", 0x0EB7, 0x0020));
  CHECK(WheelDeviceMatches("csl dd", "FANATEC CSL DD", 0, 0));
  CHECK_FALSE(WheelDeviceMatches("pedals", "FANATEC CSL DD", 0, 0));
}

TEST_CASE("Pedal direction is detected from the resting value", "[input][wheel]") {
  CHECK(ResolveAxisDirection(AxisDirection::kAuto, -32768) == AxisDirection::kNormal);
  CHECK(ResolveAxisDirection(AxisDirection::kAuto, 32767) == AxisDirection::kInverted);
  CHECK(ResolveAxisDirection(AxisDirection::kNormal, 32767) == AxisDirection::kNormal);

  CHECK(PedalFromAxis(-32768, AxisDirection::kNormal, 0) == 0);
  CHECK(PedalFromAxis(32767, AxisDirection::kNormal, 0) == 255);
  CHECK(PedalFromAxis(32767, AxisDirection::kInverted, 0) == 0);
  CHECK(PedalFromAxis(-32768, AxisDirection::kInverted, 0) == 255);
  // A pedal resting a little off its end stays released inside the deadzone.
  CHECK(PedalFromAxis(-31000, AxisDirection::kNormal, 5) == 0);
  CHECK(PedalFromAxis(32767, AxisDirection::kNormal, 5) == 255);
}

TEST_CASE("Steering keeps its sign through deadzone and scale", "[input][wheel]") {
  CHECK(SteeringFromAxis(0, false, 100, 0) == 0);
  CHECK(SteeringFromAxis(32767, false, 100, 0) == 32767);
  CHECK(SteeringFromAxis(-32768, false, 100, 0) == -32767);
  CHECK(SteeringFromAxis(16384, false, 200, 0) == 32767);
  CHECK(SteeringFromAxis(16384, true, 100, 0) < 0);
  CHECK(SteeringFromAxis(1000, false, 100, 5) == 0);
  CHECK(SteeringFromAxis(32767, false, 100, 5) == 32767);
}

TEST_CASE("Hats become the d-pad", "[input][wheel]") {
  CHECK(DpadFromHat(0) == 0);
  CHECK(DpadFromHat(0x01 | 0x02) == (X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_RIGHT));
  CHECK(DpadFromHat(0x0C) == (X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_DPAD_LEFT));
}

TEST_CASE("Motor values become a signed constant force", "[input][wheel]") {
  CHECK(ConstantForceFromMotors(0, 0, 100, false) == 0);
  CHECK(ConstantForceFromMotors(0, 65535, 100, false) == 32767);
  CHECK(ConstantForceFromMotors(65535, 0, 100, false) == -32767);
  CHECK(ConstantForceFromMotors(65535, 0, 100, true) == 32767);
  CHECK(ConstantForceFromMotors(0, 65535, 50, false) == 16383);
  CHECK(ConstantForceFromMotors(30000, 30000, 100, false) == 0);
  CHECK(ConstantForceFromMotors(0, 65535, 0, false) == 0);

  CHECK(ParseWheelFfbMode("off") == WheelFfbMode::kOff);
  CHECK(ParseWheelFfbMode("Rumble") == WheelFfbMode::kRumble);
  CHECK(ParseWheelFfbMode("difference") == WheelFfbMode::kDifference);
}

TEST_CASE("One motor read as offset binary becomes a signed force", "[input][wheel]") {
  CHECK(ConstantForceFromSignedMotor(0x8000, 100, false) == 0);
  CHECK(ConstantForceFromSignedMotor(0xFFFF, 100, false) == 32767);
  CHECK(ConstantForceFromSignedMotor(0x0000, 100, false) == -32767);
  CHECK(ConstantForceFromSignedMotor(0x0000, 100, true) == 32767);
  CHECK(ConstantForceFromSignedMotor(0xFFFF, 50, false) == 16383);

  CHECK(ParseWheelFfbMode("left-signed") == WheelFfbMode::kLeftSigned);
  CHECK(ParseWheelFfbMode("RIGHT-SIGNED") == WheelFfbMode::kRightSigned);
  CHECK(ConstantForceForMode(WheelFfbMode::kLeftSigned, 0xFFFF, 0x8000, 100, false) == 32767);
  CHECK(ConstantForceForMode(WheelFfbMode::kRightSigned, 0xFFFF, 0x8000, 100, false) == 0);
  CHECK(ConstantForceForMode(WheelFfbMode::kDifference, 0, 65535, 100, false) == 32767);
  CHECK(ConstantForceForMode(WheelFfbMode::kRumble, 0, 65535, 100, false) == 0);
  CHECK(ConstantForceForMode(WheelFfbMode::kOff, 0, 65535, 100, false) == 0);
  CHECK(std::string(WheelFfbModeName(WheelFfbMode::kRightSigned)) == "right-signed");
}
