// frame_handoff.h - hooks IDXGISwapChain::Present and copies each finished game frame into the
// textures shared with the VR thread (frame_handoff.cpp).
#pragma once
#include <d3d11.h>

extern ID3D11DeviceContext* g_replayCtx;

void HookFromSwapChain(IDXGISwapChain* swapChain);
void HookWithDummySwapChain();
void CapturePollMidFrame();  // publish finished frames without waiting for Present
