// First-person head tracking and body posing from the character's bone uploads.
#include "game/first_person.h"
#include "game/body_pose.h"
#include "core/com.h"
#include "core/config.h"
#include "core/log.h"
#include "input/gamepad.h"
#include "input/vr_controllers.h"
#include <math.h>
#include <string.h>

namespace {
constexpr UINT kSkinningSlot = 11;
constexpr DWORD kFreshMs = 250;  // a head or feet older than this aren't used (cutscene, other outfit, ...)
constexpr int32_t kMaxPartBones = 63, kMaxTinyBones = 4;
constexpr bool kLockUpperBody = true;  // false: their upper body keeps the game's animation (turned and leaned to your head)
constexpr bool kPoseFingers = false;  // false: the character's fingers keep the game's animation (grip on the sword)
constexpr float kMaxReach = 1.0f;  // the character's eyes follow your head at most this far (metres; further: a recentre, a glitch)
uint32_t s_frame = 1;

// Copies of candidate bone lists for the current frame.
struct ListCopy {
	ID3D11Resource* list;
	int32_t bones;
	float* data;
};
float s_partData[128][kMaxPartBones * 12];   // lists the size of one of the character's parts
float s_tinyData[1024][kMaxTinyBones * 12];  // lists of 1 to 4 bones (some 350 a frame)
ListCopy s_copies[std::size(s_partData) + std::size(s_tinyData)];
int32_t s_copyCount, s_partCopies, s_tinyCopies;
bool s_loggedCopiesFull;
// Each list's copy this frame, by the buffer's address (open addressing; 0 = none)
struct CopyIndex {
	const void* list;
	uint32_t frame;
	int32_t copy;
};
CopyIndex s_copyIndex[4096];
struct PendingMap {
	ID3D11Resource* list;
	float* data;
	UINT byteWidth;
} s_pending[4];
int32_t s_pendingNext;

bool CouldBeCharacters(int32_t bones) {
	if (bones >= 1 && bones <= kMaxTinyBones) return true;
	for (int32_t size : pure::kBodyPartBones)
		if (bones == size) return true;
	return false;
}

CopyIndex* IndexOf(const void* list) {
	uintptr_t hash = ((uintptr_t)list >> 4) * 0x9E3779B97F4A7C15ull;
	for (size_t i = 0; i < std::size(s_copyIndex); i++) {
		CopyIndex& entry = s_copyIndex[(hash + i) % std::size(s_copyIndex)];
		if (entry.frame != s_frame || entry.list == list) return &entry;
	}
	return nullptr;
}

ListCopy* CopyOf(const void* list) {
	CopyIndex* entry = IndexOf(list);
	return entry && entry->frame == s_frame ? &s_copies[entry->copy] : nullptr;
}

void TakeCopy(ID3D11Resource* list, const float* data, int32_t bones) {
	ListCopy* copy = CopyOf(list);
	if (!copy || copy->bones != bones) {
		bool tiny = bones <= kMaxTinyBones;
		if (s_copyCount >= (int32_t)std::size(s_copies) || (tiny ? s_tinyCopies >= (int32_t)std::size(s_tinyData) : s_partCopies >= (int32_t)std::size(s_partData))) {
			if (!s_loggedCopiesFull) {
				s_loggedCopiesFull = true;
				Log("first person: too many bone lists in a frame to copy them all (%d tiny, %d the size of the character's parts); "
					"some of theirs may be missed",
					s_tinyCopies, s_partCopies);
			}
			return;
		}
		CopyIndex* entry = IndexOf(list);
		if (!entry) return;
		*entry = {list, s_frame, s_copyCount};
		copy = &s_copies[s_copyCount++];
		*copy = {list, bones, tiny ? s_tinyData[s_tinyCopies++] : s_partData[s_partCopies++]};
	}
	memcpy(copy->data, data, (size_t)bones * pure::kBoneBytes);
}

// Per-frame labels for candidate bone lists.
struct Label {
	const void* list;
	uint32_t frame;
	int32_t part;    // which of the character's parts (-1: not theirs)
	bool confirmed;  // the walk that labelled it was confirmed
};
Label s_labels[1024];
pure::BodyPartWalker s_walker;
Label* s_walkHead;  // this walk's part 0 and 1 labels, confirmed once the walk is
Label* s_walkFace;
int32_t s_boundPart = -1;  // the character's part bound at b11 now (-1: none, or not a bone list)

Label* LabelOf(const void* list) {
	uintptr_t hash = ((uintptr_t)list >> 4) * 0x9E3779B97F4A7C15ull;
	for (size_t i = 0; i < std::size(s_labels); i++) {
		Label& label = s_labels[(hash + i) % std::size(s_labels)];
		if (label.frame != s_frame || label.list == list) return &label;  // free this frame, or this list's
	}
	return nullptr;  // full: over 1024 bone lists in a frame
}

// Head and ground poses used by the camera.
struct Tracked {
	bool valid;
	pure::HeadPose head;
	DWORD tick;
	bool groundValid;
	float ground;  // the ground under the character (game units)
	DWORD groundTick;
	pure::SteadyHeight steady;
	double steadyMs;
	float steadyEyeY;
} s_tracked;

UINT s_gameW, s_gameH;
bool s_mainTarget;  // the render target is game-sized (not the shadow map)
bool s_hiding;      // this frame's eyes are in the character's head
bool s_wasHiding;   // ... and the last frame's
bool s_loggedFound, s_loggedHiding, s_loggedOutfit, s_loggedHeadOnly;

void TakeHead(const void* headList) {
	const ListCopy* copy = CopyOf(headList);
	if (!copy || copy->bones != pure::kBodyPartBones[pure::kHeadPart]) return;
	const float* head = copy->data + pure::kHeadSlot * 12;
	if (!pure::plausible_bone(head)) return;
	s_tracked.valid = true;
	s_tracked.head = pure::head_pose(head, pure::kViewBind);  // the camera's place on the head
	s_tracked.tick = GetTickCount();
	if (!s_loggedFound) {
		s_loggedFound = true;
		Log("first person: found the character's bones; eyes at %.2f %.2f %.2f, facing %.2f %.2f %.2f", (double)s_tracked.head.eye.x,
			(double)s_tracked.head.eye.y, (double)s_tracked.head.eye.z, (double)s_tracked.head.forward.x,
			(double)s_tracked.head.forward.y, (double)s_tracked.head.forward.z);
	}
}

void TakeGround(const void* feetList) {
	const ListCopy* copy = CopyOf(feetList);
	if (!copy || copy->bones != pure::kBodyPartBones[pure::kFeetPart] || !s_tracked.valid) return;
	const float *left = copy->data + pure::kLeftAnkleSlot * 12, *right = copy->data + pure::kRightAnkleSlot * 12;
	if (!pure::plausible_bone(left) || !pure::plausible_bone(right)) return;
	s_tracked.groundValid = true;
	s_tracked.ground = pure::ground_height(left, right);
	s_tracked.groundTick = GetTickCount();
	double now = NowMs();
	s_tracked.steadyEyeY = s_tracked.steady.update(s_tracked.head.eye.y, s_tracked.ground, (float)((now - s_tracked.steadyMs) / 1000));
	s_tracked.steadyMs = now;
}

// A bone list bound for the first time this frame: which of the character's parts is it?
void Identify(Label* label, const void* list, int32_t bones) {
	int32_t part = s_walker.identify(bones);
	*label = {list, s_frame, part, s_walker.confirmed()};
	if (part == pure::kHeadPart) {
		s_walkHead = label;
		s_walkFace = nullptr;
	}
	if (part == 1) s_walkFace = label;
	if (part >= 0 && s_walker.matched == pure::kPartsToConfirm) {  // just confirmed
		if (s_walkHead) {
			s_walkHead->confirmed = true;
			TakeHead(s_walkHead->list);
		}
		if (s_walkFace) s_walkFace->confirmed = true;
		if (part != pure::kPartsToConfirm - 1 && !s_loggedOutfit) {  // the clothing parts were skipped
			s_loggedOutfit = true;
			Log("first person: 2B without her dress (after self-destructing): her head and feet are still found");
		}
	}
	if (part == pure::kFeetPart && label->confirmed) TakeGround(list);
}

// Finds the character's bone lists by matching shared bone matrices.
struct CharacterLists {
	bool found;
	int32_t parts;
	const ListCopy* part[pure::kBodyPartCount];
	const uint8_t* slots[pure::kBodyPartCount];
	const ListCopy* headOnly[8];  // tiny lists of the head and eye bones (the eyes)
	int32_t headOnlyCount;
} s_character;
float s_known[pure::kSkeletonBones][12];  // the character's bones' matrices found so far this frame
bool s_have[pure::kSkeletonBones];
constexpr int32_t kMinParts = 6;  // parts found before they count (10 after 2B self-destructs)
struct Feet {
	pure::V3 at;  // the ground under the hips
	float facing;  // the hips' yaw
	DWORD tick;
} s_feet;

bool SameBone(const float* a, const float* b) {
	for (int i = 0; i < 12; i++)
		if (fabsf(a[i] - b[i]) > 1e-4f) return false;
	return true;
}

// Checks whether known bones match the supplied slot mapping.
bool Fits(const ListCopy& copy, const uint8_t* slots) {
	int32_t shared = 0;
	for (int32_t slot = 0; slot < copy.bones; slot++) {
		if (!s_have[slots[slot]]) continue;
		if (!SameBone(copy.data + slot * 12, s_known[slots[slot]])) return false;
		shared++;
	}
	return shared > 0;
}

void Learn(const ListCopy& copy, const uint8_t* slots) {
	for (int32_t slot = 0; slot < copy.bones; slot++) {
		memcpy(s_known[slots[slot]], copy.data + slot * 12, sizeof(s_known[0]));
		s_have[slots[slot]] = true;
	}
}

// Checks whether a small bone list follows the head and eye positions.
constexpr float kAtEyes = 0.03f;
bool RidesHead(const ListCopy& copy, const float* head) {
	pure::V3 eyes = pure::bone_point(head, pure::kEyeBind);
	bool hasHead = false;
	for (int32_t slot = 0; slot < copy.bones; slot++) {
		const float* bone = copy.data + slot * 12;
		if (SameBone(bone, head)) hasHead = true;
		else if (pure::length(pure::bone_point(bone, pure::kEyeBind) - eyes) > kAtEyes) return false;
	}
	return hasHead;
}

bool Assigned(const ListCopy* copy) {
	for (const ListCopy* part : s_character.part)
		if (part == copy) return true;
	return false;
}

// Collects lists whose bone matrices match the character's known parts.
void FindCharacter(const ListCopy* body) {
	using namespace pure;
	s_character = {};
	memset(s_have, 0, sizeof(s_have));
	s_character.part[kHeadPart] = body;
	s_character.slots[kHeadPart] = kPart0Slots;
	s_character.parts = 1;
	Learn(*body, kPart0Slots);
	for (bool grew = true; grew;) {
		grew = false;
		for (int32_t i = 0; i < s_copyCount; i++) {
			const ListCopy& copy = s_copies[i];
			if (Assigned(&copy)) continue;
			for (int32_t part = 1; part < kBodyPartCount; part++) {
				if (s_character.part[part] || copy.bones != kBodyPartBones[part]) continue;
				const uint8_t* slots = kPartSlots[part].bones;
				if (!Fits(copy, slots) && part == 13 && Fits(copy, kPart13SeriousSlots)) slots = kPart13SeriousSlots;
				if (!Fits(copy, slots)) continue;
				s_character.part[part] = &copy;
				s_character.slots[part] = slots;
				s_character.parts++;
				Learn(copy, slots);
				grew = true;
				break;
			}
		}
	}
	const float* head = s_known[BONE_HEAD];
	for (int32_t i = 0; i < s_copyCount && s_character.headOnlyCount < (int32_t)std::size(s_character.headOnly); i++) {
		const ListCopy& copy = s_copies[i];
		if (copy.bones > kMaxTinyBones || Assigned(&copy)) continue;
		if (RidesHead(copy, head)) s_character.headOnly[s_character.headOnlyCount++] = &copy;
	}
	s_character.found = s_character.parts >= kMinParts;
	if (s_character.found && plausible_bone(s_known[BONE_HIPS]) && plausible_bone(s_known[BONE_LEFT_ANKLE]) &&
		plausible_bone(s_known[BONE_RIGHT_ANKLE])) {
		V3 hips = bone_point(s_known[BONE_HIPS], kSkeleton[BONE_HIPS].bind);
		const float* h = s_known[BONE_HIPS];
		float facing = yaw_of(V3{h[2], h[6], h[10]});  // the bind pose's +z, turned
		s_feet = {{hips.x, ground_height(s_known[BONE_LEFT_ANKLE], s_known[BONE_RIGHT_ANKLE]), hips.z}, facing, GetTickCount()};
	}
	if (s_character.found && s_character.headOnlyCount > 1 && !s_loggedHeadOnly) {
		s_loggedHeadOnly = true;
		Log("first person: %d of the character's lists found by their bones, and %d tiny lists of the head and eye bones "
			"(the eyes): hidden like the head",
			s_character.parts, s_character.headOnlyCount);
	}
}

// Returns the matched body part, kHeadOnlyList, or -1.
int32_t CharacterPartOf(const void* list) {
	if (!s_character.found) return -1;
	for (int32_t part = 0; part < pure::kBodyPartCount; part++)
		if (s_character.part[part] && s_character.part[part]->list == list) return part;
	for (int32_t i = 0; i < s_character.headOnlyCount; i++)
		if (s_character.headOnly[i]->list == list) return pure::kHeadOnlyList;
	return -1;
}

// Tracking state for the current viewer and hands.
bool s_uploading;  // our own upload of the posed lists
bool s_swapping;   // our own bind of a list with parts hidden
struct Viewer {
	bool valid;
	pure::M4 view;               // your head (between your eyes), camera-to-world
	pure::M4 camera;             // the camera without your head (turned by the right stick)
	pure::V3 fromCharacterEyes;  // ... from the character's eyes then (world)
	DWORD tick;
} s_viewer;
bool s_loggedBody, s_loggedArms, s_loggedFingers;
pure::BodyTurn s_bodyTurn;
double s_bodyTurnMs;

bool FingersKnown(const pure::ArmBones& arm) {
	for (int32_t finger = 0; finger < pure::FINGER_COUNT; finger++)
		for (int32_t joint = 0; joint < pure::finger_joints(finger); joint++)
			if (!s_have[arm.fingers[finger] + joint] || !pure::plausible_bone(s_known[arm.fingers[finger] + joint])) return false;
	return true;
}
struct Hands {
	bool valid[2];
	pure::M4 grip[2];  // world, at the same moment as s_viewer
} s_hands;
void Upload(ID3D11DeviceContext* c, ID3D11Resource* list, const float* bones, int32_t count) {
	D3D11_MAPPED_SUBRESOURCE mapped;
	s_uploading = true;
	if (SUCCEEDED(c->Map(list, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		memcpy(mapped.pData, bones, (size_t)count * pure::kBoneBytes);
		c->Unmap(list, 0);
	}
	s_uploading = false;
}

// Parts in your face: during attacks and jumps, their legs, shoulders and arms near your eyes are hidden in
// the main view (their shadow keeps them). An arm stays while its hand is where your controller puts it.
constexpr float kHideNear = 0.25f;     // metres from your eyes
constexpr DWORD kActionMs = 1500;      // an attack or jump button this recently: an attack or jump animation
constexpr float kArmFollows = 0.15f;   // the posed hand this close to your controller: the arm is yours to see
enum HIDE_GROUP : uint8_t { HIDE_NONE,
							HIDE_LEG,
							HIDE_SHOULDER,
							HIDE_LEFT_ARM,
							HIDE_RIGHT_ARM };
uint8_t s_hideGroup[pure::kSkeletonBones];
bool s_hideGroupsKnown;
ID3D11Buffer* s_hiddenLists[pure::kBodyPartCount];  // our copies of their parts' lists, with bones hidden
bool s_hiddenThisFrame[pure::kBodyPartCount];
int32_t s_swappedPart = -1;  // the part whose hidden copy is bound for this draw
bool s_loggedHidingParts;

// Each bone's group, from the bones it hangs off (itself included)
void LearnHideGroups() {
	using namespace pure;
	for (int32_t bone = 0; bone < kSkeletonBones; bone++) {
		uint8_t group = HIDE_NONE;
		bool shoulder = false;
		for (int32_t b = bone; b >= 0 && group == HIDE_NONE; b = kSkeleton[b].parent) {
			if (b == BONE_LEFT_THIGH || b == BONE_RIGHT_THIGH) group = HIDE_LEG;
			else if (b == BONE_LEFT_FOREARM) group = HIDE_LEFT_ARM;
			else if (b == BONE_RIGHT_FOREARM) group = HIDE_RIGHT_ARM;
			else if (b == BONE_LEFT_CLAVICLE || b == BONE_RIGHT_CLAVICLE) shoulder = true;
		}
		s_hideGroup[bone] = group == HIDE_NONE && shoulder ? HIDE_SHOULDER : group;
	}
	s_hideGroupsKnown = true;
}

bool InAttackOrJump() {
	DWORD now = GetTickCount();
	return now - GamepadAttackTick() < kActionMs || now - GamepadJumpTick() < kActionMs;
}

// The posed bones to hide this frame: in a group, and their joint (or the way to their first child) near `eye`.
// `armFree[side]`: that arm isn't where your controller puts it.
int32_t ChooseHidden(const pure::M4 delta[], pure::V3 eye, const bool armFree[2], bool hide[]) {
	using namespace pure;
	if (!s_hideGroupsKnown) LearnHideGroups();
	int32_t count = 0;
	for (int32_t bone = 0; bone < kSkeletonBones; bone++) {
		hide[bone] = false;
		uint8_t group = s_hideGroup[bone];
		if (group == HIDE_NONE || !s_have[bone] || !plausible_bone(s_known[bone])) continue;
		if ((group == HIDE_LEFT_ARM && !armFree[0]) || (group == HIDE_RIGHT_ARM && !armFree[1])) continue;
		float posed[12];
		move_bone(delta[bone], s_known[bone], posed);
		V3 joint = bone_point(posed, kSkeleton[bone].bind), along = joint;
		for (int32_t child = bone + 1; child < kSkeletonBones; child++)
			if (kSkeleton[child].parent == bone) {
				along = (joint + bone_point(posed, kSkeleton[child].bind)) * 0.5f;
				break;
			}
		hide[bone] = length(joint - eye) < kHideNear || length(along - eye) < kHideNear;
		count += hide[bone];
	}
	return count;
}

// Our copy of a part's list (made like the game's), filled with `bones`
void UploadHidden(ID3D11DeviceContext* c, int32_t part, ID3D11Resource* gameList, const float* bones, int32_t count) {
	D3D11_BUFFER_DESC desc;
	((ID3D11Buffer*)gameList)->GetDesc(&desc);
	ID3D11Buffer*& copy = s_hiddenLists[part];
	if (copy) {
		D3D11_BUFFER_DESC have;
		copy->GetDesc(&have);
		if (have.ByteWidth != desc.ByteWidth) {
			copy->Release();
			copy = nullptr;
		}
	}
	if (!copy) {
		ComPtr<ID3D11Device> device;
		c->GetDevice(&device);
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (!device || FAILED(device->CreateBuffer(&desc, nullptr, &copy))) {
			copy = nullptr;
			return;
		}
	}
	Upload(c, copy, bones, count);
	s_hiddenThisFrame[part] = true;
}

struct BodyReport {
	DWORD tick;
	uint32_t posed, notInEyes, noViewer, notFound;
	int32_t parts, headOnly;
	pure::BodyPoseInfo last;
	float turn;  // radians from the game's facing
} s_report;

void ReportBody() {
	if (GetTickCount() - s_report.tick < 30000) return;
	s_report.tick = GetTickCount();
	if (s_report.posed + s_report.notInEyes + s_report.noViewer + s_report.notFound)
		Log("first person body: last 30 s: posed %u frames (%d of the character's lists, %d head-only); not posed: %u not in "
			"their eyes, %u no headset pose, %u their lists not found. Last: your head %.2f m from theirs, they slid %.2f m, "
			"their waist bent %.0f deg, their body turned %.0f deg from the game's facing",
			s_report.posed, s_report.parts, s_report.headOnly, s_report.notInEyes, s_report.noViewer, s_report.notFound,
			(double)s_report.last.headDistance, (double)s_report.last.slide, (double)(s_report.last.bend * 57.29578f),
			(double)(s_report.turn * 57.29578f));
	s_report.posed = s_report.notInEyes = s_report.noViewer = s_report.notFound = 0;
}

// Poses the upper body and arms before drawing, then uploads the updated bone lists.
void PoseBody(ID3D11DeviceContext* c) {
	using namespace pure;
	if (!g_cfg.fpTracking) return;
	ReportBody();
	if (!s_wasHiding) {
		s_report.notInEyes++;
		return;
	}
	if (!s_viewer.valid || GetTickCount() - s_viewer.tick > kFreshMs) {
		s_report.noViewer++;
		return;
	}
	const float *waist = s_known[BONE_WAIST], *head = s_known[BONE_HEAD];
	if (!s_character.found || !plausible_bone(waist) || !plausible_bone(head)) {
		s_report.notFound++;
		return;
	}
	// Clamp the viewer offset to the maximum body reach.
	V3 fromEyes = s_viewer.fromCharacterEyes;
	float reach = length(fromEyes);
	if (reach > kMaxReach) fromEyes = fromEyes * (kMaxReach / reach);
	V3 eyeTarget = bone_point(head, kViewBind) + fromEyes;
	const M4& view = s_viewer.view;
	// Apply body yaw around the hips.
	const float* hips = s_known[BONE_HIPS];
	M4 bodyTurn = m4_identity();
	if (plausible_bone(hips)) {
		double now = NowMs();
		V3 hipsForward = {hips[2], hips[6], hips[10]};  // the bind pose's +z, turned
		float turn = s_bodyTurn.update(yaw_of(V3{-view.at(0, 2), 0, -view.at(2, 2)}),
									   yaw_of(V3{-s_viewer.camera.at(0, 2), 0, -s_viewer.camera.at(2, 2)}), yaw_of(hipsForward),
									   (float)((now - s_bodyTurnMs) / 1000));
		s_bodyTurnMs = now;
		s_report.turn = turn;
		bodyTurn = m4_yaw_about(turn, bone_point(hips, kSkeleton[BONE_HIPS].bind));
	}
	static M4 delta[kSkeletonBones];
	UpperBodyLock lock = {s_known, s_have, s_bodyTurn.body};
	pose_upper_body(waist, head, eyeTarget, view, bodyTurn, delta, &s_report.last, kLockUpperBody && plausible_bone(hips) ? &lock : nullptr);
	// Translate the viewer pose to the tracked eye target.
	M4 headNow = view;
	headNow.at(0, 3) = eyeTarget.x;
	headNow.at(1, 3) = eyeTarget.y;
	headNow.at(2, 3) = eyeTarget.z;
	M4 fromViewer = m4_mul(headNow, m4_rigid_inverse(view));
	const ArmBones* arms[2] = {&kLeftArm, &kRightArm};
	bool armFree[2] = {true, true};
	for (int hand = 0; hand < 2; hand++) {
		const ArmBones& arm = *arms[hand];
		if (!s_hands.valid[hand] || !plausible_bone(s_known[arm.upper]) || !plausible_bone(s_known[arm.fore]) ||
			!plausible_bone(s_known[arm.hand]) || !plausible_bone(s_known[BONE_CHEST]))
			continue;
		M4 grip = m4_mul(fromViewer, s_hands.grip[hand]);
		pose_arm(arm, s_known[arm.upper], s_known[arm.fore], s_known[arm.hand], s_known[BONE_CHEST], grip, delta);
		float posedHand[12];
		move_bone(delta[arm.hand], s_known[arm.hand], posedHand);
		V3 handAt = bone_point(posedHand, kSkeleton[arm.hand].bind);
		armFree[hand] = length(handAt - V3{grip.at(0, 3), grip.at(1, 3), grip.at(2, 3)}) > kArmFollows;
		if (!s_loggedArms) {
			s_loggedArms = true;
			Log("first person: the character's arms follow your controllers");
		}
		float curls[FINGER_COUNT];
		if (kPoseFingers && VrInputFingerCurls(hand, curls) && FingersKnown(arm)) {
			pose_fingers(arm, s_known, curls, delta);
			if (!s_loggedFingers) {
				s_loggedFingers = true;
				Log("first person: the character's fingers follow yours");
			}
		}
	}
	s_report.posed++;
	s_report.parts = s_character.parts;
	s_report.headOnly = s_character.headOnlyCount;
	static bool hide[kSkeletonBones];
	V3 eye = {view.at(0, 3), view.at(1, 3), view.at(2, 3)};
	int32_t hidden = InAttackOrJump() ? ChooseHidden(delta, eye, armFree, hide) : 0;
	if (hidden && !s_loggedHidingParts) {
		s_loggedHidingParts = true;
		Log("first person: hiding the character's legs, shoulders or arms near your eyes during an attack or jump (%d bones)", hidden);
	}
	float posed[kMaxPartBones * 12];
	for (int32_t part = 0; part < kBodyPartCount; part++) {
		const ListCopy* copy = s_character.part[part];
		if (!copy) continue;
		bool anyHidden = false;
		for (int32_t slot = 0; slot < copy->bones; slot++) {
			move_bone(delta[s_character.slots[part][slot]], copy->data + slot * 12, posed + slot * 12);
			anyHidden = anyHidden || (hidden && hide[s_character.slots[part][slot]]);
		}
		Upload(c, copy->list, posed, copy->bones);
		if (!anyHidden) continue;
		for (int32_t slot = 0; slot < copy->bones; slot++) {
			int32_t bone = s_character.slots[part][slot];
			if (!hide[bone]) continue;
			float* m = posed + slot * 12;
			V3 joint = bone_point(m, kSkeleton[bone].bind);
			for (int32_t row = 0; row < 3; row++) m[row * 4] = m[row * 4 + 1] = m[row * 4 + 2] = 0;  // its vertices collapse onto its joint
			m[3] = joint.x;
			m[7] = joint.y;
			m[11] = joint.z;
		}
		UploadHidden(c, part, copy->list, posed, copy->bones);
	}
	for (int32_t i = 0; i < s_character.headOnlyCount; i++) {
		const ListCopy* copy = s_character.headOnly[i];
		for (int32_t slot = 0; slot < copy->bones; slot++) move_bone(delta[BONE_HEAD], copy->data + slot * 12, posed + slot * 12);
		Upload(c, copy->list, posed, copy->bones);
	}
	if (!s_loggedBody) {
		s_loggedBody = true;
		Log("first person: the character's upper body follows your head");
	}
}

UINT TextureWidthHeight(ID3D11View* view, UINT* height) {
	ComPtr<ID3D11Resource> res;
	view->GetResource(&res);
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return 0;
	D3D11_TEXTURE2D_DESC desc;
	((ID3D11Texture2D*)res.Get())->GetDesc(&desc);
	*height = desc.Height;
	return desc.Width;
}
}  // namespace

void FirstPersonNewFrame() {
	s_frame++;
	s_wasHiding = s_hiding;
	s_walker = {};
	s_walkHead = s_walkFace = nullptr;
	s_boundPart = -1;
	s_copyCount = s_partCopies = s_tinyCopies = 0;
	s_character = {};
	memset(s_hiddenThisFrame, 0, sizeof(s_hiddenThisFrame));
	s_swappedPart = -1;
}

void FirstPersonOnSetVSConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT count, ID3D11Buffer* const* buffers) {
	if (s_swapping || !g_cfg.firstPerson || kSkinningSlot < start || kSkinningSlot >= start + count) return;
	ID3D11Buffer* list = buffers[kSkinningSlot - start];
	s_boundPart = -1;
	if (!list) return;
	D3D11_BUFFER_DESC desc;
	list->GetDesc(&desc);
	if (desc.ByteWidth % pure::kBoneBytes != 0) return;  // another kind of buffer (worldMtxBuffer, instancing)
	int32_t bones = (int32_t)(desc.ByteWidth / pure::kBoneBytes);
	if (!s_character.found && bones == pure::kBodyPartBones[pure::kHeadPart]) {  // their body? (all their uploads are done by now)
		const ListCopy* body = CopyOf(list);
		if (body && body->bones == bones) {
			FindCharacter(body);
			if (s_character.found) PoseBody(c);
		}
	}
	Label* label = LabelOf(list);
	if (!label) return;
	if (label->frame != s_frame) Identify(label, list, bones);
	int32_t byContents = CharacterPartOf(list);  // known from the first part on (the walk confirms only at the third)
	s_boundPart = byContents >= 0 ? byContents : (label->confirmed ? label->part : -1);
}

void FirstPersonOnMap(ID3D11Resource* res, void* data, UINT byteWidth) {
	if (!g_cfg.firstPerson || !data || s_uploading || byteWidth % pure::kBoneBytes || !CouldBeCharacters((int32_t)(byteWidth / pure::kBoneBytes)))
		return;
	PendingMap* slot = nullptr;
	for (auto& pending : s_pending)
		if (pending.list == res || (!pending.list && !slot)) slot = &pending;
	if (!slot) {  // a map never unmapped: its place is taken
		slot = &s_pending[s_pendingNext];
		s_pendingNext = (s_pendingNext + 1) % (int32_t)std::size(s_pending);
	}
	*slot = {res, (float*)data, byteWidth};
}

void FirstPersonBeforeUnmap(ID3D11Resource* res) {
	if (s_uploading) return;
	for (auto& pending : s_pending) {
		if (pending.list != res) continue;
		TakeCopy(res, pending.data, (int32_t)(pending.byteWidth / pure::kBoneBytes));
		pending.list = nullptr;
		return;
	}
}

void FirstPersonOnSetRenderTargets(UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
	if (!g_cfg.firstPerson) return;
	ID3D11View* view = count && rtvs && rtvs[0] ? (ID3D11View*)rtvs[0] : (ID3D11View*)dsv;
	UINT height = 0, width = view ? TextureWidthHeight(view, &height) : 0;
	s_mainTarget = width && width == s_gameW && height == s_gameH;
}

bool FirstPersonSkipDraw() {
	if (!s_hiding || !g_cfg.fpTracking || !s_mainTarget || s_boundPart < 0 || !pure::is_head_only_part(s_boundPart)) return false;
	if (!s_loggedHiding) {
		s_loggedHiding = true;
		Log("first person: hiding the character's head in the main view (their shadow keeps it)");
	}
	return true;
}

void FirstPersonBeforeDraw(ID3D11DeviceContext* c) {
	int32_t part = s_boundPart;
	if (!s_mainTarget || !s_hiding || part < 0 || part >= pure::kBodyPartCount || !s_hiddenThisFrame[part] || !s_hiddenLists[part]) return;
	s_swapping = true;
	c->VSSetConstantBuffers(kSkinningSlot, 1, &s_hiddenLists[part]);
	s_swapping = false;
	s_swappedPart = part;
}

void FirstPersonAfterDraw(ID3D11DeviceContext* c) {
	if (s_swappedPart < 0) return;
	const ListCopy* copy = s_character.part[s_swappedPart];
	s_swappedPart = -1;
	if (!copy) return;
	ID3D11Buffer* game = (ID3D11Buffer*)copy->list;
	s_swapping = true;
	c->VSSetConstantBuffers(kSkinningSlot, 1, &game);
	s_swapping = false;
}

void FirstPersonSetGameSize(UINT width, UINT height) {
	s_gameW = width;
	s_gameH = height;
}

bool FirstPersonHead(pure::HeadPose* out) {
	if (!s_tracked.valid || GetTickCount() - s_tracked.tick > kFreshMs) return false;
	*out = s_tracked.head;
	return true;
}

bool FirstPersonSteadyEyeHeight(float* out) {
	if (!s_tracked.groundValid || GetTickCount() - s_tracked.groundTick > kFreshMs) return false;
	*out = s_tracked.steadyEyeY;
	return true;
}

void FirstPersonSetHiding(bool hide) { s_hiding = hide; }

void FirstPersonSetHands(const bool valid[2], const pure::M4 grips[2]) {
	for (int hand = 0; hand < 2; hand++) {
		s_hands.valid[hand] = valid[hand];
		if (valid[hand]) s_hands.grip[hand] = grips[hand];
	}
}

bool FirstPersonFacing(float* yaw) {
	if (!s_feet.tick || GetTickCount() - s_feet.tick > kFreshMs) return false;
	*yaw = s_feet.facing;
	return true;
}

bool FirstPersonFeet(pure::V3* out) {
	if (!s_feet.tick || GetTickCount() - s_feet.tick > kFreshMs) return false;
	*out = s_feet.at;
	return true;
}

void FirstPersonSetViewer(const pure::M4& view, const pure::M4& camera, pure::V3 fromCharacterEyes) {
	s_viewer = {true, view, camera, fromCharacterEyes, GetTickCount()};
}
