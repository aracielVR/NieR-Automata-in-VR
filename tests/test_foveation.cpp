// Foveated shading's rate map (render/foveation.h): full detail in the middle, coarser towards the edges.
#include <cstdio>
#include <vector>
#include "render/foveation.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
int main() {
	using namespace pure;
	// a 1248x1312 eye image, 50 degrees each way from the centre
	const int32_t width = 1248, height = 1312;
	const float half = ftan(50 * kPi / 180);
	const int32_t tilesX = foveation_tiles(width), tilesY = foveation_tiles(height);
	check(tilesX == 78 && tilesY == 82, "16x16 tiles, rounded up");
	std::vector<uint8_t> rates((size_t)(tilesX * tilesY));
	auto rateAt = [&](float degX, float degY) {
		// the tile looking that way
		float x = ftan(degX * kPi / 180) / half, y = ftan(degY * kPi / 180) / half;
		int32_t tx = (int32_t)((x + 1) / 2 * width) / kFoveationTile, ty = (int32_t)((y + 1) / 2 * height) / kFoveationTile;
		return rates[(size_t)(ty * tilesX + tx)];
	};

	foveation_map(width, height, half, half, 0, 0, foveation_rings(1), rates.data());
	check(rateAt(0, 0) == FOVEATION_FULL, "subtle: the centre is sharp");
	check(rateAt(25, 0) == FOVEATION_FULL, "... and 25 degrees out");
	check(rateAt(40, 0) == FOVEATION_HALF, "... 40 degrees out: half");
	check(rateAt(45, 45) == FOVEATION_HALF, "... never a quarter, even in the corners");

	foveation_map(width, height, half, half, 0, 0, foveation_rings(2), rates.data());
	check(rateAt(15, 10) == FOVEATION_FULL, "strong: sharp near the centre");
	check(rateAt(28, 0) == FOVEATION_HALF, "... half at 28 degrees");
	check(rateAt(45, 0) == FOVEATION_QUARTER, "... a quarter at 45");
	check(rateAt(-45, 0) == FOVEATION_QUARTER && rateAt(0, -45) == FOVEATION_QUARTER, "... the same on every side");

	// AsymmetricEyes: the picture's middle sits 0.3 (about 17 degrees) away from where the eye
	// looks, so the sharp part follows the eye, not the picture
	{
		const float centreX = 0.3f;
		foveation_map(width, height, half, half, centreX, 0, foveation_rings(2), rates.data());
		auto rateAtTan = [&](float tx, float ty) {
			float x = (tx - centreX) / half, y = ty / half;
			int32_t ox = (int32_t)((x + 1) / 2 * width) / kFoveationTile, oy = (int32_t)((y + 1) / 2 * height) / kFoveationTile;
			return rates[(size_t)(oy * tilesX + ox)];
		};
		check(rateAtTan(0, 0) == FOVEATION_FULL, "off-centre lens: sharp where the eye looks");
		check(rateAtTan(ftan(45 * kPi / 180), 0) == FOVEATION_QUARTER, "... coarse 45 degrees out from there");
	}

	if (!fails) std::printf("ok\n");
	return fails ? 1 : 0;
}
