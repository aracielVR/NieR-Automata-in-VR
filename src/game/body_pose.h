// Mostly functions related to posing the armature
#pragma once
#include <math.h>
#include <stdint.h>
#include "core/vecmath.h"
#include "game/body_skeleton.h"
#include "game/head_bone.h"
#include "input/finger_curl.h"

namespace pure {

inline M4 m4_from_bone(const float m[12]) {
	M4 r = m4_identity();
	for (int i = 0; i < 12; i++) r.m[i] = m[i];
	return r;
}
inline void move_bone(const M4& delta, const float in[12], float out[12]) {
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			out[r * 4 + c] = delta.at(r, 0) * in[c] + delta.at(r, 1) * in[4 + c] + delta.at(r, 2) * in[8 + c] + (c == 3 ? delta.at(r, 3) : 0);
}

inline M4 m4_turn_about(const M4& rotation, V3 from, V3 to) {
	M4 r = rotation;
	V3 turned = {r.at(0, 0) * from.x + r.at(0, 1) * from.y + r.at(0, 2) * from.z, r.at(1, 0) * from.x + r.at(1, 1) * from.y + r.at(1, 2) * from.z,
				 r.at(2, 0) * from.x + r.at(2, 1) * from.y + r.at(2, 2) * from.z};
	r.at(0, 3) = to.x - turned.x;
	r.at(1, 3) = to.y - turned.y;
	r.at(2, 3) = to.z - turned.z;
	r.at(3, 0) = r.at(3, 1) = r.at(3, 2) = 0;
	r.at(3, 3) = 1;
	return r;
}

inline V3 moved_point(const M4& m, V3 p) {
	return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3), m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
			m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}

inline Q q_towards(V3 a, V3 b, float maxRadians) {
	float la = length(a), lb = length(b);
	if (la < 1e-6f || lb < 1e-6f) return {0, 0, 0, 1};
	a = a / la;
	b = b / lb;
	V3 axis = cross(a, b);
	float sine = length(axis), cosine = dot(a, b);
	if (sine < 1e-6f) return {0, 0, 0, 1};  // already there (or exactly opposite: left alone)
	float angle = 2 * fatan(sine / (1 + cosine));  // tan(angle / 2)
	if (angle > maxRadians) angle = maxRadians;
	float s, c;
	fsincos(angle / 2, &s, &c);
	axis = axis / sine;
	return {axis.x * s, axis.y * s, axis.z * s, c};
}

inline constexpr int32_t slot_of(const uint8_t* slots, int32_t count, int32_t bone) {
	for (int32_t i = 0; i < count; i++)
		if (slots[i] == bone) return i;
	return -1;
}

inline M4 m4_yaw_about(float radians, V3 pivot) {
	float s, c;
	fsincos(radians / 2, &s, &c);
	return m4_turn_about(m4_from_pose({0, s, 0, c}, {0, 0, 0}), pivot, pivot);
}

inline constexpr float kNeckFree = 0.26f;          // Free head yaw before body follow, radians.
inline constexpr float kNeckMax = 0.79f;           // Maximum head-to-body yaw, radians.
inline constexpr float kBodyFollowSeconds = 0.2f;  // Body follow time constant, seconds.
inline constexpr float kFacingSeconds = 0.3f;      // Game-facing smoothing time constant, seconds.

struct BodyTurn {
	bool valid = false;
	float body = 0;    // her body's yaw (world)
	float stick = 0;   // the camera's own yaw last time (the right stick)
	float facing = 0;  // the game's facing for her (her hips, smoothed)

	// Returns body yaw from head yaw, camera yaw, game facing, and elapsed time.
	float update(float headYaw, float stickYaw, float hipsYaw, float seconds) {
		if (!valid || seconds > 0.5f) {  // first time, or after a pause: facing where you look
			valid = true;
			body = headYaw;
			stick = stickYaw;
			facing = hipsYaw;
			return wrap_angle(body - facing);
		}
		body = wrap_angle(body + wrap_angle(stickYaw - stick));  // the stick turns her with the camera
		stick = stickYaw;
		float apart = wrap_angle(headYaw - body);
		float beyond = apart > kNeckFree ? apart - kNeckFree : (apart < -kNeckFree ? apart + kNeckFree : 0);
		body = wrap_angle(body + beyond * (1 - expf(-seconds / kBodyFollowSeconds)));
		apart = wrap_angle(headYaw - body);
		if (apart > kNeckMax) body = wrap_angle(headYaw - kNeckMax);
		if (apart < -kNeckMax) body = wrap_angle(headYaw + kNeckMax);
		facing = wrap_angle(facing + wrap_angle(hipsYaw - facing) * (1 - expf(-seconds / kFacingSeconds)));
		return wrap_angle(body - facing);
	}
};

inline constexpr float kLeanRadius = 0.25f;  // Maximum horizontal neck offset before body translation, metres.
inline constexpr float kMaxBend = 1.0f;      // Maximum waist bend, radians.

// Measurements recorded after applying a body pose.
struct BodyPoseInfo {
	float headDistance, slide, bend;
};

// The upper body locked: these bones stand as in the bind pose instead of the game's animation, turned to
// `yaw` (world, yaw_of) and leaned towards your head. What hangs off them (arms, hair, clothes) follows.
inline constexpr int32_t kLockedBones[] = {BONE_WAIST, BONE_SPINE, BONE_CHEST, BONE_NECK, BONE_LEFT_CLAVICLE, BONE_RIGHT_CLAVICLE};
struct UpperBodyLock {
	const float (*known)[12];  // the game's matrices for the bones (by kSkeleton index)
	const bool* have;          // ... which are known
	float yaw;
};

// Computes world-space bone deltas to place the head at eyeTarget and orient it like view.
inline void pose_upper_body(const float waist[12], const float head[12], V3 eyeTarget, const M4& view, const M4& bodyTurn,
							M4 delta[kSkeletonBones], BodyPoseInfo* info = nullptr, const UpperBodyLock* lock = nullptr) {
	// The head bone faces +Z; the camera faces -Z.
	M4 headTurn = m4_identity();
	for (int r = 0; r < 3; r++) {
		headTurn.at(r, 0) = -view.at(r, 0);
		headTurn.at(r, 1) = view.at(r, 1);
		headTurn.at(r, 2) = -view.at(r, 2);
	}
	M4 gameHead = m4_identity();  // the game's head rotation, inverted (transposed)
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++) gameHead.at(r, c) = head[c * 4 + r];
	V3 eye = bone_point(head, kViewBind);
	M4 headDelta = m4_turn_about(m4_mul(headTurn, gameHead), eye, eyeTarget);

	V3 neckGame = bone_point(head, kNeckPivotBind), neck = moved_point(bodyTurn, neckGame);
	V3 neckTarget = moved_point(headDelta, neckGame);
	V3 away = {neckTarget.x - neck.x, 0, neckTarget.z - neck.z};
	float distance = length(away);
	V3 slide = distance > kLeanRadius ? away * (1 - kLeanRadius / distance) : V3{0, 0, 0};
	M4 base = bodyTurn;  // all of her: turned, then slid
	base.at(0, 3) += slide.x;
	base.at(2, 3) += slide.z;

	V3 waistPivot = moved_point(base, bone_point(waist, kSkeleton[BONE_WAIST].bind));
	Q bend = q_towards(neck + slide - waistPivot, neckTarget - waistPivot, kMaxBend);
	M4 waistDelta = m4_mul(m4_turn_about(m4_from_pose(bend, {0, 0, 0}), waistPivot, waistPivot), base);
	// Locked: the bind pose's upper body, its waist on the game's, turned to the body's yaw, then leaned
	M4 upright = m4_identity();
	bool locked = lock && lock->have[BONE_WAIST] && plausible_bone(lock->known[BONE_WAIST]);
	if (locked) {
		V3 waistBind = kSkeleton[BONE_WAIST].bind;
		M4 placed = m4_identity();
		placed.at(0, 3) = waistPivot.x - waistBind.x;
		placed.at(1, 3) = waistPivot.y - waistBind.y;
		placed.at(2, 3) = waistPivot.z - waistBind.z;
		placed = m4_mul(m4_yaw_about(lock->yaw, waistPivot), placed);
		bend = q_towards(moved_point(placed, kNeckPivotBind) - waistPivot, neckTarget - waistPivot, kMaxBend);
		upright = m4_mul(m4_turn_about(m4_from_pose(bend, {0, 0, 0}), waistPivot, waistPivot), placed);
	}
	if (info) *info = {length(eyeTarget - eye), length(slide), 2 * fatan(length(V3{bend.x, bend.y, bend.z}) / bend.w)};

	for (int32_t bone = 0; bone < kSkeletonBones; bone++) {
		bool lockThis = false;
		if (locked)
			for (int32_t lockedBone : kLockedBones) lockThis = lockThis || (bone == lockedBone && lock->have[bone] && plausible_bone(lock->known[bone]));
		if (bone == BONE_HEAD) delta[bone] = headDelta;
		else if (lockThis) delta[bone] = m4_mul(upright, m4_rigid_inverse(m4_from_bone(lock->known[bone])));
		else if (bone == BONE_WAIST) delta[bone] = waistDelta;
		else delta[bone] = kSkeleton[bone].parent < 0 ? base : delta[kSkeleton[bone].parent];
	}
}

inline V3 normalized(V3 v) {
	float size = length(v);
	return size > 1e-6f ? v / size : V3{0, 0, 0};
}
inline V3 m4_column(const M4& m, int c) { return {m.at(0, c), m.at(1, c), m.at(2, c)}; }

inline M4 m4_basis(V3 forward, V3 across) {
	V3 f = normalized(forward), a = normalized(across - f * dot(across, f)), c = cross(f, a);
	M4 r = m4_identity();
	const V3 columns[3] = {f, a, c};
	for (int col = 0; col < 3; col++) {
		r.at(0, col) = columns[col].x;
		r.at(1, col) = columns[col].y;
		r.at(2, col) = columns[col].z;
	}
	return r;
}
inline M4 m4_rotation_transposed(const M4& m) {
	M4 r = m4_identity();
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) r.at(i, j) = m.at(j, i);
	return r;
}

inline M4 m4_turn(V3 axis, float radians, V3 pivot) {
	float s, c;
	fsincos(radians / 2, &s, &c);
	V3 a = normalized(axis);
	return m4_turn_about(m4_from_pose({a.x * s, a.y * s, a.z * s, c}, {0, 0, 0}), pivot, pivot);
}

inline float twist_about(const M4& rotation, V3 axis) {
	Q q = q_from_m4(rotation);
	float along = dot(V3{q.x, q.y, q.z}, normalized(axis)), w = q.w;
	if (w < 0) {  // q and -q are the same turn
		along = -along;
		w = -w;
	}
	if (w < 1e-6f) return along > 0 ? kPi : -kPi;
	return 2 * fatan(along / w);
}

// Bone indices for an arm and its fingers, in pure::FINGER order.
struct ArmBones {
	int32_t upper, fore, hand;
	int32_t fingers[FINGER_COUNT];
	int32_t foreTwist, wristTwist, foreAttach;
	float outward;  // her side: +1 left (+x: she faces +z with +y up), -1 right (-x)
};
inline constexpr ArmBones kLeftArm = {BONE_LEFT_UPPER_ARM, BONE_LEFT_FOREARM, BONE_LEFT_HAND, {81, 77, 73, 69, 65}, 85, 84, 86, 1};
inline constexpr ArmBones kRightArm = {BONE_RIGHT_UPPER_ARM, BONE_RIGHT_FOREARM, BONE_RIGHT_HAND, {38, 41, 45, 49, 53}, 58, 57, 59, -1};
inline constexpr int32_t finger_joints(int32_t finger) { return finger == FINGER_THUMB ? 3 : 4; }

// Rotates the grip's -Z axis to match the hand's pose.
inline constexpr float kGripTilt = 0.6f - 1.5707963f;
inline constexpr float kPalmToWrist = 0.08f;   // Wrist offset behind the grip point, metres.
inline constexpr bool kTwistForearm = true;  // Distribute controller roll along the forearm.
// Distributes controller roll across the elbow, forearm, and wrist helper to hide skin twisting.
inline constexpr float kElbowSwing = 0.25f, kMaxElbowSwing = 0.5f;  // Elbow swing fraction and limit, radians.
inline constexpr float kElbowEndTwist = 0.5f, kMidForearmTwist = 0.75f;  // Remaining twist fractions.
inline constexpr float kStraightArm = 0.995f;  // Maximum reach as a fraction of arm length.

// Builds wrist and hand targets from the controller grip pose.
struct HandTarget {
	V3 wrist;
	V3 forward;  // wrist to middle knuckle
	V3 across;   // index knuckle to little knuckle
};
inline HandTarget hand_target(const M4& grip) {
	float s, c;
	fsincos(kGripTilt, &s, &c);
	V3 y = m4_column(grip, 1), z = m4_column(grip, 2);
	V3 forward = y * s - z * c, across = y * -c - z * s;  // -z and -y turned up about x
	return {m4_column(grip, 3) - forward * kPalmToWrist, forward, across};
}

// Solves two-bone arm IK so the wrist reaches target, with the elbow offset by swing.
struct ArmSolve {
	V3 elbow, wrist;
	M4 upperDelta, foreDelta;
};
inline ArmSolve solve_arm(const ArmBones& arm, const float upper[12], const float fore[12], const float hand[12], const float chest[12],
						  V3 target, float swing, const M4 delta[kSkeletonBones]) {
	V3 shoulderBind = kSkeleton[arm.upper].bind, elbowBind = kSkeleton[arm.fore].bind, wristBind = kSkeleton[arm.hand].bind;
	float upperLength = length(elbowBind - shoulderBind), foreLength = length(wristBind - elbowBind);
	V3 shoulder = moved_point(delta[arm.upper], bone_point(upper, shoulderBind));
	V3 elbowNow = moved_point(delta[arm.upper], bone_point(fore, elbowBind));

	V3 reach = target - shoulder;
	float distance = length(reach), longest = (upperLength + foreLength) * kStraightArm, shortest = fabsf(upperLength - foreLength) + 0.01f;
	distance = distance > longest ? longest : (distance < shortest ? shortest : distance);
	V3 along = normalized(reach);
	ArmSolve out;
	out.wrist = shoulder + along * distance;
	M4 chestNow = m4_mul(delta[BONE_CHEST], m4_from_bone(chest));
	V3 pole = m4_column(chestNow, 0) * (0.4f * arm.outward) - m4_column(chestNow, 1) - m4_column(chestNow, 2) * 0.3f;
	V3 bendTowards = normalized(pole - along * dot(pole, along));
	float toElbow = (distance * distance + upperLength * upperLength - foreLength * foreLength) / (2 * distance);
	float side = upperLength * upperLength - toElbow * toElbow;
	out.elbow = moved_point(m4_turn(along, swing, shoulder), shoulder + along * toElbow + bendTowards * (side > 0 ? fsqrt(side) : 0));

	out.upperDelta = m4_mul(m4_turn_about(m4_from_pose(q_towards(elbowNow - shoulder, out.elbow - shoulder, 3.2f), {0, 0, 0}), shoulder, shoulder),
							delta[arm.upper]);
	V3 wristNow = moved_point(out.upperDelta, bone_point(hand, wristBind));
	out.foreDelta =
		m4_mul(m4_turn_about(m4_from_pose(q_towards(wristNow - out.elbow, out.wrist - out.elbow, 3.2f), {0, 0, 0}), out.elbow, out.elbow), out.upperDelta);
	return out;
}

// Returns the hand's twist around the forearm axis.
inline float hand_twist(const ArmSolve& arm, const float fore[12], const M4& handPose) {
	M4 foreList = m4_mul(arm.foreDelta, m4_from_bone(fore));
	return twist_about(m4_mul(handPose, m4_rotation_transposed(foreList)), arm.wrist - arm.elbow);
}

// Poses an arm to the controller target and distributes controller roll along the arm.
inline void pose_arm(const ArmBones& arm, const float upper[12], const float fore[12], const float hand[12], const float chest[12], const M4& grip,
					 M4 delta[kSkeletonBones]) {
	HandTarget target = hand_target(grip);
	ArmSolve solved = solve_arm(arm, upper, fore, hand, chest, target.wrist, 0, delta);

	// Aligns the hand bind pose with the target at the solved wrist.
	V3 wristBind = kSkeleton[arm.hand].bind;
	V3 bindForward = kSkeleton[arm.fingers[FINGER_MIDDLE]].bind - wristBind,
	   bindAcross = kSkeleton[arm.fingers[FINGER_LITTLE]].bind - kSkeleton[arm.fingers[FINGER_INDEX]].bind;
	M4 handPose = m4_mul(m4_basis(target.forward, target.across), m4_rotation_transposed(m4_basis(bindForward, bindAcross)));
	handPose = m4_turn_about(handPose, wristBind, solved.wrist);  // the bone list's matrix she gets (bone pose x inverse bind)
	M4 handDelta = m4_mul(handPose, m4_rigid_inverse(m4_from_bone(hand)));

	// Distributes controller roll while keeping the hand in place.
	M4 foreTwistDelta = solved.foreDelta, wristTwistDelta = solved.foreDelta;
	if (kTwistForearm) {
		float swing = kElbowSwing * hand_twist(solved, fore, handPose);
		swing = swing > kMaxElbowSwing ? kMaxElbowSwing : (swing < -kMaxElbowSwing ? -kMaxElbowSwing : swing);
		solved = solve_arm(arm, upper, fore, hand, chest, target.wrist, swing, delta);
		float twist = hand_twist(solved, fore, handPose);
		V3 axis = solved.wrist - solved.elbow;
		foreTwistDelta = m4_mul(m4_turn(axis, kMidForearmTwist * twist, solved.elbow), solved.foreDelta);
		wristTwistDelta = m4_mul(m4_turn(axis, twist, solved.elbow), solved.foreDelta);
		solved.foreDelta = m4_mul(m4_turn(axis, kElbowEndTwist * twist, solved.elbow), solved.foreDelta);
	}

	bool moved[kSkeletonBones] = {};
	for (int32_t bone = 0; bone < kSkeletonBones; bone++) {
		int32_t parent = kSkeleton[bone].parent;
		if (bone == arm.upper) delta[bone] = solved.upperDelta;
		else if (bone == arm.fore) delta[bone] = solved.foreDelta;
		else if (bone == arm.hand) delta[bone] = handDelta;
		else if (bone == arm.foreTwist || bone == arm.foreAttach) delta[bone] = foreTwistDelta;
		else if (bone == arm.wristTwist) delta[bone] = wristTwistDelta;
		else if (parent >= 0 && moved[parent]) delta[bone] = delta[parent];
		else continue;
		moved[bone] = true;
	}
}

// Additional bend at each finger joint when fully curled, in radians.
inline constexpr float kFistThumbJoints[3] = {0.35f, 0.6f, 0.8f};
inline constexpr float kFistFingerJoints[3] = {1.1f, 1.2f, 0.7f};

// Returns the palm normal in the bind pose.
inline V3 palm_side(const ArmBones& arm) {
	V3 forward = kSkeleton[arm.fingers[FINGER_MIDDLE]].bind - kSkeleton[arm.hand].bind;
	V3 across = kSkeleton[arm.fingers[FINGER_LITTLE]].bind - kSkeleton[arm.fingers[FINGER_INDEX]].bind;
	return normalized(cross(forward, across)) * -arm.outward;
}

inline void pose_fingers(const ArmBones& arm, const float bones[][12], const float curls[FINGER_COUNT], M4 delta[kSkeletonBones]) {
	M4 hand = m4_mul(delta[arm.hand], m4_from_bone(bones[arm.hand]));
	V3 palm = palm_side(arm);
	V3 across = normalized(kSkeleton[arm.fingers[FINGER_LITTLE]].bind - kSkeleton[arm.fingers[FINGER_INDEX]].bind);
	V3 thumbTowards = normalized(palm + across);
	for (int32_t finger = 0; finger < FINGER_COUNT; finger++) {
		const float* fist = finger == FINGER_THUMB ? kFistThumbJoints : kFistFingerJoints;
		float curl = clamp01(curls[finger]);
		M4 chain = hand;
		for (int32_t joint = 0; joint < finger_joints(finger); joint++) {
			int32_t bone = arm.fingers[finger] + joint;
			if (joint < 3) {
				V3 pivot = kSkeleton[bone].bind;
				V3 along = joint + 1 < finger_joints(finger) ? kSkeleton[bone + 1].bind - pivot : pivot - kSkeleton[bone - 1].bind;
				V3 axis = cross(along, finger == FINGER_THUMB ? thumbTowards : palm);
				chain = m4_mul(chain, m4_turn(axis, curl * fist[joint], pivot));
			}
			delta[bone] = m4_mul(chain, m4_rigid_inverse(m4_from_bone(bones[bone])));
		}
	}
}

}  // namespace pure
