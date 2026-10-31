// VR controllers as a gamepad: the left menu button (tap = Start, hold = Back, + stick = D-pad).
#include <cstdio>
#include "core/pure.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
using MB = pure::MenuButton;
int main() {
	// XInput bit values
	check(MB::UP == 0x1 && MB::DOWN == 0x2 && MB::LEFT == 0x4 && MB::RIGHT == 0x8 && MB::START == 0x10 && MB::BACK == 0x20,
		  "bits match XInput");
	{  // tap: Start after release, for 120 ms
		MB m;
		float x = 0, y = 0;
		check(m.update(true, &x, &y, 1000) == 0, "tap: nothing while held");
		check(m.update(true, &x, &y, 1200) == 0, "tap: nothing at 200 ms");
		check(m.update(false, &x, &y, 1250) == MB::START, "tap: Start on release");
		check(m.update(false, &x, &y, 1300) == MB::START, "tap: Start still");
		check(m.update(false, &x, &y, 1400) == 0, "tap: Start over");
	}
	{  // hold: Back after 0.5 s, no Start on release
		MB m;
		float x = 0, y = 0;
		m.update(true, &x, &y, 5000);
		check(m.update(true, &x, &y, 5400) == 0, "hold: not yet");
		check(m.update(true, &x, &y, 5600) == MB::BACK, "hold: Back");
		check(m.update(false, &x, &y, 6000) == 0, "hold: no Start");
	}
	{  // D-pad: stick consumed, no Start, no Back
		MB m;
		float x = 0, y = 0.9f;
		m.update(true, &x, &y, 100);
		check(x == 0 && y == 0, "dpad: stick neutral");
		x = 0.9f, y = 0;
		check(m.update(true, &x, &y, 200) == MB::RIGHT, "dpad: right");
		x = 0, y = 0;
		check(m.update(true, &x, &y, 900) == 0, "dpad: no Back after use");
		check(m.update(false, &x, &y, 950) == 0, "dpad: no Start after use");
		x = -0.9f, y = -0.9f;
		m.update(true, &x, &y, 2000);
		x = -0.9f, y = -0.9f;
		check(m.update(true, &x, &y, 2010) == (MB::LEFT | MB::DOWN), "dpad: diagonal");
	}
	{  // released: stick passes through
		MB m;
		float x = 0.5f, y = -0.3f;
		check(m.update(false, &x, &y, 10) == 0 && x == 0.5f && y == -0.3f, "released: stick untouched");
	}
	{  // tick counter wrap
		MB m;
		float x = 0, y = 0;
		m.update(true, &x, &y, 0xFFFFFF00u);
		check(m.update(false, &x, &y, 0x40u) == MB::START, "wrap: tap");
	}
	if (!fails) std::printf("test_vr_controllers: all passed\n");
	return fails ? 1 : 0;
}
