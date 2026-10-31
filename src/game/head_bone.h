// First-person bone identification and head pose math.
#pragma once
#include <iterator>
#include <math.h>
#include <stdint.h>
#include "core/vecmath.h"

namespace pure {

// Each skinned part uploads 48-byte row-major bone matrices in its skinning buffer.
inline constexpr int32_t kBodyPartBones[] = {59, 1, 15, 63, 63, 25, 62, 63, 2, 23, 20, 14, 11, 14, 8};
inline constexpr int32_t kBodyPartCount = std::size(kBodyPartBones);
inline constexpr bool is_clothing_part(int32_t part) { return part == 2 || part == 3 || part == 4 || part == 6 || part == 7; }
// Head-only parts include the face, hair, face details, and eye bone lists.
inline constexpr int32_t kHeadOnlyList = 100;
inline constexpr bool is_head_only_part(int32_t part) { return part == 1 || part == 9 || part == 10 || part == 13 || part == kHeadOnlyList; }
inline constexpr int32_t kBoneBytes = 48;
inline constexpr int32_t kPartsToConfirm = 3;  // her parts in a row before they count as hers

// Head and eye bind-pose locations in metres (+Y up, +Z forward).
inline constexpr int32_t kHeadPart = 0, kHeadSlot = 10;
inline constexpr V3 kEyeBind = {0, 1.56f, 0.07f};
// Camera position relative to the head bind pose.
inline constexpr V3 kViewBind = {0, 1.65f, 0.07f};
inline constexpr V3 kNeckPivotBind = {0, 1.49f, -0.019f};
// Ankle bind positions and standing height.
inline constexpr int32_t kFeetPart = 14, kRightAnkleSlot = 0, kLeftAnkleSlot = 4;
inline constexpr V3 kRightAnkleBind = {-0.169f, 0.174f, -0.020f}, kLeftAnkleBind = {0.169f, 0.174f, -0.020f};

// Identifies parts from bone-list sizes and first-bind order.
struct BodyPartWalker {
	int32_t next = 0;     // her part expected next (0: looking for her first part)
	int32_t matched = 0;  // her parts in a row so far

	// Returns the part for a new bone-list size, or -1 if it does not match.
	int32_t identify(int32_t bones) {
		if (next > 0)
			for (int32_t part = next; part < kBodyPartCount; part++) {
				if (bones == kBodyPartBones[part]) {
					next = part + 1;
					matched++;
					return part;
				}
				if (!is_clothing_part(part)) break;
			}
		if (bones == kBodyPartBones[0]) {
			next = 1;
			matched = 1;
			return 0;
		}
		next = matched = 0;
		return -1;
	}
	bool confirmed() const { return matched >= kPartsToConfirm; }
};

inline V3 bone_point(const float m[12], V3 p) {
	return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
			m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

inline bool plausible_bone(const float m[12]) {
	for (int row = 0; row < 3; row++) {
		float squared = m[row * 4] * m[row * 4] + m[row * 4 + 1] * m[row * 4 + 1] + m[row * 4 + 2] * m[row * 4 + 2];
		if (!(squared > 0.81f && squared < 1.21f)) return false;
	}
	return true;
}

// Head position and forward direction in world space.
struct HeadPose {
	V3 eye;
	V3 forward;  // unit length
};

inline HeadPose head_pose(const float head[12], V3 eyeBind) {
	V3 forward = {head[2], head[6], head[10]};  // the bone's +z
	float size = length(forward);
	return {bone_point(head, eyeBind), size > 1e-6f ? forward / size : V3{0, 0, 1}};
}

// Ground height from the ankle positions.
inline float ground_height(const float leftAnkleBone[12], const float rightAnkleBone[12]) {
	float left = bone_point(leftAnkleBone, kLeftAnkleBind).y, right = bone_point(rightAnkleBone, kRightAnkleBind).y;
	return (left < right ? left : right) - kLeftAnkleBind.y;
}

// Smooths ground height quickly and eye height above ground slowly to remove running bob.
struct SteadyHeight {
	static constexpr float kGroundSeconds = 0.15f, kHeightSeconds = 0.8f;
	bool valid = false;
	float ground = 0, height = 0;

	float update(float eyeY, float groundY, float seconds) {
		if (!valid || seconds > 0.5f) {  // first time, or after a pause: start where she is
			valid = true;
			ground = groundY;
			height = eyeY - groundY;
		} else if (seconds > 0) {
			ground += (groundY - ground) * (1 - expf(-seconds / kGroundSeconds));
			height += (eyeY - groundY - height) * (1 - expf(-seconds / kHeightSeconds));
		}
		return ground + height;
	}
};

}  // namespace pure
