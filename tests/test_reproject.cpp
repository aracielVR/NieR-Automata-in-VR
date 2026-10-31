// Depth reprojection: the shader's math (reproject.cpp kHlsl) on the CPU. A world point seen by an eye
// with camera C1 must land where the same eye sees it with camera C2.
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
static pure::M4 pose(float yawDeg, float pitchDeg, pure::V3 p) {
	float y = yawDeg / 57.29578f, x = pitchDeg / 57.29578f;
	return pure::m4_from_pose(pure::q_mul(pure::Q{0, std::sin(y / 2), 0, std::cos(y / 2)}, pure::Q{std::sin(x / 2), 0, 0, std::cos(x / 2)}), p);
}
static pure::V3 mulp(const pure::M4& m, pure::V3 v) {
	return {m.at(0, 0) * v.x + m.at(0, 1) * v.y + m.at(0, 2) * v.z + m.at(0, 3), m.at(1, 0) * v.x + m.at(1, 1) * v.y + m.at(1, 2) * v.z + m.at(1, 3),
			m.at(2, 0) * v.x + m.at(2, 1) * v.y + m.at(2, 2) * v.z + m.at(2, 3)};
}

// The vertex shader's gap test (0.4.1): a triangle of grid corners (pixel, depth) in the old image is a
// gap when its depth jumps by more than edge0 and the motion stretched it beyond edge1 grid steps.
struct Corner {
	float px, py, d;
};
static bool gapTri(const Corner* c, const pure::M4& T, float W, float H, float step, float P22, float P23, float sx,
				   float sy, float edge0 = 1.03f, float edge1 = 2.5f) {
	float s[3][2], dist[3];
	for (int i = 0; i < 3; i++) {
		float u = (c[i].px + 0.5f) / W, v = (c[i].py + 0.5f) / H;
		float nx = u * 2 - 1, ny = 1 - v * 2, z = P23 / (-c[i].d - P22);
		pure::V3 q = mulp(T, pure::V3{nx * -z / sx, ny * -z / sy, z});
		if (q.z >= 0) return false;
		s[i][0] = q.x * sx / -q.z * W / 2;
		s[i][1] = q.y * sy / -q.z * H / 2;
		dist[i] = -z;
	}
	float dmin = std::fmin(dist[0], std::fmin(dist[1], dist[2])), dmax = std::fmax(dist[0], std::fmax(dist[1], dist[2]));
	float e = 0;
	for (int i = 0; i < 3; i++) {
		int j = (i + 1) % 3;
		e = std::fmax(e, std::hypot(s[i][0] - s[j][0], s[i][1] - s[j][1]));
	}
	return dmax > dmin * edge0 && e > step * edge1;
}
static float depthAt(float dist, float P22, float P23) { return (P22 * -dist + P23) / dist; }
int main() {
	const float P22 = -1.00001f, P23 = -0.100001f, sx = 1 / 1.402f, sy = 1 / 1.091f;
	// eye offset relative to the camera (scaled), as in StereoReprojectParams
	pure::M4 E = pose(3, -2, {0.03f, 0.01f, 0.0f});
	pure::M4 C1 = pose(20, 0, {250, 10, 660}), C2 = pose(24, 1, {250.4f, 10, 659.7f});  // camera moved/turned a little
	pure::M4 Ce1 = pure::m4_mul(C1, E), Ce2 = pure::m4_mul(C2, E);
	for (pure::V3 world : {pure::V3{248, 11, 655}, pure::V3{240, 10.5f, 650}, pure::V3{100, 30, 300}, pure::V3{249.5f, 9.8f, 658.9f}}) {
		// render with eye 1: view-space point, ndc, depth
		pure::V3 v1 = mulp(pure::m4_rigid_inverse(Ce1), world);
		if (v1.z >= 0) continue;
		float w = -v1.z, nx = sx * v1.x / w, ny = sy * v1.y / w, d = (P22 * v1.z + P23) / w;
		// shader: back to view space from ndc + depth, move with T, project
		float z = P23 / (-d - P22);
		pure::V3 p{nx * -z / sx, ny * -z / sy, z};
		pure::M4 T = pure::m4_mul(pure::m4_rigid_inverse(Ce2), Ce1);
		pure::V3 q = mulp(T, p);
		float cx = q.x * sx / -q.z, cy = q.y * sy / -q.z, cd = (P22 * q.z + P23) / -q.z;
		// reference: render with eye 2 directly
		pure::V3 v2 = mulp(pure::m4_rigid_inverse(Ce2), world);
		float rx = sx * v2.x / -v2.z, ry = sy * v2.y / -v2.z, rd = (P22 * v2.z + P23) / -v2.z;
		char what[96];
		std::snprintf(what, sizeof(what), "point %.1f %.1f %.1f lands where eye 2 sees it", world.x, world.y, world.z);
		check(std::fabs(cx - rx) < 2e-3f && std::fabs(cy - ry) < 2e-3f && std::fabs(cd - rd) < 1e-4f, what);
	}
	// unchanged camera: identity
	{
		pure::M4 T = pure::m4_mul(pure::m4_rigid_inverse(Ce1), Ce1);
		bool id = true;
		for (int i = 0; i < 16; i++) id = id && std::fabs(T.m[i] - (i % 5 == 0 ? 1.f : 0.f)) < 1e-4f;
		check(id, "same camera: T is the identity");
	}
	// gap test: the camera orbits 2B (3 m away) by 2 degrees, background 30 m behind her
	{
		const float W = 4800, H = 2704, step = 4;
		pure::M4 A = pose(0, 0, {0, 0, 3}), B = pose(2, 0, {3 * std::sin(2 / 57.29578f), 0, 3 * std::cos(2 / 57.29578f)});
		pure::M4 T = pure::m4_mul(pure::m4_rigid_inverse(B), A);
		float near_ = depthAt(3, P22, P23), far_ = depthAt(30, P22, P23);
		Corner edgeTri[3] = {{2400, 1352, near_}, {2404, 1352, far_}, {2400, 1356, near_}};
		check(gapTri(edgeTri, T, W, H, step, P22, P23, sx, sy), "2B's edge over the background is a gap");
		Corner bodyTri[3] = {{2400, 1352, near_}, {2404, 1352, depthAt(3.02f, P22, P23)}, {2400, 1356, near_}};
		check(!gapTri(bodyTri, T, W, H, step, P22, P23, sx, sy), "a triangle on 2B is not a gap");
		Corner farTri[3] = {{1000, 1352, far_}, {1004, 1352, depthAt(31, P22, P23)}, {1000, 1356, far_}};
		check(!gapTri(farTri, T, W, H, step, P22, P23, sx, sy), "background with a small depth step is not a gap");
		// flat ground seen at a grazing angle 100 m away: big relative depth change, but not pulled open
		Corner ground[3] = {{2400, 1400, depthAt(100, P22, P23)}, {2404, 1400, depthAt(100, P22, P23)}, {2400, 1404, depthAt(92, P22, P23)}};
		check(!gapTri(ground, T, W, H, step, P22, P23, sx, sy), "distant ground is not a gap");
		// no motion: nothing is a gap
		pure::M4 I = pure::m4_mul(pure::m4_rigid_inverse(A), A);
		check(!gapTri(edgeTri, I, W, H, step, P22, P23, sx, sy), "no camera motion: no gap");
	}
	// depth 1 (sky) gives a finite far point
	{
		float z = P23 / (-1.0f - P22);
		check(z < -5000 && z > -20000, "sky depth maps to the far plane");
	}
	std::printf(fails ? "reprojection tests FAILED\n" : "reprojection tests passed\n");
	return fails != 0;
}
