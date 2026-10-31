// Checks the view-fix transforms: T maps game view space to eye view space, Tinv back.
#include <cstdio>
#include <cmath>
#include <cstring>
#include "core/pure.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
static void apply(const pure::M4& m, const float p[4], float o[4]) {
	for (int r = 0; r < 4; r++) {
		o[r] = 0;
		for (int c = 0; c < 4; c++) o[r] += m.at(r, c) * p[c];
	}
}
int main() {
	// game camera: rotated 30 deg about Y, at (5, 2, -3); eye camera: different pose
	pure::M4 Cg = pure::m4_from_pose({0, 0.2588190f, 0, 0.9659258f}, {5, 2, -3});
	pure::M4 Ce = pure::m4_from_pose({0.0436194f, 0, 0, 0.9990482f}, {5.03f, 2.1f, -5.5f});
	pure::M4 Vg = pure::m4_rigid_inverse(Cg), Ve = pure::m4_rigid_inverse(Ce);
	pure::M4 T = pure::m4_mul(Ve, Cg), Ti = pure::m4_mul(Vg, Ce);
	float world[4] = {1.5f, 0.7f, -9.0f, 1};
	float pg[4], pe[4], viaT[4], back[4];
	apply(Vg, world, pg);
	apply(Ve, world, pe);
	apply(T, pg, viaT);
	apply(Ti, pe, back);
	for (int k = 0; k < 3; k++) {
		check(std::fabs(viaT[k] - pe[k]) < 1e-4f, "T: game view -> eye view");
		check(std::fabs(back[k] - pg[k]) < 1e-4f, "Tinv: eye view -> game view");
	}
	// probe rows: row . p_game == (row * Tinv) . p_eye
	float row[4] = {0.3f, -0.2f, 0.9f, 4.0f}, rowT[4] = {0, 0, 0, 0};
	for (int c = 0; c < 4; c++)
		for (int k = 0; k < 4; k++) rowT[c] += row[k] * Ti.at(k, c);
	float a = 0, b = 0;
	for (int k = 0; k < 4; k++) {
		a += row[k] * pg[k];
		b += rowT[k] * pe[k];
	}
	check(std::fabs(a - b) < 1e-4f, "probe row transform");
	std::printf(fails ? "viewfix tests FAILED\n" : "viewfix tests passed\n");
	return fails != 0;
}
