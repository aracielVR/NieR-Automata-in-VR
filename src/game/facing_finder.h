// Finds the character's facing direction
#pragma once
#include <math.h>
#include <stdint.h>
#include "core/vecmath.h"

namespace pure {

struct FacingFinder {
	static constexpr int32_t kBytes = 0x400, kFloats = kBytes / 4;
	static constexpr uint32_t kMinSamples = 300;  // frames looked at (about 5 s)
	static constexpr int32_t kMinDirections = 5;  // of 8: they faced this many ways while it agreed
	static constexpr float kAgree = 0.35f;        // radians: the hips sway and twist around the facing
	static constexpr float kMinShare = 0.85f;     // of the frames, it agreed in this many
	struct Candidate {
		uint32_t hits[2];        // agreed, as +yaw and as -yaw
		uint8_t directions[2];  // the 8 ways (45 degrees each) it agreed in
	};
	Candidate candidates[kFloats] = {};
	uint32_t samples = 0;
	int32_t offset = -1;  // where it is (bytes; -1: not found yet)
	float sign = 1;       // the game's angle is sign x yaw_of

	// Checks one player object snapshot against the hips' yaw.
	bool sample(const float floats[kFloats], float hipsYaw) {
		if (offset >= 0) return true;
		samples++;
		int32_t direction = (int32_t)((wrap_angle(hipsYaw) + kPi) / (2 * kPi) * 8) & 7;
		for (int32_t i = 0; i < kFloats; i++) {
			float value = floats[i];
			if (!(fabsf(value) < 20)) continue;  // not an angle (or not a number)
			for (int32_t s = 0; s < 2; s++) {
				if (fabsf(wrap_angle((s ? -value : value) - hipsYaw)) > kAgree) continue;
				candidates[i].hits[s]++;
				candidates[i].directions[s] |= (uint8_t)(1 << direction);
			}
		}
		if (samples < kMinSamples) return false;
		float best = kMinShare;
		for (int32_t i = 0; i < kFloats; i++)
			for (int32_t s = 0; s < 2; s++) {
				float share = (float)candidates[i].hits[s] / (float)samples;
				if (share >= best && __builtin_popcount(candidates[i].directions[s]) >= kMinDirections) {
					best = share;
					offset = i * 4;
					sign = s ? -1.0f : 1.0f;
				}
			}
		return offset >= 0;
	}

	// Best matching candidate so far.
	int32_t best(float* share, int32_t* directions) const {
		int32_t bestIndex = -1;
		uint32_t bestHits = 0;
		for (int32_t i = 0; i < kFloats; i++)
			for (int32_t s = 0; s < 2; s++)
				if (candidates[i].hits[s] > bestHits && __builtin_popcount(candidates[i].directions[s]) >= 2) {
					bestHits = candidates[i].hits[s];
					bestIndex = i;
					*directions = __builtin_popcount(candidates[i].directions[s]);
				}
		*share = samples ? (float)bestHits / (float)samples : 0;
		return bestIndex < 0 ? -1 : bestIndex * 4;
	}
};

}  // namespace pure
