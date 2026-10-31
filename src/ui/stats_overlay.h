// stats_overlay.h - the stats overlay, drawn with Dear ImGui on the VR thread ([Screen] StatsOverlay, stats_overlay.cpp).
#pragma once
#include <d3d11.h>
#include <stdint.h>

// Headset frame times, a ring buffer for the overlay's graph.
struct FrameTimes {
	static constexpr int32_t kCount = 180;  // two seconds at 90 Hz
	float ms[kCount];
	int32_t next;  // where the next one goes (the oldest entry)
};

// A hand's highest speed and acceleration over the last half second (< 0 = unknown).
struct OverlayHand {
	float speed, spin;       // m/s, deg/s
	float accel, spinAccel;  // m/s^2, deg/s^2
};

// Everything the overlay shows. The frame rates and frame times come from its own counting; the
// gather callback (xr_frame.cpp) fills in the rest from the VR thread's state.
struct OverlayNumbers {
	float headsetFps;
	float gameFpsLeft, gameFpsRight, gameFpsFlat;  // game images per second
	FrameTimes frameTimes;
	char mode[32];                 // "alternating", "flat screen", ...
	uint32_t gameW, gameH;         // the game's render size
	uint32_t sceneW, sceneH;       // the 3D image's size in the headset (larger when upscaling)
	char upscaler[128];            // "DLSS 310.9.16 Quality" or why not; "" = off
	float sharpX, sharpY;          // % of the headset's recommended detail, across / up-down; 0 = unknown
	float poseDelayMs, predictMs;  // poseDelayMs < 0 = unknown
	OverlayHand leftHand, rightHand;
};

// Each headset frame: counts it and records its frame time, for the graph.
void OverlayCountHeadsetFrame();
// Each game image the VR thread receives: 0 left, 1 right, 2 flat.
void OverlayCountGameImage(int kind);
// Each headset frame while the overlay is on: true when it should be drawn again (ten times a second).
// Twice a second it calls gather for the VR thread's numbers and works out the frame rates.
bool OverlayRedrawDue(bool shown, void (*gather)(OverlayNumbers& numbers));
// Draws the overlay into rtv (w x h pixels). False if ImGui couldn't start.
bool OverlayRender(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h);
