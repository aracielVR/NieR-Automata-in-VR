// foveated.h - foveated shading ([Render] FoveatedShading): the edges of each eye's 3D image shaded at a lower rate, with NVIDIA's variable rate shading (foveated.cpp).
#pragma once
#include <d3d11.h>

void FoveatedFrameStart();                          // each game frame: its scene starts
void FoveatedSetGameSize(UINT width, UINT height);  // the game's resolution (frame_handoff.cpp, each Present)
// The game's (or the replay's) render targets changed: on for the scene's full-size passes in stereo, off for the rest
void FoveatedOnTargets(ID3D11DeviceContext* c, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
void FoveatedSceneEnd(ID3D11DeviceContext* c);   // the UI starts, or the replay ends: off until the next scene
void FoveatedSceneAgain();                        // Synchronized: the replay draws the scene again
