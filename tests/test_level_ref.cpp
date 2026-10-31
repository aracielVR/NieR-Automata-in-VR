// Level reference (pure::level_reference): upright for the player in a normal and in a flipped space.
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
static pure::Q axisAngle(float x, float y, float z, float deg) {
	float a = deg / 57.29578f / 2, s = std::sin(a);
	return {x * s, y * s, z * s, std::cos(a)};
}
int main() {
	bool flipped = false;
	pure::Q ref = {};
	// normal space, head turned 30 deg left and pitched down 10: yaw only
	pure::Q head = pure::q_mul(axisAngle(0, 1, 0, 30), axisAngle(1, 0, 0, -10));
	check(pure::level_reference(head, &flipped, &ref) && !flipped, "normal: found, not flipped");
	pure::V3 up = pure::rotate(ref, {0, 1, 0}), fw = pure::rotate(ref, {0, 0, -1}), hf = pure::rotate(head, {0, 0, -1});
	check(up.y > 0.999f, "normal: reference up is +Y");
	check(std::fabs(fw.y) < 1e-5f && fw.x * hf.x + fw.z * hf.z > 0.98f, "normal: forward is the head's heading");
	// flipped space (rolled 180 about forward), same real head: the head's up is -Y
	pure::Q flip = axisAngle(0, 0, 1, 180);
	pure::Q headF = pure::q_mul(flip, head);  // the same head expressed in the flipped space
	check(pure::level_reference(headF, &flipped, &ref) && flipped, "flipped: detected");
	up = pure::rotate(ref, {0, 1, 0});
	check(up.y < -0.999f, "flipped: reference up is the space's -Y (the real up)");
	// the head relative to the reference has no roll: its up stays close to the reference's up
	pure::Q rel = pure::q_mul(pure::Q{-ref.x, -ref.y, -ref.z, ref.w}, headF);
	pure::V3 relUp = pure::rotate(rel, {0, 1, 0});
	check(relUp.y > 0.98f, "flipped: head upright relative to the reference (no 180 deg roll)");
	// tilting the head sideways 70 deg keeps the decision
	pure::Q tilted = pure::q_mul(headF, axisAngle(0, 0, 1, 70));
	check(pure::level_reference(tilted, &flipped, &ref) && flipped, "flipped: kept while tilted");
	if (!fails) std::printf("level reference tests passed\n");
	return fails ? 1 : 0;
}
