// Quaternions from matrices and the angle between two of them.
#include <cstdio>
#include <cmath>
#include <initializer_list>
#include "core/vecmath.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
static pure::Q yawPitch(float yawDeg, float pitchDeg) {
	float y = yawDeg / 57.29578f, p = pitchDeg / 57.29578f;
	return pure::q_mul(pure::Q{0, std::sin(y / 2), 0, std::cos(y / 2)}, pure::Q{std::sin(p / 2), 0, 0, std::cos(p / 2)});
}
static bool qnear(pure::Q a, pure::Q b, float e = 1e-4f) {
	float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
	return d > 1 - e;
}
int main() {
	// q_from_m4 round trip, including the w<0-ish branches (large angles)
	for (float y : {0.f, 30.f, 120.f, 179.f, -170.f})
		for (float p : {0.f, -40.f, 80.f}) {
			pure::Q q = yawPitch(y, p);
			check(qnear(pure::q_from_m4(pure::m4_from_pose(q, {1, 2, 3})), q), "q_from_m4 round trip");
		}
	pure::Q q180{1, 0, 0, 0};
	check(qnear(pure::q_from_m4(pure::m4_from_pose(q180, {0, 0, 0})), q180), "180 deg about x");
	check(std::fabs(pure::q_angle_deg(yawPitch(0, 0), yawPitch(7, 0)) - 7) < 0.01f, "angle 7");
	check(std::fabs(pure::q_angle_deg(yawPitch(10, 0), yawPitch(-30, 0)) - 40) < 0.01f, "angle 40");
	check(std::fabs(pure::q_angle_deg(pure::Q{0, 0, 0, 1}, pure::Q{0, 1, 0, 0}) - 180) < 0.01f, "angle 180");
	std::printf(fails ? "quaternion tests FAILED\n" : "quaternion tests passed\n");
	return fails != 0;
}
