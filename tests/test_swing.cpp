// Motion controls: slashes and stabs from a hand's motion (pure::SwingAnalyser, input/waggle.h).
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include "input/waggle.h"

using namespace pure;

static int fails;
static void check(bool c, const char* what) {
	if (!c) {
		std::printf("FAIL %s\n", what);
		fails++;
	}
}

// A simulated hand at 90 Hz. Each step moves it with a velocity in its own frame (-Z = where the fist
// points) and turns it about an axis of its own frame, like a real wrist.
struct Hand {
	SwingAnalyser analyser;
	SwingProfile profile = swing_profile(1);
	int64_t timeNs = 1000000000;
	V3 position = {0.2f, 1.2f, -0.4f};
	Q rotation = {0, 0, 0, 1};
	V3 head = {0, 1.6f, 0};  // about half a metre from the hand
	bool tracked = true;
	int fires = 0, slashes = 0, stabs = 0, rejected = 0;

	void step(V3 localLinear, V3 localAngular) {
		const float dt = 1.0f / 90;
		V3 linear = rotate(rotation, localLinear), angular = rotate(rotation, localAngular);
		position = position + linear * dt;
		float angle = length(localAngular) * dt;
		if (angle > 0) {
			V3 axis = localAngular / length(localAngular);
			float s = std::sin(angle / 2);
			rotation = q_mul(rotation, Q{axis.x * s, axis.y * s, axis.z * s, std::cos(angle / 2)});
		}
		timeNs += 11111111;
		SwingStep result = analyser.update({timeNs, tracked, position, rotation, linear, angular, head}, profile);
		if (result.fired) {
			fires++;
			slashes += result.kind == SWING_SLASH;
			stabs += result.kind == SWING_STAB;
		}
		if (result.ended && !result.hadFired) rejected++;
	}
	void still(int frames) {
		for (int i = 0; i < frames; i++) step({}, {});
	}
	// a slash: turning about the hand's X axis, up to `speed` rad/s, held for `frames`
	void slash(float speed, int frames, float sign = 1) {
		for (float ramp : {0.3f, 0.6f, 0.9f}) step({0, 1.0f * sign, 0}, {speed * ramp * sign, 0, 0});
		for (int i = 0; i < frames; i++) step({0, 1.5f * sign, 0}, {speed * sign, 0, 0});
		for (float ramp : {0.5f, 0.2f}) step({0, 0.5f * sign, 0}, {speed * ramp * sign, 0, 0});
	}
};

int main() {
	{
		Hand h;
		h.still(90);
		check(h.fires == 0 && h.rejected == 0, "a still hand does nothing");
	}
	{
		Hand h;
		h.still(10);
		h.slash(9, 12);  // ~1 rad = 57 degrees
		h.still(40);
		check(h.fires == 1 && h.slashes == 1, "a clean slash attacks once");
	}
	{
		Hand h;
		h.still(10);
		for (float ramp : {0.3f, 0.7f}) h.step({0, 0, -2.5f * ramp}, {});
		for (int i = 0; i < 8; i++) h.step({0, 0, -2.5f}, {});  // 2.5 m/s along -Z: ~0.2 m
		for (float ramp : {0.5f, 0.1f}) h.step({0, 0, -2.5f * ramp}, {});
		h.still(40);
		check(h.fires == 1 && h.stabs == 1, "a clean stab attacks once");
	}
	{
		Hand h;
		h.profile.stabs = false;
		h.still(10);
		for (float ramp : {0.3f, 0.7f}) h.step({0, 0, -2.5f * ramp}, {});
		for (int i = 0; i < 8; i++) h.step({0, 0, -2.5f}, {});
		for (float ramp : {0.5f, 0.1f}) h.step({0, 0, -2.5f * ramp}, {});
		h.still(40);
		check(h.fires == 0, "with stabs off a stab doesn't attack");
	}
	{
		Hand h;
		h.still(10);
		for (int i = 0; i < 45; i++) h.step({0, 1.0f, 0}, {0.8f, 0, 0});  // half a second lifting the arm slowly
		h.still(20);
		check(h.fires == 0, "a slow lift is not an attack");
	}
	{
		Hand h;
		h.still(10);
		for (int i = 0; i < 20; i++) h.step({0, 0.5f, 0}, {(i % 2 ? -8.0f : 8.0f), 0, 0});  // fast but shaking back and forth
		h.still(20);
		check(h.fires == 0, "a wobbling wrist is not an attack");
		check(h.rejected >= 1, "the wobble is rejected (and logged)");
	}
	{
		Hand h;
		h.still(10);
		h.slash(9, 12);
		h.still(50);  // over half a second later
		h.slash(9, 12, -1);
		h.still(40);
		check(h.fires == 2, "two slashes attack twice");
	}
	{
		Hand h;
		h.tracked = false;
		h.still(10);
		h.slash(9, 12);
		h.still(40);
		check(h.fires == 0, "an untracked hand never attacks");
	}
	if (!fails) std::printf("swing: all passed\n");
	return fails ? 1 : 0;
}
