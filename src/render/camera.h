// camera.h - stereo on the game side: the game camera rewritten for each eye (camera.cpp).
#pragma once
#include <d3d11.h>
#include "core/pure.h"
#include "render/frame_share.h"

struct StereoShared {  // written by the VR thread, read by the game thread
	SRWLOCK lock;
	bool enabled;                      // stereo toggled on (hotkey)
	bool valid;                        // eye poses below are current
	float eyePos[2][3], eyeRot[2][4];  // LOCAL space
	float eyeTan[2][4];                // tan of angleLeft, angleRight, angleUp, angleDown
	bool handValid[2];                 // the controllers' grip poses (left, right), LOCAL space, same time as the eyes
	float handPos[2][3], handRot[2][4];
	bool menuMode;                     // the UI covers the screen: render flat (mono)
	bool refValid;                     // recentred reference (yaw-only head pose)
	float refPos[3], refRot[4];
	int64_t poseBase;  // VR frame display time (XrTime) the poses were predicted from;
						 // they are predicted for poseBase + the measured lag
};
extern StereoShared g_stereo;

// NieR's camera is never narrower than 5:4: at a taller backbuffer (the headset's shape) its projection
// keeps aspect 1.25, and its final pass letterboxes the image into a 5:4 box (hud_layer.cpp).
constexpr float kGameMinAspect = 1.25f;

void StereoSetAspect(float aspect);
void StereoBeginFrame();
void StereoGetFrameTag(FrameTag* tag);
bool StereoActive();
bool StereoWatching();
void StereoCountDraw();
int StereoFrameEye();  // the eye this frame renders (-1 = mono)
bool StereoGameCamera(float pos[3], float fwd[3], float up[3]);
bool StereoHeadRelative(pure::M4* rel);              // eye pose relative to the game camera (head culling)
// The middle of the head (no eye turn) relative to the game camera, turned on by leadMs of its current turn (sun shadows)
bool StereoHeadCentreRelative(pure::M4* rel, float leadMs);
float StereoHeadCullFov(float marginDeg);            // radians; 0 = not known yet
bool StereoEyeTangents(float* halfW, float* halfH, float centre[2]);  // this frame's eye image: tangents of its half-angles and where its lens points (false: mono)
bool StereoGetViewFix(float T[16], float Tinv[16]);  // game view -> eye view (and back), this frame
// The same for data the game uploads BEFORE this eye's camera (light scattering): this frame's once the camera is
// patched, else this frame's eye pose on the last game camera (only the levelling depends on the camera).
bool StereoGetEyeViewFix(float T[16]);
void StereoOnMap(ID3D11Resource* res, void* data, UINT size);
void StereoBeforeUnmap(ID3D11Resource* res);
const void* StereoPatchUpdate(UINT size, const void* data, float* scratch);
bool StereoSynchronizedFrame();  // this frame renders the left eye and replays the right one
void StereoBeginSecondEye();     // switch the camera patching to the right eye (same head pose)
bool StereoSecondEyePatched();
bool StereoReprojectParams(int eye, float T[16], float proj[4], float centre[2], FrameTag* tag);  // depth reprojection
// AFW: this frame's eye moved into the other eye's view (source/destination projection and lens centre each)
bool StereoAfwParams(int eye, float T[16], float projSrc[4], float lensSrc[2], float projDst[4], float lensDst[2], FrameTag* tag);
bool StereoUpscaleCams(int eye, float T[16], float cur[4], float prev[4], float centres[4], float jitter[2], bool* hasPrev);
