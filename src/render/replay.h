// replay.h - Synchronized: record the left eye's frame, play it back for the right eye at Present (replay.cpp).
#pragma once
#include <atomic>
#include <d3d11.h>
#include "render/d3d_hooks.h"

extern std::atomic<bool> g_replaying;           // playback running: hooks pass calls straight through
extern std::atomic<bool> g_recording;           // the recorder is taking calls
extern ID3D11DeviceContext* g_gameContext;  // the game's immediate context
extern std::atomic<int32_t> g_modCallsInProgress;  // mod-internal calls (not recorded)
inline bool ShouldRecord(ID3D11DeviceContext* c) { return g_recording && c == g_gameContext && !g_modCallsInProgress && !g_replaying; }

enum RecStage {
	RS_VS = 0,
	RS_HS,
	RS_DS,
	RS_GS,
	RS_PS,
	RS_CS,
	RS_COUNT
};

void RecSetCB(int stage, UINT start, UINT count, ID3D11Buffer* const* buffers);
void RecSetSRV(int stage, UINT start, UINT count, ID3D11ShaderResourceView* const* views);
void RecSetShader(int stage, void* shader, ID3D11ClassInstance* const* classInstances, UINT count);
void RecCSSetUAV(UINT start, UINT count, ID3D11UnorderedAccessView* const* views, const UINT* initialCounts);
void RecDraw(const DrawArgs& args);
void RecDispatch(UINT x, UINT y, UINT z, int viewFixKind);
void RecSetRenderTargets(UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
void RecSetViewports(UINT count, const D3D11_VIEWPORT* viewports);
void RecBeforeMap(ID3D11Resource* res, D3D11_MAP type);
void RecMap(ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, const D3D11_MAPPED_SUBRESOURCE* mapped);
void RecUnmap(ID3D11Resource* res, UINT sub);
void RecUpdate(ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT rowPitch, UINT depthPitch);
void RecUnsupported(const char* what);

void ReplayInstall(ID3D11DeviceContext* ctx);
bool ReplayAvailable();
void ReplayFrameStart(ID3D11DeviceContext* c, bool synchronized);
void ReplayRelease();  // before the swapchain is resized: hold nothing of the game's
void ReplayStop(bool fromUi);
void ReplayCaptureFinalPassState(ID3D11DeviceContext* c);
void ReplayApplyFinalPassState(ID3D11DeviceContext* c);
bool ReplaySecondEye(ID3D11DeviceContext* c, bool* inBackbuffer);
void ReplayReport();

// The game's pipeline state around the mod's own drawing on its context
void StateSaveTemp(ID3D11DeviceContext* c);
void StateRestoreTemp(ID3D11DeviceContext* c);
