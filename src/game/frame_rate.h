// frame_rate.h - the game's frame rate above its 60 cap, with the game kept at its real speed (frame_rate.cpp).
#pragma once
#include <stdint.h>

void FrameRateInstall();  // from EnsureInit: checks the game's code and hooks its frame limiter and clock step
// From the VR thread, right after each xrWaitFrame: the headset's refresh period, for GameFrameRate "headset rate"
void FrameRateNoteHeadsetFrame(int64_t periodNs);
