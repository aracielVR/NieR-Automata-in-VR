// hud_layer.h - separates NieR's 2D UI (HUD, menus) from the 3D scene on the game side, so the HUD can
// go on its own flat layer (hud_layer.cpp).
#pragma once
#include <d3d11.h>
#include "render/d3d_hooks.h"

void UiSetBackbuffer(ID3D11Texture2D* backbuffer);
void UiBeginFrame();
void UiOnSetVS(bool isUI);
bool UiBeforeDraw(ID3D11DeviceContext* c);  // true = skip this draw
void UiAfterDraw(ID3D11DeviceContext* c, const DrawArgs& args);
void UiOnMap(ID3D11Resource* res, void* data, UINT size);
void UiBeforeUnmap(ID3D11Resource* res);
void UiOnUpdate(ID3D11Resource* res, const void* data, UINT size);
void UiOnSetVSConstantBuffers(UINT start, UINT count, ID3D11Buffer* const* buffers);
void UiOnResizeBuffers();                       // the swapchain's buffers are about to be replaced
ID3D11Texture2D* UiSceneTexture();              // this frame's scene without UI, or null
bool UiSecondEyeScene(ID3D11DeviceContext* c);  // right eye's scene image from the replayed UI target
