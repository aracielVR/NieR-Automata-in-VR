// Late warp (xr_main.cpp PSWarp): a pixel of the shown eye must sample the rendered image where the
// same world direction was drawn. CPU mirror of the shader and of the matrix built in Frame().
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
static pure::Q yawPitch(float y, float p) {
	y /= 57.29578f, p /= 57.29578f;
	return pure::q_mul(pure::Q{0, std::sin(y / 2), 0, std::cos(y / 2)}, pure::Q{std::sin(p / 2), 0, 0, std::cos(p / 2)});
}
struct Warp {
	float outTan[4], srcTan[2], m[3][3];
};
static Warp make(pure::Q render, pure::Q shown, float l, float r, float u, float d, float hw, float hh) {
	Warp w{{l, r, u, d}, {hw, hh}, {}};
	pure::M4 rr = pure::m4_from_pose(render, {0, 0, 0}), rd = pure::m4_from_pose(shown, {0, 0, 0});
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) w.m[i][j] = rr.at(0, i) * rd.at(0, j) + rr.at(1, i) * rd.at(1, j) + rr.at(2, i) * rd.at(2, j);
	return w;
}
// shader: output uv -> source uv (false = outside)
static bool sample(const Warp& w, float u, float v, float* su, float* sv) {
	float d[3] = {w.outTan[0] + (w.outTan[1] - w.outTan[0]) * u, w.outTan[2] + (w.outTan[3] - w.outTan[2]) * v, -1};
	float r[3];
	for (int i = 0; i < 3; i++) r[i] = w.m[i][0] * d[0] + w.m[i][1] * d[1] + w.m[i][2] * d[2];
	if (r[2] > -1e-4f) return false;
	float tx = r[0] / -r[2], ty = r[1] / -r[2];
	*su = tx / w.srcTan[0] * 0.5f + 0.5f;
	*sv = 0.5f - ty / w.srcTan[1] * 0.5f;
	return *su >= 0 && *su <= 1 && *sv >= 0 && *sv <= 1;
}
// world direction -> uv in an eye with orientation q and tangent window
static void project(pure::Q q, const float dir[3], float l, float r, float u, float d, float* pu, float* pv) {
	pure::M4 m = pure::m4_from_pose(q, {0, 0, 0});
	float e[3];
	for (int i = 0; i < 3; i++) e[i] = m.at(0, i) * dir[0] + m.at(1, i) * dir[1] + m.at(2, i) * dir[2];  // R^T dir
	float tx = e[0] / -e[2], ty = e[1] / -e[2];
	*pu = (tx - l) / (r - l);
	*pv = (ty - u) / (d - u);
}
int main() {
	float s54 = std::tan(54 / 57.29578f), s40 = std::tan(40 / 57.29578f), s44 = std::tan(44 / 57.29578f), s55 = std::tan(55 / 57.29578f);
	float hw = s54 * 1.1f, hh = s55 * 1.1f;  // rendered symmetric frustum (Anamorphic, FovScale 1.10)
	// 1. same pose, symmetric window = rendered window: identity
	{
		Warp w = make(yawPitch(10, 5), yawPitch(10, 5), -hw, hw, hh, -hh, hw, hh);
		float su, sv;
		bool in = sample(w, 0.3f, 0.7f, &su, &sv);
		check(in && std::fabs(su - 0.3f) < 1e-4f && std::fabs(sv - 0.7f) < 1e-4f, "identity");
	}
	// 2. Quest 3 left eye (54 out / 40 in, 44 up / 55 down), same pose: straight ahead -> rendered centre
	{
		Warp w = make(yawPitch(0, 0), yawPitch(0, 0), -s54, s40, s44, -s55, hw, hh);
		float u0 = s54 / (s54 + s40), v0 = s44 / (s44 + s55), su, sv;  // where straight ahead is in the shown eye
		check(sample(w, u0, v0, &su, &sv) && std::fabs(su - 0.5f) < 1e-4f && std::fabs(sv - 0.5f) < 1e-4f,
			  "asymmetric eye: straight ahead samples the rendered centre");
	}
	// 3. head turned between render and display: every shown pixel samples where the same world
	//    direction was rendered
	for (float yaw : {3.f, -7.f}) {
		pure::Q qr = yawPitch(20, -5), qd = yawPitch(20 + yaw, -5 + yaw * 0.3f);
		Warp w = make(qr, qd, -s54, s40, s44, -s55, hw, hh);
		bool ok = true;
		for (float u = 0.1f; u < 1; u += 0.2f)
			for (float v = 0.1f; v < 1; v += 0.2f) {
				// world direction of this shown pixel
				float d[3] = {-s54 + (s40 + s54) * u, s44 + (-s55 - s44) * v, -1};
				pure::M4 m = pure::m4_from_pose(qd, {0, 0, 0});
				float wd[3];
				for (int i = 0; i < 3; i++) wd[i] = m.at(i, 0) * d[0] + m.at(i, 1) * d[1] + m.at(i, 2) * d[2];
				float eu, ev, su, sv;
				project(qr, wd, -hw, hw, hh, -hh, &eu, &ev);  // where it was rendered
				bool in = sample(w, u, v, &su, &sv);
				bool expectIn = eu >= 0 && eu <= 1 && ev >= 0 && ev <= 1;
				if (in != expectIn || (in && (std::fabs(su - eu) > 1e-3f || std::fabs(sv - ev) > 1e-3f))) ok = false;
			}
		check(ok, yaw > 0 ? "turned 3 deg: same world direction" : "turned -7 deg: same world direction");
	}
	// 4. turned beyond the margin: the far side falls outside (black), the centre stays inside
	{
		Warp w = make(yawPitch(0, 0), yawPitch(30, 0), -s54, s40, s44, -s55, hw, hh);
		float su, sv;
		check(!sample(w, 0.02f, 0.5f, &su, &sv), "turned 30 deg left: the left edge has no picture");
		check(sample(w, 0.9f, 0.5f, &su, &sv), "turned 30 deg left: the right part still has picture");
	}
	if (!fails) std::printf("late warp tests passed\n");
	return fails ? 1 : 0;
}
