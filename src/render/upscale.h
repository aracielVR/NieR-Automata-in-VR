// upscale.h - temporal upscaling of the 3D image for the headset ([Upscaler], upscale.cpp).
#pragma once
#include <d3d11.h>

// At Present, before the frame goes to the headset: decides this frame's scene-image size (*outW,
// *outH: enlarged when upscaling, else w x h). True while upscaling (also DLAA: same size).
bool UpscaleBeginPresent(ID3D11DeviceContext* c, UINT w, UINT h, UINT* outW, UINT* outH);
// The HUD-free scene image of `eye` (-1 = mono) at the output size: upscaled, or stretched when it
// can't be. Returns `scene` itself when not upscaling, null on failure.
ID3D11Texture2D* UpscaleScene(ID3D11DeviceContext* c, int eye, ID3D11Texture2D* scene);
void UpscaleAtUiStart(ID3D11DeviceContext* c);               // the scene is finished: keep this eye's depth
void UpscaleCaptureDepth(ID3D11DeviceContext* c, int eye);   // Synchronized: the replayed right eye's depth
bool UpscaleJitter(int eye, float ndc[2], float pixels[2]);  // camera.cpp: this frame's sub-pixel offset
bool UpscaleWantsDepth();                                    // the scene depth buffer has to be tracked
void UpscaleRelease();                                       // before a resize
void UpscaleStatus(char* out, size_t cap);                   // for the stats overlay / Info page
