// Finger curls (input/finger_curl.h): from hand tracking's joints, and from a controller's buttons.
#include <cstdio>
#include <cmath>
#include "input/finger_curl.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
// A hand in XR_EXT_hand_tracking's joint order, each finger's joints bent by `bend` radians apiece.
static void make_hand(float bend, pure::V3 joints[pure::kHandJoints]) {
	joints[0] = {0, 0, -0.05f};  // palm
	joints[1] = {0, 0, 0};       // wrist
	for (int finger = 0; finger < pure::FINGER_COUNT; finger++) {
		int first = finger == pure::FINGER_THUMB ? 2 : 6 + (finger - 1) * 5, count = finger == pure::FINGER_THUMB ? 4 : 5;
		pure::V3 at = {0.02f * (float)finger - 0.04f, 0, -0.02f};
		float angle = 0;
		for (int joint = 0; joint < count; joint++) {
			joints[first + joint] = at;
			if (joint > 0) angle += bend;  // bends towards -y (the palm), one joint at a time
			at = at + pure::V3{0, -std::sin(angle), -std::cos(angle)} * 0.03f;
		}
	}
}
int main() {
	pure::V3 joints[pure::kHandJoints];
	make_hand(0, joints);
	pure::FingerCurls open = pure::finger_curls_from_joints(joints);
	bool allOpen = true;
	for (float curl : open.curl) allOpen = allOpen && curl < 1e-3f;
	check(allOpen, "joints: a flat hand is open");

	make_hand(0.7f, joints);
	pure::FingerCurls half = pure::finger_curls_from_joints(joints);
	check(std::fabs(half.curl[pure::FINGER_INDEX] - 3 * 0.7f / pure::kFistFinger) < 1e-3f, "joints: a finger's bends added up, against a fist");
	check(std::fabs(half.curl[pure::FINGER_THUMB] - 2 * 0.7f / pure::kFistThumb) < 1e-3f, "joints: the thumb's two bends");

	make_hand(1.6f, joints);
	pure::FingerCurls fist = pure::finger_curls_from_joints(joints);
	bool allFist = true;
	for (float curl : fist.curl) allFist = allFist && curl == 1;
	check(allFist, "joints: bent past a fist is a fist");

	pure::FingerCurls b = pure::finger_curls_from_buttons({0, 0, false, false});
	check(b.curl[pure::FINGER_INDEX] == 0 && b.curl[pure::FINGER_THUMB] == 0 && b.curl[pure::FINGER_MIDDLE] == pure::kHoldingCurl,
		  "buttons: nothing touched: index and thumb out, the rest holding the controller");
	b = pure::finger_curls_from_buttons({0, 0, true, true});
	check(b.curl[pure::FINGER_INDEX] == pure::kTriggerTouchCurl && b.curl[pure::FINGER_THUMB] == pure::kThumbDownCurl,
		  "buttons: touching the trigger and a button curls them a little");
	b = pure::finger_curls_from_buttons({1, 1, true, false});
	check(b.curl[pure::FINGER_INDEX] == 1 && b.curl[pure::FINGER_MIDDLE] == 1 && b.curl[pure::FINGER_LITTLE] == 1, "buttons: trigger and grip pulled: a fist");
	b = pure::finger_curls_from_buttons({0.5f, 0, false, false});
	check(b.curl[pure::FINGER_INDEX] == 0.5f, "buttons: no trigger touch sensor: the trigger alone");

	pure::FingerCurls from = {}, to = {{1, 1, 1, 1, 1}};
	pure::FingerCurls step = pure::smooth_curls(from, to, 0.35f);
	check(std::fabs(step.curl[2] - 0.35f) < 1e-6f, "smoothing: part of the way");
	if (!fails) std::printf("finger curl tests passed\n");
	return fails ? 1 : 0;
}
