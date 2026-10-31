// extra_resolutions.h - adds headset-shaped resolutions to the game's list (extra_resolutions.cpp).
#pragma once
#include <d3d11.h>

void ExtraResolutionsInstallHooks();
void ExtraResolutionsEarlyHooks();  // from DllMain: patches code only
void ExtraResolutionsHookSwapChain(IDXGISwapChain* swapChain);
void ExtraResolutionsOnFirstPresent(IDXGISwapChain* swapChain);
// Remembers the size the game renders at, so its own resolution list always offers it again.
void RememberGameSize(UINT width, UINT height);
