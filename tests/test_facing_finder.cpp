// The facing finder (game/facing_finder.h): which float in the player object follows the way the character faces.
#include <cstdio>
#include <cmath>
#include "game/facing_finder.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
int main() {
	using F = pure::FacingFinder;
	{
		static F finder;
		static float floats[F::kFloats];
		bool found = false;
		for (int frame = 0; frame < 600 && !found; frame++) {
			float facing = std::sin(frame * 0.01f) * 3.0f;                        // turning all the way round and back
			float hips = facing + 0.2f * std::sin(frame * 0.5f);                  // the hips swaying around it
			for (int i = 0; i < F::kFloats; i++) floats[i] = (float)(i % 7) * 0.5f;  // constants that sometimes agree
			floats[0x50 / 4] = 100;                                                // position: not an angle
			floats[0x94 / 4] = -facing;                                            // the facing, the other way round
			floats[0x98 / 4] = frame % 40 < 34 ? facing : facing + 2;              // agrees, but only 85% of the time... nearly
			found = finder.sample(floats, hips);
		}
		check(found && finder.offset == 0x94 && finder.sign == -1, "finds the float that follows the hips (negated), not the near misses");
	}
	{
		static F finder;
		static float floats[F::kFloats];
		bool found = false;
		for (int frame = 0; frame < 600; frame++) {
			for (int i = 0; i < F::kFloats; i++) floats[i] = 0.4f;
			found = finder.sample(floats, 0.4f + 0.05f * std::sin(frame * 0.3f));  // standing still, facing one way
		}
		check(!found, "facing one way only: nothing is sure");
		float share;
		int directions;
		check(finder.best(&share, &directions) == -1, "... and no candidate seen in two ways");
	}
	if (!fails) std::printf("facing finder tests passed\n");
	return fails ? 1 : 0;
}
