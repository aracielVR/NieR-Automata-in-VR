// reproject.h - depth-based correction of the older eye in alternating mode (reproject.cpp).
#pragma once
#include <d3d11.h>
#include "render/frame_share.h"

void ReprojectNoteDsv(ID3D11DepthStencilView* dsv);  // a depth buffer the game draws with
void ReprojectAtUiStart(ID3D11DeviceContext* c);     // the scene is finished: keep its depth
void ReprojectRelease();                             // before a resize
// At Present after this frame's eye was captured: returns the warped other eye (or null)
ID3D11Texture2D* ReprojectOtherEye(ID3D11DeviceContext* c, ID3D11Texture2D* scene, ID3D11Texture2D* bb, FrameTag* tag);
ID3D11Resource* ReprojectSceneDepth();            // the game's scene depth buffer (raw), or null
void ReprojectSetBackbufferSize(UINT w, UINT h);  // at Present: the size the scene depth must have
