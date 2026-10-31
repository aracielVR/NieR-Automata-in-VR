// frame_share.h - hand-off of finished game frames from the game's device to the VR thread's (g_share: frame_handoff.cpp).
#pragma once
#include <d3d11.h>
#include <stdint.h>

struct FrameTag {          // what a captured game frame shows
	int32_t eye;           // 0 left, 1 right, -1 mono
	bool patched;          // the camera was rewritten for that eye
	bool hasScene;         // a scene-only (no UI) image was captured too
	float pos[3], rot[4];  // eye pose (LOCAL) it was rendered from
	float tanHalfW, tanHalfH;        // half-extents of the frustum it was rendered with
	float tanCentreX, tanCentreY;    // where that frustum points (AsymmetricEyes; 0 = straight ahead)
	uint32_t pairId;    // Synchronized: left and right image of one game frame share it (0 = single)
	bool noUI;          // drawn without UI (the replayed second eye): don't key the HUD from it
	bool hasWorld;      // worldRot is valid (the scene camera was patched)
	float worldRot[4];  // game-world orientation of LOCAL space for this frame (eye sync)
	int64_t poseBase;   // StereoShared::poseBase of the pose it was rendered with (0 = unknown)
};

constexpr int32_t kRingSize = 8;  // capture ring slots (Synchronized pushes two images per game frame)

struct FrameShare {
	CRITICAL_SECTION lock;
	// request: written by the game thread (under lock)
	int32_t requestGen;  // bumps whenever the request changes
	UINT requestW, requestH;
	UINT requestSceneW, requestSceneH;  // the scene-only images: larger than the frame when upscaling
	bool requestKey;                    // upscaling: also the scene images before upscaling (ringKey)
	// written by the VR thread: the game-image size with the detail the headset recommends, in the
	// headset's shape (0 = not seen yet: HeadsetWidth/HeadsetHeight from the ini). The upscaler's output
	// is the current game image enlarged until it has that detail both ways
	UINT nativeW, nativeH;
	DXGI_FORMAT requestFormat;
	// reply: written by the VR thread (under lock): a ring of shared textures
	HANDLE ring[kRingSize];       // legacy shared handles: full frames
	HANDLE ringScene[kRingSize];  // and scene-only (no UI) frames
	HANDLE ringKey[kRingSize];    // upscaling: the scene-only frames before upscaling (the HUD is cut out
								  // against these, which match the full frames pixel for pixel); else null
	int32_t ringGen;        // bumps whenever a new ring is published
	int32_t forRequestGen;  // the request this ring answers
	// completed frames, published by the game thread (under lock) once the GPU finished the copy
	struct Published {
		int32_t seq;      // 0 = none yet
		int32_t slot;  // ring index
		FrameTag tag;
	} latest[3];   // [0] left eye, [1] right eye, [2] mono
	int32_t lastSeq;  // last sequence number handed out
	// GPU that runs the game
	LUID gameLuid;
	bool gameLuidValid;
};
extern FrameShare g_share;
