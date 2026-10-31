// Head-direction culling: the look-at handed to the game's frustum builder turns with the headset.
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
static pure::M4 rot(float yawDeg, float pitchDeg) {
	float y = yawDeg / 57.29578f, p = pitchDeg / 57.29578f;
	pure::Q qy{0, std::sin(y / 2), 0, std::cos(y / 2)}, qp{std::sin(p / 2), 0, 0, std::cos(p / 2)};
	return pure::m4_from_pose(pure::q_mul(qy, qp), {0, 0, 0});
}
static bool near(const float* a, float x, float y, float z, float e = 1e-3f) {
	return std::fabs(a[0] - x) < e && std::fabs(a[1] - y) < e && std::fabs(a[2] - z) < e;
}
int main() {
	// camera at (10, 2, 5) looking along -z (target 3 units ahead), world up
	float eye[3] = {10, 2, 5}, target[3] = {10, 2, 2}, up[3] = {0, 1, 0}, t[3] = {}, u[3] = {};
	check(pure::head_lookat(eye, target, up, pure::m4_identity(), t, u), "identity ok");
	check(near(t, 10, 2, 2) && near(u, 0, 1, 0), "identity keeps the look-at");
	// head turned 90 deg left (yaw +90 about +y): looks along -x
	check(pure::head_lookat(eye, target, up, rot(90, 0), t, u), "yaw ok");
	check(near(t, 7, 2, 5) && near(u, 0, 1, 0), "90 deg left looks along -x");
	check(pure::head_lookat(eye, target, up, rot(-90, 0), t, u) && near(t, 13, 2, 5), "90 deg right looks along +x");
	// head pitched 30 deg up
	check(pure::head_lookat(eye, target, up, rot(0, 30), t, u), "pitch ok");
	check(near(t, 10, 2 + 3 * 0.5f, 2 + 3 * (1 - 0.8660254f)) && near(u, 0, 0.8660254f, 0.5f), "30 deg up");
	// camera looking down 20 deg, game up vector not orthogonal: identity gives the same direction
	float tgt2[3] = {10, 2 - 3 * 0.34202f, 5 - 3 * 0.93969f};
	check(pure::head_lookat(eye, tgt2, up, pure::m4_identity(), t, u), "pitched cam ok");
	check(near(t, tgt2[0], tgt2[1], tgt2[2]) && std::fabs(u[0] * 0 + u[1] * 0.93969f - u[2] * 0.34202f - 1) < 1e-3f,
		  "pitched camera: same target, orthogonal up");
	// the eye-relative rotation is applied in the camera's frame: camera facing +x, head left -> -z
	float tgt3[3] = {13, 2, 5};
	check(pure::head_lookat(eye, tgt3, up, rot(90, 0), t, u) && near(t, 10, 2, 2), "camera-relative yaw");
	// degenerate inputs are refused
	check(!pure::head_lookat(eye, eye, up, pure::m4_identity(), t, u), "zero-length look refused");
	float upAlong[3] = {0, 0, -1};
	check(!pure::head_lookat(eye, target, upAlong, pure::m4_identity(), t, u), "up along the look refused");
	pure::M4 zero = {};
	check(!pure::head_lookat(eye, target, up, zero, t, u), "non-rotation refused");
	// leading a turning head: 10 deg turned since the last pose, k = 0.5 -> 5 deg further; clamped; position kept
	{
		pure::M4 prev = rot(0, 0), now = rot(10, 0);
		now.at(0, 3) = 1;
		pure::M4 ahead = pure::m4_rotation_ahead(prev, now, 0.5f, 25);
		check(std::fabs(pure::q_angle_deg(pure::q_from_m4(ahead), pure::q_from_m4(rot(15, 0)))) < 0.01f, "half a turn ahead");
		check(ahead.at(0, 3) == 1, "position kept");
		pure::M4 clamped = pure::m4_rotation_ahead(prev, now, 10, 20);
		check(std::fabs(pure::q_angle_deg(pure::q_from_m4(clamped), pure::q_from_m4(rot(30, 0)))) < 0.01f, "clamped to 20 deg");
		pure::M4 still = pure::m4_rotation_ahead(now, now, 1, 20);
		check(pure::q_angle_deg(pure::q_from_m4(still), pure::q_from_m4(now)) < 0.01f, "no turn, no lead");
		pure::M4 pitch = pure::m4_rotation_ahead(rot(0, 0), rot(0, -8), 1, 20);
		check(pure::q_angle_deg(pure::q_from_m4(pitch), pure::q_from_m4(rot(0, -16))) < 0.01f, "pitch leads too");
	}
	std::printf(fails ? "head culling tests FAILED\n" : "head culling tests passed\n");
	return fails != 0;
}
