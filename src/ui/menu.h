// menu.h - the in-headset settings menu (menu.cpp).
#pragma once
#include <d3d11.h>

void MenuTick(bool foreground);
bool MenuRender(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, int width, int height);
// Lines of the Info page, each set by the part of the mod it is about
enum InfoLine {
	INFO_MODE,                             // xr_frame.cpp
	INFO_HEADSET,                          // xr_frame.cpp
	INFO_CULLING,                          // culling.cpp
	INFO_SYNCHRONIZED,                     // replay.cpp
	INFO_GPU_TOTAL,                        // gpu_timing.cpp
	INFO_GPU_PASSES,                       // gpu_timing.cpp: the five slowest passes, one line each
	INFO_REPROJECT = INFO_GPU_PASSES + 5,  // reproject.cpp
	INFO_CONTROLLERS,                      // vr_controllers.cpp
	INFO_VIEWFIX,                          // viewfix.cpp
	INFO_HEAD_POSE,                        // xr_frame.cpp
	INFO_RESOLUTION,                       // xr_frame.cpp
	INFO_MOTION_CONTROLS,                  // vr_controllers.cpp
	INFO_PANEL_RESOLUTION,                 // game_resolution.cpp
	INFO_UPSCALER,                         // upscale.cpp
	INFO_UPSCALE_ADVICE,                   // upscale.cpp
	INFO_LINE_COUNT
};
void MenuSetInfo(int line, const char* text);  // a line of the Info page (any thread)
bool MenuIsOpen();
bool MenuTakeJustOpened();
bool MenuTakeRecenter();
