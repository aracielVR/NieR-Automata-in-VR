// Finger curl input from hand tracking or controller buttons.
#pragma once
#include <stdint.h>
#include "core/vecmath.h"

namespace pure {

enum FINGER { FINGER_THUMB,
			  FINGER_INDEX,
			  FINGER_MIDDLE,
			  FINGER_RING,
			  FINGER_LITTLE,
			  FINGER_COUNT };

struct FingerCurls {
	float curl[FINGER_COUNT];
};

// Returns the angle between two directions in radians.
inline float angle_between(V3 a, V3 b) {
	float la = length(a), lb = length(b);
	if (la < 1e-6f || lb < 1e-6f) return 0;
	float sine = length(cross(a, b)) / (la * lb), cosine = dot(a, b) / (la * lb);
	return 2 * fatan(sine / (1 + cosine + 1e-9f));  // tan(angle / 2)
}

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Estimates curl from the 26 XR_EXT_hand_tracking joint positions.
inline constexpr int32_t kHandJoints = 26;
inline constexpr float kFistThumb = 2.0f;   // thumb: knuckle and tip joint (radians)
inline constexpr float kFistFinger = 4.2f;  // finger: its three joints
inline FingerCurls finger_curls_from_joints(const V3 joints[kHandJoints]) {
	FingerCurls out = {};
	for (int32_t finger = 0; finger < FINGER_COUNT; finger++) {
		int32_t first = finger == FINGER_THUMB ? 2 : 6 + (finger - 1) * 5, count = finger == FINGER_THUMB ? 4 : 5;
		float bent = 0;
		for (int32_t joint = first + 1; joint + 1 < first + count; joint++)
			bent += angle_between(joints[joint] - joints[joint - 1], joints[joint + 1] - joints[joint]);
		out.curl[finger] = clamp01(bent / (finger == FINGER_THUMB ? kFistThumb : kFistFinger));
	}
	return out;
}

// Estimates curl from trigger, grip, and thumb-button input.
struct FingerButtons {
	float trigger, squeeze;
	bool triggerTouch, thumbTouch;
};
inline constexpr float kTriggerTouchCurl = 0.35f, kHoldingCurl = 0.25f, kThumbDownCurl = 0.6f;
inline FingerCurls finger_curls_from_buttons(const FingerButtons& b) {
	FingerCurls out = {};
	float trigger = clamp01(b.trigger), squeeze = clamp01(b.squeeze);
	out.curl[FINGER_THUMB] = b.thumbTouch ? kThumbDownCurl : 0;
	out.curl[FINGER_INDEX] = b.triggerTouch ? kTriggerTouchCurl + (1 - kTriggerTouchCurl) * trigger : trigger;
	for (int32_t finger = FINGER_MIDDLE; finger < FINGER_COUNT; finger++) out.curl[finger] = kHoldingCurl + (1 - kHoldingCurl) * squeeze;
	return out;
}

// Smooths current curls toward target by amount.
inline FingerCurls smooth_curls(const FingerCurls& from, const FingerCurls& target, float amount) {
	FingerCurls out;
	for (int32_t finger = 0; finger < FINGER_COUNT; finger++) out.curl[finger] = from.curl[finger] + (target.curl[finger] - from.curl[finger]) * amount;
	return out;
}

}  // namespace pure
