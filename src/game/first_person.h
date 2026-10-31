// First-person tracking from the character's bone uploads.
#pragma once
#include <d3d11.h>
#include "game/head_bone.h"

// From the D3D11 hooks (the game's immediate context only)
void FirstPersonOnSetVSConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT count, ID3D11Buffer* const* buffers);
void FirstPersonOnMap(ID3D11Resource* res, void* data, UINT byteWidth);
void FirstPersonBeforeUnmap(ID3D11Resource* res);
void FirstPersonOnSetRenderTargets(UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
// Returns true when a head-only part should be hidden from the main view.
bool FirstPersonSkipDraw();
void FirstPersonBeforeDraw(ID3D11DeviceContext* c);  // a draw of the character into the main view: their parts near your eyes hidden
void FirstPersonAfterDraw(ID3D11DeviceContext* c);   // ... and the game's bone list bound again

void FirstPersonNewFrame();                             // camera.cpp, after each Present
void FirstPersonSetGameSize(UINT width, UINT height);  // the game's resolution (frame_handoff.cpp, each Present)
bool FirstPersonHead(pure::HeadPose* out);              // the character's head as last drawn (false = not seen lately)
bool FirstPersonSteadyEyeHeight(float* out);            // their eyes' height without running bob
void FirstPersonSetHiding(bool hide);                   // camera.cpp: this frame's eyes are in their head
// Updates the viewer pose and offset from the character's eyes for this game frame.
void FirstPersonSetViewer(const pure::M4& view, const pure::M4& camera, pure::V3 fromCharacterEyes);
// Updates the controller grip poses used by body tracking.
void FirstPersonSetHands(const bool valid[2], const pure::M4 grips[2]);
bool FirstPersonFacing(float* yaw);   // the way the game faces the character: their hips' yaw (pure::yaw_of), as posed by the game
bool FirstPersonFeet(pure::V3* out);  // the ground under the character's hips, as the game posed them (false = not seen lately)
