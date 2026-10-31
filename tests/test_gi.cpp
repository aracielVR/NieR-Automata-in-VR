// GI volumes: which slots stay on, and how long the game's loop must be (pure::gi_pick).
#include <cstdio>
#include <cstring>
#include "core/pure.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
static float buf[128 * 24];
// a box of half size h around view-space point p (rows: identity, translation -p)
static void put(int slot, float px, float py, float pz, float h) {
	float* v = buf + slot * 24;
	memset(v, 0, 96);
	v[0] = px, v[1] = py, v[2] = pz, v[3] = 1;
	v[4] = 1, v[7] = -px;
	v[9] = 1, v[11] = -py;
	v[14] = 1, v[15] = -pz;
	v[20] = v[21] = v[22] = h;
}
static bool on(const pure::GiPick& p, int i) { return p.mask[i >> 5] >> (i & 31) & 1; }
int main() {
	memset(buf, 0, sizeof(buf));
	// 5 volumes in slots 3, 10, 11, 40, 41; distances 50, 5, 0 (inside), 100, 20
	put(3, 0, 0, -50 - 2, 2);
	put(10, 0, 0, -5 - 2, 2);
	put(11, 0, 0, 0, 2);
	put(40, 0, 0, -100 - 2, 2);
	put(41, 20 + 2, 0, 0, 2);
	pure::GiPick a = pure::gi_pick(buf, 128, 128, 8);
	check(a.used == 5 && a.highest == 41 && !a.dropping, "all kept: counts");
	check(a.length == 50, "all kept: loop ends 8 past the last used slot");
	check(on(a, 0) && on(a, 127), "all kept: nothing switched off");
	pure::GiPick b = pure::gi_pick(buf, 128, 3, 8);
	check(b.dropping && b.kept == 3, "keep 3: three kept");
	check(on(b, 11) && on(b, 10) && on(b, 41), "keep 3: the nearest three (inside, 5, 20)");
	check(!on(b, 3) && !on(b, 40), "keep 3: the far ones off");
	check(b.length == 42, "keep 3: loop ends after slot 41");
	pure::GiPick c = pure::gi_pick(buf, 128, 2, 8);
	check(on(c, 11) && on(c, 10) && !on(c, 41) && c.length == 12, "keep 2: loop ends after slot 11");
	pure::GiPick d = pure::gi_pick(buf, 128, 5, 8);
	check(!d.dropping && d.length == 50, "keep = used: nothing dropped");
	memset(buf, 0, sizeof(buf));
	pure::GiPick e = pure::gi_pick(buf, 128, 16, 8);
	check(e.used == 0 && e.length == 8, "empty buffer: short loop (margin)");
	put(127, 0, 0, 0, 1);
	check(pure::gi_pick(buf, 128, 128, 8).length == 128, "last slot used: full length");
	if (!fails) std::printf("gi tests passed\n");
	return fails ? 1 : 0;
}
