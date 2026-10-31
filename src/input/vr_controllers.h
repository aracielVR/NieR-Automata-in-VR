// vr_controllers.h - VR controllers as a gamepad, and motion controls (vr_controllers.cpp).
#pragma once
#include <stdint.h>
#include <xinput.h>
#include "xr/openxr.h"

// Game side (input/gamepad.cpp)
bool VrInputPad(XINPUT_GAMEPAD* out, bool evenIfOff = false);  // false = no VR gamepad; evenIfOff: for the menu
void VrInputRumble(uint16_t left, uint16_t right);

// VR thread
void VrInputInit(PFN_xrGetInstanceProcAddr getProcAddr, XrInstance instance, bool pico, bool handTracking);
void VrInputAttach(XrSession session);
void VrInputSync(bool focused, bool foreground);
void VrInputMotion(XrSpace local, XrSpace view, XrTime time, bool focused, bool flipped);  // motion controls, after VrInputSync
// Each hand's highest linear / angular speed (m/s, deg/s) and acceleration (m/s^2, deg/s^2) since the
// last call, in the tracking space (the stats overlay). -1 = not known (hand not tracked, or the
// runtime gives no angular velocity).
struct HandPeaks {
	float speed, spin, accel, spinAccel;
};
void VrInputTakeHandPeaks(HandPeaks out[2]);
// Each VR frame after VrInputSync: each hand's finger curls, from hand tracking (joints located in `base` at
// `time`) or else from the buttons.
void VrInputFingers(XrSpace base, XrTime time);
// Any thread: a hand's finger curls (thumb, index, middle, ring, little; 0 open, 1 a fist), for first
// person's hands. False = not known lately.
bool VrInputFingerCurls(int hand, float out[5]);
// A hand's grip pose in `base` at `time` (first person's arms); false = not known.
bool VrInputLocateHand(int hand, XrSpace base, XrTime time, XrPosef* out);
void VrInputProfileChanged();                                                              // interaction profile changed event
void VrInputDetach();
void VrInputShutdown();
