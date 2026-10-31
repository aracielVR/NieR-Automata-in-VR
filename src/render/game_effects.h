// game_effects.h - switches off NieR effects that don't suit VR: vignette, shadows, ambient occlusion, low-health filter (game_effects.cpp).
#pragma once
#include <d3d11.h>
#include <stdint.h>
#include "render/shader_table.h"

void GameEffectsBeginFrame();
// Before a draw with pixel shader class `ps` (shadows, ambient occlusion). True = skip the draw.
bool GameEffectsBeforeDraw(ID3D11DeviceContext* c, PixelShaderClass ps, uint64_t psHash);
void GameEffectsAfterDraw(ID3D11DeviceContext* c);  // puts back a shader swapped for the draw
// This draw keeps the game's state but draws no pixels (its pixel shader swapped until GameEffectsAfterDraw)
bool GameEffectsDrawNothing(ID3D11DeviceContext* c);
// An effect-system draw (particles, screen filters) that samples the texture registers in `sampled`:
// true = skip it (the low-health filter while in stereo, LowHealthEffect=0).
bool GameEffectsSkipEffectDraw(ID3D11DeviceContext* c, uint32_t sampled, uint64_t shaderHash);

// Vignette and the light scattering's sun direction: their constant buffers are patched as the game uploads them
void GameEffectsOnMap(ID3D11Resource* r, void* data, UINT size);
void GameEffectsBeforeUnmap(ID3D11Resource* r);
// UpdateSubresource and replay: returns `data`, or `scratch` (kPatchScratchFloats floats) holding a patched copy.
// The camera patch (StereoPatchUpdate) uses the same scratch.
constexpr UINT kPatchScratchFloats = 128;
const void* GameEffectsPatchUpdate(ID3D11Resource* r, const void* data, UINT size, float* scratch);
