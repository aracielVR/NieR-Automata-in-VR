// Foveated shading's rate map: each 16x16 tile's shading rate from its angle to the image's centre (foveated.cpp)
#pragma once
#include <stdint.h>
#include "core/vecmath.h"

namespace pure {

inline constexpr int32_t kFoveationTile = 16;  // pixels: NVIDIA's shading rate tiles
enum FOVEATION_RATE : uint8_t { FOVEATION_FULL,
								FOVEATION_HALF,
								FOVEATION_QUARTER };  // 1x1, 2x2, 4x4 pixels per shade

struct FoveationRings {
	float fullDeg, halfDeg;  // sharp within fullDeg, 2x2 within halfDeg, 4x4 beyond
};
// strength: 1 subtle, 2 strong (0 off)
inline FoveationRings foveation_rings(int32_t strength) { return strength >= 2 ? FoveationRings{22, 35} : FoveationRings{30, 1000}; }

// The image's tiles (width x height pixels, tangents of its half-angles halfW x halfH) into `rates`,
// row by row: tiles_x(width) x tiles_y(height) entries. centreX/centreY say where the picture's
// middle sits in tangent units (AsymmetricEyes): full detail stays around the eye's own axis.
inline int32_t foveation_tiles(int32_t pixels) { return (pixels + kFoveationTile - 1) / kFoveationTile; }
inline void foveation_map(int32_t width, int32_t height, float halfW, float halfH, float centreX, float centreY, FoveationRings rings, uint8_t* rates) {
	int32_t tilesX = foveation_tiles(width), tilesY = foveation_tiles(height);
	float fullTan = ftan(rings.fullDeg * kPi / 180), halfTan = rings.halfDeg >= 89 ? 1e9f : ftan(rings.halfDeg * kPi / 180);
	for (int32_t y = 0; y < tilesY; y++)
		for (int32_t x = 0; x < tilesX; x++) {
			// the tile's corner nearest the centre: a tile is only coarser when all of it is
			float cx = ((float)x * kFoveationTile + kFoveationTile * 0.5f) / (float)width * 2 - 1;
			float cy = ((float)y * kFoveationTile + kFoveationTile * 0.5f) / (float)height * 2 - 1;
			float slackX = (float)kFoveationTile / (float)width, slackY = (float)kFoveationTile / (float)height;
			cx = cx > slackX ? cx - slackX : (cx < -slackX ? cx + slackX : 0);
			cy = cy > slackY ? cy - slackY : (cy < -slackY ? cy + slackY : 0);
			float tx = cx * halfW + centreX, ty = cy * halfH + centreY, off = fsqrt(tx * tx + ty * ty);  // tan of the angle off the eye's axis
			rates[y * tilesX + x] = off < fullTan ? FOVEATION_FULL : (off < halfTan ? FOVEATION_HALF : FOVEATION_QUARTER);
		}
}

}  // namespace pure
