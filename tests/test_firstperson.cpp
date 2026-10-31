// First person: 2B's eyes, body and arms from her bone lists (game/head_bone.h, game/body_pose.h).
#include <cstdio>
#include <cmath>
#include "core/pure.h"
#include "game/body_pose.h"
#include "game/body_skeleton.h"
#include "game/head_bone.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
static bool near3(const float* a, float x, float y, float z) {
	return std::fabs(a[0] - x) < 1e-4f && std::fabs(a[1] - y) < 1e-4f && std::fabs(a[2] - z) < 1e-4f;
}
int main() {
	// her parts, as their lists are first bound in a frame, among other characters' lists
	{
		pure::BodyPartWalker walker;
		check(walker.identify(10) == -1, "9S's first part is not hers");
		check(walker.identify(59) == 0 && !walker.confirmed(), "59 bones: maybe her part 0");
		check(walker.identify(1) == 1, "then her face (part 1)");
		check(walker.identify(15) == 2 && walker.confirmed(), "three in a row: confirmed");
		bool inOrder = true;
		for (int part = 3; part < pure::kBodyPartCount; part++) inOrder &= walker.identify(pure::kBodyPartBones[part]) == part;
		check(inOrder, "her remaining parts follow");
		check(walker.identify(63) == -1, "after her last part, a pod's 63 is not hers");
		int first = walker.identify(59);
		int second = walker.identify(2);
		check(first == 0 && second == -1 && !walker.confirmed(), "59 then a wrong size: not her");
	}
	{
		// after self-destructing: 59 1 25 2 23 20 14 11 14 8 (her dress, parts 2-4 and 6-7, is gone)
		pure::BodyPartWalker walker;
		const int bones[] = {59, 1, 25, 2, 23, 20, 14, 11, 14, 8};
		const int parts[] = {0, 1, 5, 8, 9, 10, 11, 12, 13, 14};
		bool all = true;
		for (int i = 0; i < 10; i++) all &= walker.identify(bones[i]) == parts[i];
		check(all && walker.confirmed(), "without her dress: the same parts, the dress skipped");
		pure::BodyPartWalker other;
		other.identify(59);
		check(other.identify(25) == -1, "her face can't be skipped");
	}
	check(pure::is_head_only_part(1) && pure::is_head_only_part(9) && pure::is_head_only_part(13) && !pure::is_head_only_part(0) &&
			  !pure::is_head_only_part(2),
		  "head-only parts: face, hair, face details; not her body");

	// her head bone: turned 90 degrees (facing +X) and moved
	{
		const float m[12] = {0, 0, 1, 10, 0, 1, 0, 2, -1, 0, 0, 5};  // rows: x' = z + 10, y' = y + 2, z' = -x + 5
		check(pure::plausible_bone(m), "a rotation is a plausible bone");
		pure::HeadPose head = pure::head_pose(m, pure::kEyeBind);
		const float eyeAt[3] = {head.eye.x, head.eye.y, head.eye.z};
		check(near3(eyeAt, 10 + pure::kEyeBind.z, 2 + pure::kEyeBind.y, 5), "eyes: the bind position moved with the bone");
		const float facing[3] = {head.forward.x, head.forward.y, head.forward.z};
		check(near3(facing, 1, 0, 0), "facing: the bone's +z");
		const float collapsed[12] = {};
		check(!pure::plausible_bone(collapsed), "a collapsed bone is not");
	}

	// the ground under her: standing, one foot up, both feet up (a jump)
	{
		const float standing[12] = {1, 0, 0, 0, 0, 1, 0, 9.5f, 0, 0, 1, 0};
		const float lifted[12] = {1, 0, 0, 0, 0, 1, 0, 9.8f, 0, 0, 1, 0};
		const float jumped[12] = {1, 0, 0, 0, 0, 1, 0, 11.0f, 0, 0, 1, 0};
		check(std::fabs(pure::ground_height(standing, standing) - 9.5f) < 1e-4f, "standing: the ground under her feet");
		check(std::fabs(pure::ground_height(lifted, standing) - 9.5f) < 1e-4f, "one foot up: the other one decides");
		check(std::fabs(pure::ground_height(jumped, jumped) - 11.0f) < 1e-4f, "both feet up: the ground follows the jump");
	}

	// her skeleton (tools/wmb_skeleton.py) agrees with what the captures showed
	{
		bool sizes = std::size(pure::kPartSlots) == (size_t)pure::kBodyPartCount;
		for (int32_t part = 0; sizes && part < pure::kBodyPartCount; part++) sizes = pure::kPartSlots[part].count == pure::kBodyPartBones[part];
		check(sizes && std::size(pure::kPart13SeriousSlots) == (size_t)pure::kBodyPartBones[13], "each part's slots match its bone list's size");
		auto bind = [](const uint8_t* slots, int32_t slot) { return pure::kSkeleton[slots[slot]].bind; };
		auto same = [](pure::V3 a, pure::V3 b) { return pure::length(a - b) < 1e-3f; };
		check(pure::kPart0Slots[pure::kHeadSlot] == pure::BONE_HEAD && same(bind(pure::kPart0Slots, pure::kHeadSlot), pure::kNeckPivotBind),
			  "part 0's head slot is her head bone, at the neck pivot");
		check(pure::kPart14Slots[pure::kLeftAnkleSlot] == pure::BONE_LEFT_ANKLE && pure::kPart14Slots[pure::kRightAnkleSlot] == pure::BONE_RIGHT_ANKLE &&
				  same(bind(pure::kPart14Slots, pure::kLeftAnkleSlot), pure::kLeftAnkleBind) &&
				  same(bind(pure::kPart14Slots, pure::kRightAnkleSlot), pure::kRightAnkleBind),
			  "part 14's ankle slots are her ankles");
		bool parentsFirst = true;
		for (int32_t bone = 0; bone < pure::kSkeletonBones; bone++) parentsFirst = parentsFirst && pure::kSkeleton[bone].parent < bone;
		check(parentsFirst && pure::kSkeleton[pure::BONE_LEFT_HAND].parent == pure::BONE_LEFT_FOREARM &&
				  pure::kSkeleton[pure::BONE_PELVIS].parent == pure::BONE_HIPS && pure::kSkeleton[pure::BONE_WAIST].parent == pure::BONE_HIPS,
			  "parents come before their children; arms, upper and lower body hang where expected");
	}

	// her upper body follows your head (bones in the bind pose: every matrix the identity)
	{
		const float bind[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
		pure::M4 facingHer = pure::m4_identity();  // a camera looking along her +z
		facingHer.at(0, 0) = facingHer.at(2, 2) = -1;
		static pure::M4 delta[pure::kSkeletonBones];
		auto moved = [](const pure::M4& d, pure::V3 p) {
			return pure::V3{d.at(0, 0) * p.x + d.at(0, 1) * p.y + d.at(0, 2) * p.z + d.at(0, 3), d.at(1, 0) * p.x + d.at(1, 1) * p.y + d.at(1, 2) * p.z + d.at(1, 3),
							d.at(2, 0) * p.x + d.at(2, 1) * p.y + d.at(2, 2) * p.z + d.at(2, 3)};
		};
		auto at = [&](int32_t bone, pure::V3 p) { return moved(delta[bone], p); };
		auto same = [](pure::V3 a, pure::V3 b) { return pure::length(a - b) < 1e-3f; };
		const pure::V3 ankle = pure::kLeftAnkleBind, neck = pure::kNeckPivotBind, eye = pure::kViewBind;

		pure::pose_upper_body(bind, bind, eye, facingHer, pure::m4_identity(), delta);
		bool still = true;
		for (int32_t bone = 0; bone < pure::kSkeletonBones; bone++) still = still && same(at(bone, pure::kSkeleton[bone].bind), pure::kSkeleton[bone].bind);
		check(still, "your head where hers is: nothing moves");

		pure::pose_upper_body(bind, bind, eye + pure::V3{0, 0, 0.1f}, facingHer, pure::m4_identity(), delta);
		check(same(at(pure::BONE_HEAD, eye), eye + pure::V3{0, 0, 0.1f}), "leaning 10 cm forward: her eyes are at yours");
		check(same(at(pure::BONE_LEFT_ANKLE, ankle), ankle), "... her feet stay");
		check(pure::length(at(pure::BONE_NECK, neck) - (neck + pure::V3{0, 0, 0.1f})) < 0.01f, "... her waist bends so her neck follows");

		pure::pose_upper_body(bind, bind, eye + pure::V3{1, 0, 0}, facingHer, pure::m4_identity(), delta);
		check(same(at(pure::BONE_LEFT_ANKLE, ankle), ankle + pure::V3{1 - pure::kLeanRadius, 0, 0}) && same(at(pure::BONE_HEAD, eye), eye + pure::V3{1, 0, 0}),
			  "1 m away: all of her slides along, her head on yours");

		pure::M4 lookingRight = pure::m4_identity();  // a camera looking along her -x
		lookingRight.at(0, 2) = 1;
		lookingRight.at(2, 0) = -1;
		lookingRight.at(0, 0) = lookingRight.at(2, 2) = 0;
		pure::pose_upper_body(bind, bind, eye, lookingRight, pure::m4_identity(), delta);
		pure::V3 nose = at(pure::BONE_HEAD, eye + pure::V3{0, 0, 0.1f});
		check(same(at(pure::BONE_HEAD, eye), eye) && same(nose, eye + pure::V3{-0.1f, 0, 0}) && same(at(pure::BONE_LEFT_ANKLE, ankle), ankle),
			  "turning your head: hers turns about her eyes, her body stays");
		check(same(at(pure::kPart9Slots[0], pure::kSkeleton[pure::kPart9Slots[0]].bind), moved(delta[pure::BONE_HEAD], pure::kSkeleton[pure::kPart9Slots[0]].bind)),
			  "her hair turns with her head");

		float out[12];
		pure::move_bone(delta[pure::BONE_HEAD], bind, out);
		check(same(pure::bone_point(out, eye + pure::V3{0, 0, 0.1f}), nose), "a moved bone list's matrix moves its points the same way");

		{
			// locked: an animated chest (bent 30 degrees forward) stands upright again; its arm keeps hanging off it
			static float known[pure::kSkeletonBones][12];
			static bool have[pure::kSkeletonBones];
			for (int32_t bone = 0; bone < pure::kSkeletonBones; bone++) {
				memcpy(known[bone], bind, sizeof(bind));
				have[bone] = true;
			}
			pure::M4 bent = pure::m4_turn_about(pure::m4_from_pose({0.2588f, 0, 0, 0.9659f}, {0, 0, 0}), pure::kSkeleton[pure::BONE_CHEST].bind,
												pure::kSkeleton[pure::BONE_CHEST].bind);
			for (int32_t i = 0; i < 12; i++) known[pure::BONE_CHEST][i] = bent.m[i];
			pure::UpperBodyLock lock = {known, have, 0};
			pure::pose_upper_body(bind, bind, eye, facingHer, pure::m4_identity(), delta, nullptr, &lock);
			float chest[12];
			pure::move_bone(delta[pure::BONE_CHEST], known[pure::BONE_CHEST], chest);
			pure::V3 top = pure::kSkeleton[pure::BONE_NECK].bind;
			check(same(pure::bone_point(chest, top), top), "locked: the bent chest stands upright again");
			check(same(at(pure::BONE_LEFT_ANKLE, ankle), ankle), "... her feet stay");
		}

		const pure::V3 hips = pure::kSkeleton[pure::BONE_HIPS].bind;
		pure::pose_upper_body(bind, bind, eye, facingHer, pure::m4_yaw_about(pure::kPi / 2, hips), delta);
		pure::V3 ankleTurned = hips + pure::V3{ankle.z - hips.z, ankle.y - hips.y, -(ankle.x - hips.x)};
		check(same(at(pure::BONE_LEFT_ANKLE, ankle), ankleTurned) && same(at(pure::BONE_HEAD, eye), eye),
			  "her body turned a quarter about her hips (her feet too), her head still on yours");
	}

	// her arms on your controllers (bones in the bind pose: every matrix the identity)
	{
		const float bind[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
		static pure::M4 delta[pure::kSkeletonBones];
		auto same = [](pure::V3 a, pure::V3 b, float within) { return pure::length(a - b) < within; };
		const pure::ArmBones& arm = pure::kLeftArm;
		pure::V3 shoulderBind = pure::kSkeleton[arm.upper].bind, elbowBind = pure::kSkeleton[arm.fore].bind, wristBind = pure::kSkeleton[arm.hand].bind;
		float upperLength = pure::length(elbowBind - shoulderBind), foreLength = pure::length(wristBind - elbowBind);

		for (auto& d : delta) d = pure::m4_identity();
		pure::M4 grip = pure::m4_identity();  // in front of her left shoulder, the barrel along -z
		grip.at(0, 3) = 0.25f;
		grip.at(1, 3) = 1.1f;
		grip.at(2, 3) = 0.15f;
		pure::HandTarget target = pure::hand_target(grip);
		pure::pose_arm(arm, bind, bind, bind, bind, grip, delta);
		pure::V3 shoulder = pure::moved_point(delta[arm.upper], shoulderBind), elbow = pure::moved_point(delta[arm.fore], elbowBind);
		pure::V3 wrist = pure::moved_point(delta[arm.hand], wristBind);
		check(same(wrist, target.wrist, 1e-3f), "her wrist is at the controller (behind your palm)");
		check(same(shoulder, shoulderBind, 1e-4f), "her shoulder stays");
		check(std::fabs(pure::length(elbow - shoulder) - upperLength) < 1e-3f && std::fabs(pure::length(wrist - elbow) - foreLength) < 1e-3f,
			  "her upper arm and forearm keep their lengths");
		check(same(pure::moved_point(delta[arm.upper], elbowBind), elbow, 1e-3f), "the forearm turns about the elbow the upper arm put it at");
		check(elbow.y < (shoulder.y + wrist.y) / 2, "her elbow bends down");
		pure::V3 knuckle = pure::moved_point(delta[arm.hand], pure::kSkeleton[arm.fingers[pure::FINGER_MIDDLE]].bind);
		check(pure::dot(pure::normalized(knuckle - wrist), pure::normalized(target.forward)) > 0.999f, "her hand points the way the controller says");
		bool fingers = true;
		for (int32_t bone = 0; bone < pure::kSkeletonBones; bone++)
			if (pure::kSkeleton[bone].parent == arm.hand)
				for (int i = 0; i < 16; i++) fingers = fingers && delta[bone].m[i] == delta[arm.hand].m[i];
		check(fingers, "her fingers go with her hand");
		bool otherArm = true;
		for (int i = 0; i < 16; i++) otherArm = otherArm && delta[pure::kRightArm.hand].m[i] == pure::m4_identity().m[i];
		check(otherArm, "the other arm stays");

		// rolling the controller: her arm takes the twist (the wrist helper all of it), her wrist only swings
		auto twistFrom = [&](int32_t bone) {  // the hand's twist about the forearm, from this bone (bind pose: the deltas are the lists)
			pure::M4 from = delta[bone], handList = delta[arm.hand];
			pure::V3 axis = pure::moved_point(delta[arm.fore], wristBind) - pure::moved_point(delta[arm.fore], elbowBind);
			return pure::twist_about(pure::m4_mul(handList, pure::m4_rotation_transposed(from)), axis);
		};
		check(!pure::kTwistForearm || std::fabs(twistFrom(arm.wristTwist)) < 1e-3f, "no twist at her wrist");
		pure::V3 elbowUnrolled = elbow;
		pure::M4 rolled = pure::m4_mul(pure::m4_turn(target.forward, 1.0f, pure::m4_column(grip, 3)), grip);
		pure::pose_arm(arm, bind, bind, bind, bind, rolled, delta);
		pure::HandTarget rolledTarget = pure::hand_target(rolled);
		wrist = pure::moved_point(delta[arm.hand], wristBind);
		knuckle = pure::moved_point(delta[arm.hand], pure::kSkeleton[arm.fingers[pure::FINGER_MIDDLE]].bind);
		pure::V3 indexKnuckle = pure::moved_point(delta[arm.hand], pure::kSkeleton[arm.fingers[pure::FINGER_INDEX]].bind);
		pure::V3 littleKnuckle = pure::moved_point(delta[arm.hand], pure::kSkeleton[arm.fingers[pure::FINGER_LITTLE]].bind);
		pure::V3 across = littleKnuckle - indexKnuckle;
		pure::V3 acrossWanted = rolledTarget.across - rolledTarget.forward * pure::dot(rolledTarget.across, pure::normalized(rolledTarget.forward));
		check(same(wrist, rolledTarget.wrist, 1e-3f) && pure::dot(pure::normalized(knuckle - wrist), pure::normalized(rolledTarget.forward)) > 0.999f &&
				  pure::dot(pure::normalized(across - rolledTarget.forward * pure::dot(across, pure::normalized(rolledTarget.forward))),
							pure::normalized(acrossWanted)) > 0.999f,
			  "a rolled controller: her hand still turned exactly like it");
		check(!pure::kTwistForearm || std::fabs(twistFrom(arm.wristTwist)) < 1e-3f, "... the roll in her arm, none at her wrist");
		float atElbowEnd = twistFrom(arm.fore), atMiddle = twistFrom(arm.foreTwist);
		check(!pure::kTwistForearm || (std::fabs(atElbowEnd) > std::fabs(atMiddle) + 0.05f && std::fabs(atMiddle) > 0.05f),
			  "... more of it towards her wrist (less left to twist there)");
		pure::V3 elbowRolled = pure::moved_point(delta[arm.fore], elbowBind);
		check(!pure::kTwistForearm || pure::length(elbowRolled - elbowUnrolled) > 0.01f, "... her elbow swings a little");
		check(std::fabs(pure::length(elbowRolled - shoulderBind) - upperLength) < 1e-3f &&
				  std::fabs(pure::length(wrist - elbowRolled) - foreLength) < 1e-3f,
			  "... her arm keeps its lengths");
		check(same(pure::moved_point(delta[arm.fore], elbowBind), pure::moved_point(delta[arm.upper], elbowBind), 1e-3f),
			  "... her forearm still turns about her elbow");

		grip.at(0, 3) = 1.5f;  // far out of reach to her left
		pure::pose_arm(arm, bind, bind, bind, bind, grip, delta);
		wrist = pure::moved_point(delta[arm.hand], wristBind);
		check(std::fabs(pure::length(wrist - shoulderBind) - (upperLength + foreLength) * pure::kStraightArm) < 1e-3f,
			  "out of reach: her arm straight, towards the controller");
	}

	// her fingers on yours (bones in the bind pose, her hand where pose_arm put it)
	{
		static float bones[pure::kSkeletonBones][12];
		for (auto& bone : bones) {
			for (float& v : bone) v = 0;
			bone[0] = bone[5] = bone[10] = 1;
		}
		static pure::M4 delta[pure::kSkeletonBones];
		auto same = [](pure::V3 a, pure::V3 b) { return pure::length(a - b) < 1e-4f; };
		const pure::ArmBones* arms[2] = {&pure::kLeftArm, &pure::kRightArm};
		const char* sides[2] = {"left", "right"};
		for (int side = 0; side < 2; side++) {
			const pure::ArmBones& arm = *arms[side];
			for (auto& d : delta) d = pure::m4_identity();
			const float open[5] = {0, 0, 0, 0, 0}, fist[5] = {1, 1, 1, 1, 1};
			char what[128];
			pure::pose_fingers(arm, bones, open, delta);
			bool still = true;
			for (int finger = 0; finger < pure::FINGER_COUNT; finger++)
				for (int joint = 0; joint < pure::finger_joints(finger); joint++) {
					int bone = arm.fingers[finger] + joint;
					still = still && same(pure::moved_point(delta[bone], pure::kSkeleton[bone].bind), pure::kSkeleton[bone].bind);
				}
			std::snprintf(what, sizeof(what), "%s hand open: her fingers as in her bind pose", sides[side]);
			check(still, what);

			pure::pose_fingers(arm, bones, fist, delta);
			pure::V3 palm = pure::palm_side(arm), wrist = pure::kSkeleton[arm.hand].bind;
			for (int finger = 0; finger < pure::FINGER_COUNT; finger++) {
				int tip = arm.fingers[finger] + pure::finger_joints(finger) - 1, first = arm.fingers[finger];
				pure::V3 tipBind = pure::kSkeleton[tip].bind, tipNow = pure::moved_point(delta[tip], tipBind);
				pure::V3 firstNow = pure::moved_point(delta[first], pure::kSkeleton[first].bind);
				std::snprintf(what, sizeof(what), "%s fist, finger %d: its tip goes to her palm's side and nearer her wrist", sides[side], finger);
				check(pure::dot(tipNow - tipBind, palm) > 0.005f && pure::length(tipNow - wrist) < pure::length(tipBind - wrist), what);
				if (finger != pure::FINGER_THUMB) {
					std::snprintf(what, sizeof(what), "%s fist, finger %d: its tip lands on her palm, not past it", sides[side], finger);
					check(pure::length(tipNow - wrist) > 0.03f, what);
				}
				std::snprintf(what, sizeof(what), "%s fist, finger %d: its first joint stays on her hand", sides[side], finger);
				check(same(firstNow, pure::kSkeleton[first].bind), what);
			}
			int index = arm.fingers[pure::FINGER_INDEX];
			pure::V3 a = pure::moved_point(delta[index], pure::kSkeleton[index].bind), b = pure::moved_point(delta[index + 1], pure::kSkeleton[index + 1].bind);
			std::snprintf(what, sizeof(what), "%s fist: her finger bones keep their lengths", sides[side]);
			check(std::fabs(pure::length(b - a) - pure::length(pure::kSkeleton[index + 1].bind - pure::kSkeleton[index].bind)) < 1e-4f, what);
		}
	}

	// her body turning after your head
	{
		check(std::fabs(pure::yaw_of({0, 0, 1})) < 1e-5f && std::fabs(pure::yaw_of({1, 0, 0}) - pure::kPi / 2) < 1e-5f &&
				  std::fabs(std::fabs(pure::yaw_of({0, 0, -1})) - pure::kPi) < 1e-5f && std::fabs(pure::yaw_of({-1, 0, -1}) + 3 * pure::kPi / 4) < 1e-4f,
			  "yaw: 0 along +z, positive towards +x");
		pure::M4 turn = pure::m4_yaw_about(0.3f, {0, 0, 0});
		check(std::fabs(pure::yaw_of(pure::moved_point(turn, {0, 0, 1})) - 0.3f) < 1e-5f, "a yaw turn turns that way");

		const float dt = 1.0f / 60;
		pure::BodyTurn body;
		check(std::fabs(body.update(0.5f, 0, 0.1f, dt) - 0.4f) < 1e-5f, "first frame: her body faces where you look");
		float turned = 0;
		for (int frame = 0; frame < 60; frame++) turned = body.update(0.5f + 0.2f, 0, 0.1f, dt);
		check(std::fabs(turned - 0.4f) < 1e-4f, "your head turned a little: her body stays");
		for (int frame = 0; frame < 60; frame++) turned = body.update(0.5f + 1.2f, 0, 0.1f, dt);
		check(std::fabs(body.body - (0.5f + 1.2f - pure::kNeckFree)) < 0.01f, "turned further and held: her body follows to within the free turn");
		float before = body.body;
		body.update(0.5f + 1.2f + 1.0f, 1.0f, 0.1f, dt);
		check(std::fabs(pure::wrap_angle(body.body - before - 1.0f)) < 1e-3f, "the stick turns her body at once");
		body.update(0.5f + 1.2f + 1.0f + 1.5f, 1.0f, 0.1f, dt);
		check(pure::wrap_angle(0.5f + 1.2f + 1.0f + 1.5f - body.body) <= pure::kNeckMax + 1e-4f, "a sudden turn: never further than the most a neck turns");

		pure::BodyTurn across;
		across.update(3.0f, 0, 3.0f, dt);
		for (int frame = 0; frame < 120; frame++) across.update(-3.0f, 0, 3.0f, dt);  // 0.28 rad on, across +-pi
		check(std::fabs(pure::wrap_angle(across.body - (-3.0f - pure::kNeckFree))) < 0.01f, "turning across +-pi: the short way");
	}

	// steady height: running bob filtered out, a jump followed
	{
		pure::SteadyHeight steady;
		const float dt = 1.0f / 60;
		float y = steady.update(1.56f, 0, dt);
		check(std::fabs(y - 1.56f) < 1e-4f, "starts at her eyes");
		float lowest = 10, highest = -10;
		for (int frame = 0; frame < 180; frame++) {  // 3 s of running: head bobbing 5 cm at 3 Hz
			float bob = 0.05f * std::sin(frame * dt * 3 * 2 * 3.14159f);
			y = steady.update(1.56f + bob, 0, dt);
			if (frame > 60) {
				lowest = y < lowest ? y : lowest;
				highest = y > highest ? y : highest;
			}
		}
		check(highest - lowest < 0.02f, "running bob: under 2 cm left of 10");
		for (int frame = 0; frame < 30; frame++) y = steady.update(1.56f + 1.0f, 1.0f, dt);  // half a second up a 1 m jump
		check(y > 2.45f, "a jump: followed within half a second");
	}
	if (!fails) std::printf("first person tests passed\n");
	return fails ? 1 : 0;
}
