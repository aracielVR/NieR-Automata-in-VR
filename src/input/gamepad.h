// gamepad.h - the XInput hooks and the menu's gamepad reading (gamepad.cpp).
#pragma once
#include <atomic>
#include <windows.h>
#include <xinput.h>

extern std::atomic<bool> g_blockPad;  // the menu is open: the game sees a neutral pad

void InstallGamepad();
bool ReadGamepad(XINPUT_GAMEPAD* out);
DWORD GamepadAttackTick();  // when an attack button (X light, Y heavy) last went down on the pad the game reads (0: never)
DWORD GamepadMoveTick();    // when the left stick was last out of its dead zone on the pad the game reads (0: never)
DWORD GamepadJumpTick();    // when the jump button (A) last went down on the pad the game reads (0: never)
