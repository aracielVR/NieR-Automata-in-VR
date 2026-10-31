// Upscaler helpers (pure.h): jitter sequence and output sizes.
#include <cstdio>
#include <cmath>
#include "core/pure.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
static bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }
int main() {
	// Halton: base 2 = 1/2, 1/4, 3/4, 1/8 ...; base 3 = 1/3, 2/3, 1/9 ...
	check(near(pure::halton(1, 2), 0.5f) && near(pure::halton(2, 2), 0.25f) && near(pure::halton(3, 2), 0.75f) &&
			  near(pure::halton(4, 2), 0.125f),
		  "halton base 2");
	check(near(pure::halton(1, 3), 1 / 3.f) && near(pure::halton(2, 3), 2 / 3.f) && near(pure::halton(3, 3), 1 / 9.f),
		  "halton base 3");
	{  // the offsets cover the pixel evenly: each quarter of it gets a quarter of 16 samples
		int q[4] = {};
		for (uint32_t i = 1; i <= 16; i++) q[(pure::halton(i, 2) >= 0.5f) + 2 * (pure::halton(i, 3) >= 0.5f)]++;
		check(q[0] >= 3 && q[1] >= 3 && q[2] >= 3 && q[3] >= 3, "jitter spread over the pixel");
	}
	uint32_t w, h;
	// the output stays at the target; the game's resolution is the input
	check(pure::upscale_output(1600, 900, 2400, 1350, &w, &h) && w == 2400 && h == 1350, "1600x900 to a 2400x1350 target");
	check(pure::upscale_output(1200, 675, 2400, 1350, &w, &h) && w == 2400 && h == 1350, "smaller game image, same output");
	check(pure::upscale_output(2400, 1350, 2400, 1350, &w, &h) && w == 2400 && h == 1350, "game at the target: DLAA");
	check(pure::upscale_output(2560, 1440, 2400, 1350, &w, &h) && w == 2560 && h == 1440, "game above the target: DLAA, kept");
	check(pure::upscale_output(640, 360, 2400, 1350, &w, &h) && w == 1920 && h == 1080, "at most 3x");
	check(pure::upscale_output(1600, 900, 2000, 1500, &w, &h) && w >= 1998 && h >= 1500 && std::fabs((float)w / h - 16.f / 9) < 0.01f,
		  "a target of another shape: covered in the game's shape");
	check(pure::upscale_output(1600, 900, 0, 0, &w, &h) && w == 1600 && h == 900, "target unknown: DLAA");
	check(pure::upscale_output(1001, 563, 2400, 1350, &w, &h) && w % 2 == 0 && h % 2 == 0, "even sizes");
	check(pure::upscale_output(3840, 2160, 16000, 9000, &w, &h) && w <= 8192 && h <= 8192, "capped at 8192");
	check(!pure::upscale_output(32, 32, 2400, 1350, &w, &h), "too small to upscale");
	// the mode follows the ratio, as in games
	check(pure::upscale_mode(1.0f) == 0 && pure::upscale_mode(1.5f) == 1 && pure::upscale_mode(1.72f) == 2 &&
			  pure::upscale_mode(2.0f) == 3 && pure::upscale_mode(3.0f) == 4,
		  "NVIDIA's ratios give their own modes");
	check(pure::upscale_mode(1.1f) == 0 && pure::upscale_mode(1.35f) == 1 && pure::upscale_mode(2.3f) == 3, "in-between ratios");
	pure::upscale_input_for(2400, 1350, 1, &w, &h);
	check(w == 1600 && h == 904, "Quality for 2400x1350 is about 1600x900");
	pure::upscale_input_for(2400, 1350, 3, &w, &h);
	check(w == 1200 && h == 672, "Performance for 2400x1350 is about 1200x675");
	check(pure::jitter_phases(1.0f) == 8 && pure::jitter_phases(1.5f) == 18 && pure::jitter_phases(2.0f) == 32 &&
			  pure::jitter_phases(3.0f) == 72,
		  "jitter phases 8 x ratio^2");
	if (!fails) std::printf("upscale tests passed\n");
	return fails ? 1 : 0;
}
