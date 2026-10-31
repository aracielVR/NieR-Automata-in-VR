// game_resolution.h - the render resolution chosen in the VR panel, applied through the game's own settings.
#pragma once
#include <stdint.h>

// [Render] PanelResolution: makes the game's saved resolution (SystemData.dat, in My Games under
// Documents) the size the panel's Resolution and DLSS quality ask for. The game reads it when it
// starts, so it renders at that size with all of its own buffers made to match.
void GameResolutionInit();  // once, at start-up: brings the saved resolution in line with the panel
void GameResolutionTick();  // every frame: writes it again when the panel's settings change
bool GameResolutionPanelSize(uint32_t* width, uint32_t* height);  // the size the panel asks for (false: none)
// For the panel: what the game draws and what the headset gets with the current Resolution and DLSS quality.
bool GameResolutionSizes(uint32_t* drawW, uint32_t* drawH, uint32_t* headsetW, uint32_t* headsetH);
