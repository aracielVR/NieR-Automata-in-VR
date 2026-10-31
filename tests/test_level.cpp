// Adaptive levelling: horizontal views are levelled, top-down views keep their pitch.
#include <cstdio>
#include <cmath>
#include <initializer_list>
#include "core/pure.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
static pure::M4 camAt(float yawDeg, float pitchDeg) {
	float y = yawDeg / 57.29578f, p = pitchDeg / 57.29578f;
	pure::Q qy{0, std::sin(y / 2), 0, std::cos(y / 2)}, qp{std::sin(p / 2), 0, 0, std::cos(p / 2)};
	return pure::m4_from_pose(pure::q_mul(qy, qp), {1, 2, 3});
}
static float pitchOf(const pure::M4& m) { return std::asin(-m.at(1, 2)) * 57.29578f; }
static float yawOf(const pure::M4& m) { return std::atan2(m.at(0, 2), m.at(2, 2)) * 57.29578f; }
int main() {
	for (float yaw : {0.f, 37.f, -120.f, 170.f}) {
		float pd, k;
		pure::M4 a = pure::m4_level_camera_adaptive(camAt(yaw, -20), 55, 75, &pd, &k);
		check(std::fabs(pitchOf(a)) < 0.1f && k == 0, "horizontal view levelled");
		check(std::fabs(std::remainder(yawOf(a) - yaw, 360.f)) < 0.2f, "heading kept (levelled)");
		pure::M4 b = pure::m4_level_camera_adaptive(camAt(yaw, -85), 55, 75, &pd, &k);
		check(std::fabs(pitchOf(b) + 85) < 0.2f && k > 0.999f, "top-down keeps pitch");
		pure::M4 c = pure::m4_level_camera_adaptive(camAt(yaw, -89.9f), 55, 75, &pd, &k);
		check(std::fabs(pitchOf(c) + 89.9f) < 0.3f, "straight down stable");
		pure::M4 d = pure::m4_level_camera_adaptive(camAt(yaw, -65), 55, 75, &pd, &k);
		check(pitchOf(d) < -20 && pitchOf(d) > -45, "in between: partial");
		check(std::fabs(std::remainder(yawOf(d) - yaw, 360.f)) < 0.3f, "heading kept (partial)");
	}
	// 0.3.13 band (45-60): the flight unit's ~65 deg view keeps its full tilt
	for (float yaw : {0.f, 90.f}) {
		float pd, k;
		pure::M4 e = pure::m4_level_camera_adaptive(camAt(yaw, -65), 45, 60, &pd, &k);
		check(std::fabs(pitchOf(e) + 65) < 0.2f && k > 0.999f, "65 deg fully kept with 45-60");
		check(std::fabs(pure::camera_pitch_deg(camAt(yaw, -65)) + 65) < 0.1f, "camera_pitch_deg down");
		check(std::fabs(pure::camera_pitch_deg(camAt(yaw, 30)) - 30) < 0.1f, "camera_pitch_deg up");
		// low behind the character the camera looks UP steeply: still levelled, nothing kept
		pure::M4 f = pure::m4_level_camera_adaptive(camAt(yaw, 55), 45, 60, &pd, &k);
		check(std::fabs(pitchOf(f)) < 0.1f && k == 0, "upward view levelled (not a top-down view)");
		check(std::fabs(std::remainder(yawOf(f) - yaw, 360.f)) < 0.3f, "heading kept (upward)");
	}
	std::printf(fails ? "level tests FAILED\n" : "level tests passed\n");
	return fails != 0;
}
