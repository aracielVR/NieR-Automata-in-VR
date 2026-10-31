// Headset-shaped resolutions (pure::headset_sizes): 50-100% of the full-detail size, no duplicates.
#include <cstdio>
#include "core/pure.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
static uint32_t key(uint32_t w, uint32_t h) { return w << 16 | h; }
int main() {
	uint32_t plain[] = {key(1920, 1080), key(2560, 1440), key(3840, 2160)};
	uint32_t hw[6], hh[6];
	int n = pure::headset_sizes(2160, 2304, plain, 3, hw, hh);
	check(n == 6 && hw[0] == 1080 && hh[0] == 1152 && hw[5] == 2160 && hh[5] == 2304, "headset: 1080x1152 .. 2160x2304");
	uint32_t listedHeadset[] = {key(1728, 1840)};
	n = pure::headset_sizes(2160, 2304, listedHeadset, 1, hw, hh);
	check(n == 5, "headset: a listed size is not added twice");
	n = pure::headset_sizes(1000, 1100, nullptr, 0, hw, hh);
	check(n == 4 && hw[0] == 704, "headset: sizes below 640x480 left out");
	n = pure::headset_sizes(12000, 20000, nullptr, 0, hw, hh);
	check(n == 4 && hh[3] == 16000, "headset: only sizes within 16384");
	// the panel's own render size: the finished picture (headset detail x Output) divided by the
	// enlargement the chosen quality asks for, which is what PanelRenderSize() sets the game to
	for (int mode = 0; mode < 4; mode++) {
		uint32_t w, h;
		pure::upscale_input_for(2336, 2520, mode, &w, &h);
		check(pure::upscale_mode((float)2336 / w) == mode, "panel size: lands on the quality it was asked for");
		check(w <= 2336 && h <= 2520 && w % 8 == 0 && h % 8 == 0, "panel size: no larger than the finished picture");
	}
	if (!fails) std::printf("resolution tests passed\n");
	return fails ? 1 : 0;
}
