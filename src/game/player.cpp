// Player position and facing hooks.
#include "game/player.h"
#include "core/log.h"
#include "core/safe_memory.h"
#include "game/facing_finder.h"
#include "game/first_person.h"
#include "input/gamepad.h"
#include <math.h>
#include <stdint.h>
#include <windows.h>

namespace {
constexpr uintptr_t kPlayerPointer = 0x1020948;  // in the exe's own memory
constexpr uintptr_t kPositionOffset = 0x50;
constexpr float kSameSpot = 1.5f;       // the object's position this close to the character's bones (metres, sideways)
constexpr float kSameHeight = 3.0f;     // ... and up or down (in the air their bones are well above it)
constexpr DWORD kAttackFacingMs = 250;  // after an attack button goes down, the facing is set this long (else the game turns them back)
constexpr float kStillFacing = 0.6f;    // radians: the found facing must agree with the hips this well before an attack's first write
constexpr DWORD kFinderReportMs = 60000;

bool s_loggedFound, s_loggedMismatch, s_loggedAttackFacing, s_loggedFacingDisagrees;
pure::FacingFinder s_finder;
DWORD s_finderReportTick;
uintptr_t s_finderObject;  // the object the finder looked at (a new one starts it over)
DWORD s_facedAttack;       // the attack (its button's tick) the facing is being set for

// Returns the player object only when its position matches the character's bones.
uintptr_t CharacterObject(pure::V3* position) {
	pure::V3 feet;
	if (!FirstPersonFeet(&feet)) return 0;
	uintptr_t object = 0;
	if (!SafeRead((uintptr_t)GetModuleHandleW(nullptr) + kPlayerPointer, &object, sizeof(object)) || !object) return 0;
	float at[3];
	if (!SafeRead(object + kPositionOffset, at, sizeof(at))) return 0;
	if (!(fabsf(at[0] - feet.x) < kSameSpot && fabsf(at[1] - feet.y) < kSameHeight && fabsf(at[2] - feet.z) < kSameSpot)) {
		if (!s_loggedMismatch) {
			s_loggedMismatch = true;
			Log("player: the object at NieRAutomata.exe+0x%llx is at %.2f %.2f %.2f, the character's bones at %.2f %.2f %.2f: "
				"not them (left alone)",
				(unsigned long long)kPlayerPointer, (double)at[0], (double)at[1], (double)at[2], (double)feet.x, (double)feet.y, (double)feet.z);
		}
		return 0;
	}
	if (!s_loggedFound) {
		s_loggedFound = true;
		Log("player: found the character's object (NieRAutomata.exe+0x%llx -> %llx), their position at +0x%llx",
			(unsigned long long)kPlayerPointer, (unsigned long long)object, (unsigned long long)kPositionOffset);
	}
	*position = {at[0], at[1], at[2]};
	return object;
}

// One frame of looking for the facing in `object`, with the hips' yaw. Only reads.
void FindFacing(uintptr_t object, float hipsYaw) {
	if (object != s_finderObject) {
		if (s_finderObject && s_finder.offset < 0) Log("player: a new player object: looking for the facing again");
		s_finderObject = object;
		if (s_finder.offset < 0) s_finder = {};
	}
	if (s_finder.offset >= 0) return;
	static float floats[pure::FacingFinder::kFloats];
	if (!SafeRead(object, floats, sizeof(floats))) return;
	if (s_finder.sample(floats, hipsYaw)) {
		Log("player: found the character's facing at +0x%x (the game's angle = %sthe yaw about +y): attacks will go where you look",
			s_finder.offset, s_finder.sign > 0 ? "" : "minus ");
		return;
	}
	if (GetTickCount() - s_finderReportTick > kFinderReportMs) {
		s_finderReportTick = GetTickCount();
		float share = 0;
		int32_t directions = 0, best = s_finder.best(&share, &directions);
		Log("player: still looking for the character's facing (%u frames; best +0x%x agreed %.0f%%, facing %d of 8 ways). Turn "
			"around now and then",
			s_finder.samples, best, (double)(share * 100), directions);
	}
}

// Sets the character's facing for `attack` (its button's tick), every frame of the window: their hips only
// catch up over a few frames, and the game would turn them back towards the stick.
void FaceForAttack(uintptr_t object, float hipsYaw, float yaw, DWORD attack) {
	if (s_finder.offset < 0 || object != s_finderObject) return;
	uintptr_t at = object + (uintptr_t)s_finder.offset;
	float now;
	if (!SafeRead(at, &now, sizeof(now))) return;
	bool first = attack != s_facedAttack;
	if (first && fabsf(pure::wrap_angle(s_finder.sign * now - hipsYaw)) > kStillFacing) {  // not the facing any more: leave it
		if (!s_loggedFacingDisagrees) {
			s_loggedFacingDisagrees = true;
			Log("player: the facing at +0x%x (%.2f) doesn't agree with the hips (%.2f): not turned for the attack", s_finder.offset,
				(double)now, (double)hipsYaw);
		}
		return;
	}
	s_facedAttack = attack;
	float value = s_finder.sign * yaw;
	if (!SafeWrite(at, &value, sizeof(value))) return;
	if (!s_loggedAttackFacing) {
		s_loggedAttackFacing = true;
		Log("player: an attack turned towards where you look (%.0f deg from where they faced)", (double)(pure::wrap_angle(yaw - hipsYaw) * 57.29578f));
	}
}
}  // namespace

bool PlayerMove(pure::V3 step, pure::V3* from) {
	uintptr_t object = CharacterObject(from);
	if (!object) return false;
	float x = from->x + step.x, z = from->z + step.z;  // x and z only: the game keeps their height (the ground)
	return SafeWrite(object + kPositionOffset, &x, sizeof(x)) && SafeWrite(object + kPositionOffset + 8, &z, sizeof(z));
}

bool PlayerPosition(pure::V3* position) { return CharacterObject(position) != 0; }

void PlayerUpdate(pure::V3 lookForward) {
	float hipsYaw;
	pure::V3 position;
	if (!FirstPersonFacing(&hipsYaw)) return;
	uintptr_t object = CharacterObject(&position);
	if (!object) return;
	FindFacing(object, hipsYaw);
	DWORD attack = GamepadAttackTick();
	if (GetTickCount() - attack < kAttackFacingMs && (lookForward.x != 0 || lookForward.z != 0))
		FaceForAttack(object, hipsYaw, pure::yaw_of({lookForward.x, 0, lookForward.z}), attack);
}
