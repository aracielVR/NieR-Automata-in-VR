// d3d_hooks.cpp - hooks on the game's D3D11 device and immediate context, passed on to every feature that needs them.
#include "render/d3d_hooks.h"
#include "core/com.h"
#include <atomic>
#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include "debug/frame_capture.h"
#include "debug/gpu_timing.h"
#include "render/camera.h"
#include "game/first_person.h"
#include "render/foveated.h"
#include "render/frame_handoff.h"
#include "render/game_effects.h"
#include "ui/hud_layer.h"
#include "render/replay.h"
#include "render/reproject.h"
#include "render/shader_table.h"
#include "render/texture_tags.h"
#include "render/viewfix.h"

namespace {
// vtable slots (verified against the d3d11.h layout at build time, see tools/vtable_check.c)
enum {
	CTX_VSSetConstantBuffers = 7,
	CTX_PSSetShaderResources = 8,
	CTX_PSSetShader = 9,
	CTX_VSSetShader = 11,
	CTX_DrawIndexed = 12,
	CTX_Draw = 13,
	CTX_Map = 14,
	CTX_Unmap = 15,
	CTX_PSSetConstantBuffers = 16,
	CTX_DrawIndexedInstanced = 20,
	CTX_DrawInstanced = 21,
	CTX_GSSetConstantBuffers = 22,
	CTX_OMSetRenderTargets = 33,
	CTX_Dispatch = 41,
	CTX_RSSetViewports = 44,
	CTX_UpdateSubresource = 48,
	CTX_ExecuteCommandList = 58,
	CTX_CSSetShaderResources = 67,
	CTX_CSSetUnorderedAccessViews = 68,
	CTX_CSSetShader = 69,
	CTX_CSSetConstantBuffers = 71,
	DEV_CreateTexture2D = 5,
	DEV_CreateVertexShader = 12,
	DEV_CreatePixelShader = 15,
	DEV_CreateComputeShader = 18,
	DEV_CreateDeferredContext = 27,
};

ID3D11Device* s_device;
ID3D11DeviceContext* s_context;
std::atomic<int32_t> s_deferredContextCount;

typedef void(STDMETHODCALLTYPE* PFN_SetConstantBuffers)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
typedef void(STDMETHODCALLTYPE* PFN_SetShader)(ID3D11DeviceContext*, void*, ID3D11ClassInstance* const*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Map)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
											D3D11_MAPPED_SUBRESOURCE*);
typedef void(STDMETHODCALLTYPE* PFN_Unmap)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
														ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* PFN_UpdateSubresource)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
													   const void*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_RSSetViewports)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandList)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D11DeviceContext*, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_SetShaderResources)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
typedef void(STDMETHODCALLTYPE* PFN_SetUnorderedAccessViews)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*,
															 const UINT*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateShader)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDeferredContext)(ID3D11Device*, UINT, ID3D11DeviceContext**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateTexture2D)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*,
														ID3D11Texture2D**);

PFN_SetConstantBuffers o_SetConstantBuffers[4];
PFN_SetShader o_VSSetShader, o_PSSetShader, o_CSSetShader;
PFN_DrawIndexed o_DrawIndexed;
PFN_Draw o_Draw;
PFN_DrawIndexedInstanced o_DrawIndexedInstanced;
PFN_DrawInstanced o_DrawInstanced;
PFN_Map o_Map;
PFN_Unmap o_Unmap;
PFN_OMSetRenderTargets o_OMSetRenderTargets;
PFN_UpdateSubresource o_UpdateSubresource;
PFN_RSSetViewports o_RSSetViewports;
PFN_ExecuteCommandList o_ExecuteCommandList;
PFN_Dispatch o_Dispatch;
PFN_SetShaderResources o_PSSetShaderResources, o_CSSetShaderResources;
PFN_SetUnorderedAccessViews o_CSSetUnorderedAccessViews;
PFN_CreateShader o_CreateVertexShader, o_CreatePixelShader, o_CreateComputeShader;
PFN_CreateDeferredContext o_CreateDeferredContext;
PFN_CreateTexture2D o_CreateTexture2D;

bool IsConstantBuffer(ID3D11Resource* res, UINT* byteWidth) {
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
	D3D11_BUFFER_DESC desc;
	((ID3D11Buffer*)res)->GetDesc(&desc);
	*byteWidth = desc.ByteWidth;
	return (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

// A buffer shaders read as a resource (the effect system's per-object structured buffers): recorded by the capture only.
bool IsShaderBuffer(ID3D11Resource* res, UINT* byteWidth) {
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
	D3D11_BUFFER_DESC desc;
	((ID3D11Buffer*)res)->GetDesc(&desc);
	*byteWidth = desc.ByteWidth;
	return (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
}

// The game's current pixel and compute shaders
PixelShaderClass s_psClass;
uint32_t s_psSampledSlots;  // texture registers it samples
uint64_t s_psHash;          // bytecode hash (stable between runs, for the log)
ComputeShaderClass s_csClass;

constexpr RecStage kRecStageOf[4] = {RS_VS, RS_PS, RS_GS, RS_CS};  // by ShaderStage
template <int Stage>
void STDMETHODCALLTYPE H_SetConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT count, ID3D11Buffer* const* buffers) {
	if (g_replaying) return o_SetConstantBuffers[Stage](c, start, count, buffers);
	if (Stage == ST_VS && c == s_context && buffers) {
		UiOnSetVSConstantBuffers(start, count, buffers);
		FirstPersonOnSetVSConstantBuffers(c, start, count, buffers);
	}
	if (ShouldRecord(c)) RecSetCB(kRecStageOf[Stage], start, count, buffers);
	if (CaptureRecording(c)) CaptureSetConstantBuffers((ShaderStage)Stage, start, count, buffers);
	o_SetConstantBuffers[Stage](c, start, count, buffers);
}

void STDMETHODCALLTYPE H_VSSetShader(ID3D11DeviceContext* c, void* shader, ID3D11ClassInstance* const* classInstances, UINT count) {
	if (g_replaying) return o_VSSetShader(c, shader, classInstances, count);
	if (ShouldRecord(c)) RecSetShader(RS_VS, shader, classInstances, count);
	if (c == s_context) UiOnSetVS(ShaderTableIsUi(shader));
	if (CaptureRecording(c)) CaptureSetShader(ST_VS, shader);
	o_VSSetShader(c, shader, classInstances, count);
}

void STDMETHODCALLTYPE H_PSSetShader(ID3D11DeviceContext* c, void* shader, ID3D11ClassInstance* const* classInstances, UINT count) {
	if (g_replaying) return o_PSSetShader(c, shader, classInstances, count);
	if (ShouldRecord(c)) RecSetShader(RS_PS, shader, classInstances, count);
	if (c == s_context) {
		PixelShaderClass previous = s_psClass;
		s_psClass = ShaderTableClassifyPixel(shader, &s_psSampledSlots, &s_psHash);
		// GPU timing: the expensive full-screen passes get their own marks
		// One entry per class: a table shorter than the enum read past its end when PS_GLARE was added,
		// handing a garbage label to the timing code, which crashed in ucrtbase (2026-10-27).
		static const char* const kNames[] = {nullptr, "AO (SAO)", "shadow mask", nullptr, nullptr, "light scattering", nullptr};
		static_assert(sizeof(kNames) / sizeof(kNames[0]) == PS_CLASS_COUNT, "a name (or nullptr) for every pixel shader class");
		if (s_psClass != previous) {
			const char* name = kNames[s_psClass];
			if (name) GpuTimingMark(c, name);
			else if (kNames[previous]) GpuTimingMark(c, "(after a full-screen pass)");
		}
	}
	if (CaptureRecording(c)) CaptureSetShader(ST_PS, shader);
	o_PSSetShader(c, shader, classInstances, count);
}

// Shared by the four draw hooks: true = the game's draw must not run.
bool BeforeDraw(ID3D11DeviceContext* c) {
	CapturePollMidFrame();
	StereoCountDraw();
	if (FirstPersonSkipDraw()) return true;
	// the game's own shader: a replacement swapped in below is classified as another kind
	PixelShaderClass ps = s_psClass;
	uint32_t sampled = s_psSampledSlots;
	uint64_t hash = s_psHash;
	if (GameEffectsBeforeDraw(c, ps, hash)) return true;
	// skipped after all: the pixel shader GameEffectsBeforeDraw may have swapped in goes back now
	if (UiBeforeDraw(c) || (ps == PS_EFFECT && GameEffectsSkipEffectDraw(c, sampled, hash))) {
		GameEffectsAfterDraw(c);
		return true;
	}
	FirstPersonBeforeDraw(c);
	GpuTimingDraw();
	return false;
}
void AfterDraw(ID3D11DeviceContext* c, const DrawArgs& args) {
	FirstPersonAfterDraw(c);
	GameEffectsAfterDraw(c);
	UiAfterDraw(c, args);
}

void STDMETHODCALLTYPE H_Draw(ID3D11DeviceContext* c, UINT vertexCount, UINT startVertex) {
	if (g_replaying) return o_Draw(c, vertexCount, startVertex);
	bool isGame = c == s_context;
	if (isGame && BeforeDraw(c)) return;
	DrawArgs args = {DRAW, vertexCount, startVertex, 0, 0, 0};
	if (ShouldRecord(c)) RecDraw(args);
	if (CaptureRecording(c)) CaptureDraw(DRAW, vertexCount, startVertex, 0);
	o_Draw(c, vertexCount, startVertex);
	if (isGame) AfterDraw(c, args);
}
void STDMETHODCALLTYPE H_DrawIndexed(ID3D11DeviceContext* c, UINT indexCount, UINT startIndex, INT baseVertex) {
	if (g_replaying) return o_DrawIndexed(c, indexCount, startIndex, baseVertex);
	bool isGame = c == s_context;
	if (isGame && BeforeDraw(c)) return;
	DrawArgs args = {DRAW_INDEXED, indexCount, startIndex, baseVertex, 0, 0};
	if (ShouldRecord(c)) RecDraw(args);
	if (CaptureRecording(c)) CaptureDraw(DRAW_INDEXED, indexCount, startIndex, (UINT)baseVertex);
	o_DrawIndexed(c, indexCount, startIndex, baseVertex);
	if (isGame) AfterDraw(c, args);
}
void STDMETHODCALLTYPE H_DrawInstanced(ID3D11DeviceContext* c, UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex, UINT startInstance) {
	if (g_replaying) return o_DrawInstanced(c, vertexCountPerInstance, instanceCount, startVertex, startInstance);
	bool isGame = c == s_context;
	if (isGame && BeforeDraw(c)) return;
	DrawArgs args = {DRAW_INSTANCED, vertexCountPerInstance, instanceCount, (INT)startVertex, startInstance, 0};
	if (ShouldRecord(c)) RecDraw(args);
	if (CaptureRecording(c)) CaptureDraw(DRAW_INSTANCED, vertexCountPerInstance, instanceCount, startVertex);
	o_DrawInstanced(c, vertexCountPerInstance, instanceCount, startVertex, startInstance);
	if (isGame) AfterDraw(c, args);
}
void STDMETHODCALLTYPE H_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT indexCountPerInstance, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance) {
	if (g_replaying) return o_DrawIndexedInstanced(c, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
	bool isGame = c == s_context;
	if (isGame && BeforeDraw(c)) return;
	DrawArgs args = {DRAW_INDEXED_INSTANCED, indexCountPerInstance, instanceCount, (INT)startIndex, (UINT)baseVertex, startInstance};
	if (ShouldRecord(c)) RecDraw(args);
	if (CaptureRecording(c)) CaptureDraw(DRAW_INDEXED_INSTANCED, indexCountPerInstance, instanceCount, startIndex);
	o_DrawIndexedInstanced(c, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
	if (isGame) AfterDraw(c, args);
}

HRESULT STDMETHODCALLTYPE H_Map(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags,
								D3D11_MAPPED_SUBRESOURCE* mapped) {
	if (!g_replaying && res && ShouldRecord(c)) RecBeforeMap(res, type);
	HRESULT hr = o_Map(c, res, sub, type, flags, mapped);
	if (g_replaying) return hr;
	if (SUCCEEDED(hr) && ShouldRecord(c)) RecMap(res, sub, type, flags, mapped);
	UINT byteWidth;
	if (c != s_context || FAILED(hr) || !mapped || !res || type == D3D11_MAP_READ) return hr;
	if (!IsConstantBuffer(res, &byteWidth)) {
		if (CaptureRecording(c) && IsShaderBuffer(res, &byteWidth)) CaptureMap(res, mapped->pData, byteWidth);
		return hr;
	}
	if (StereoWatching()) StereoOnMap(res, mapped->pData, byteWidth);
	FirstPersonOnMap(res, mapped->pData, byteWidth);
	GameEffectsOnMap(res, mapped->pData, byteWidth);
	UiOnMap(res, mapped->pData, byteWidth);
	if (CaptureRecording(c)) CaptureMap(res, mapped->pData, byteWidth);
	return hr;
}

void STDMETHODCALLTYPE H_Unmap(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub) {
	if (g_replaying) return o_Unmap(c, res, sub);
	if (c == s_context && res) FirstPersonBeforeUnmap(res);  // the character's posed bones: recorded so for both eyes
	if (ShouldRecord(c) && res) RecUnmap(res, sub);          // the game's own data, before the camera is patched in
	if (c == s_context && res) {
		StereoBeforeUnmap(res);  // patch the camera for this frame's eye
		UiBeforeUnmap(res);      // remember UI draw parameters
		GameEffectsBeforeUnmap(res);
	}
	if (CaptureRecording(c) && res) CaptureUnmap(res);
	o_Unmap(c, res, sub);
}

void STDMETHODCALLTYPE H_UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, const D3D11_BOX* box,
										   const void* data, UINT rowPitch, UINT depthPitch) {
	if (g_replaying) return o_UpdateSubresource(c, res, sub, box, data, rowPitch, depthPitch);
	if (ShouldRecord(c)) RecUpdate(res, sub, box, data, rowPitch, depthPitch);
	UINT byteWidth;
	float patched[kPatchScratchFloats];
	if (c == s_context && res && data && !box && IsConstantBuffer(res, &byteWidth)) {
		// Constant buffers can only be updated whole, so `data` holds byteWidth bytes.
		if (StereoWatching()) data = StereoPatchUpdate(byteWidth, data, patched);
		data = GameEffectsPatchUpdate(res, data, byteWidth, patched);
		UiOnUpdate(res, data, byteWidth);
		if (CaptureRecording(c)) CaptureUpdate(res, data, byteWidth);
	} else if (c == s_context && res && data && CaptureRecording(c) && IsShaderBuffer(res, &byteWidth)) {
		UINT offset = box ? box->left : 0;
		CaptureBufferUpdate(res, offset, data, box ? box->right - box->left : byteWidth - offset);
	}
	o_UpdateSubresource(c, res, sub, box, data, rowPitch, depthPitch);
}

void STDMETHODCALLTYPE H_OMSetRenderTargets(ID3D11DeviceContext* c, UINT count, ID3D11RenderTargetView* const* rtvs,
											ID3D11DepthStencilView* dsv) {
	if (g_replaying) return o_OMSetRenderTargets(c, count, rtvs, dsv);
	if (ShouldRecord(c)) RecSetRenderTargets(count, rtvs, dsv);
	if (c == s_context) {
		GpuTimingTargets(c, count, rtvs, dsv, "");
		if (dsv) ReprojectNoteDsv(dsv);
		FirstPersonOnSetRenderTargets(count, rtvs, dsv);
		FoveatedOnTargets(c, count, rtvs, dsv);
	}
	if (CaptureRecording(c)) CaptureSetRenderTargets(count, rtvs, dsv);
	o_OMSetRenderTargets(c, count, rtvs, dsv);
}

void STDMETHODCALLTYPE H_RSSetViewports(ID3D11DeviceContext* c, UINT count, const D3D11_VIEWPORT* viewports) {
	if (g_replaying) return o_RSSetViewports(c, count, viewports);
	if (ShouldRecord(c)) RecSetViewports(count, viewports);
	if (CaptureRecording(c)) CaptureViewports(count, viewports);
	o_RSSetViewports(c, count, viewports);
}

void STDMETHODCALLTYPE H_ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* commandList, BOOL restore) {
	if (ShouldRecord(c)) RecUnsupported("ExecuteCommandList (deferred rendering)");
	if (CaptureRecording(c)) CaptureExecuteCommandList();
	o_ExecuteCommandList(c, commandList, restore);
}

void STDMETHODCALLTYPE H_PSSetShaderResources(ID3D11DeviceContext* c, UINT startSlot, UINT count, ID3D11ShaderResourceView* const* views) {
	if (g_replaying) return o_PSSetShaderResources(c, startSlot, count, views);
	if (ShouldRecord(c)) RecSetSRV(RS_PS, startSlot, count, views);
	if (CaptureRecording(c)) CaptureShaderResources(ST_PS, startSlot, count, views);
	o_PSSetShaderResources(c, startSlot, count, views);
}
void STDMETHODCALLTYPE H_CSSetShaderResources(ID3D11DeviceContext* c, UINT startSlot, UINT count, ID3D11ShaderResourceView* const* views) {
	if (g_replaying) return o_CSSetShaderResources(c, startSlot, count, views);
	if (ShouldRecord(c)) RecSetSRV(RS_CS, startSlot, count, views);
	if (CaptureRecording(c)) CaptureShaderResources(ST_CS, startSlot, count, views);
	o_CSSetShaderResources(c, startSlot, count, views);
}
void STDMETHODCALLTYPE H_CSSetUnorderedAccessViews(ID3D11DeviceContext* c, UINT startSlot, UINT count, ID3D11UnorderedAccessView* const* uavs,
												   const UINT* initialCounts) {
	if (g_replaying) return o_CSSetUnorderedAccessViews(c, startSlot, count, uavs, initialCounts);
	if (ShouldRecord(c)) RecCSSetUAV(startSlot, count, uavs, initialCounts);
	if (CaptureRecording(c)) CaptureUnorderedAccessViews(startSlot, count, uavs);
	o_CSSetUnorderedAccessViews(c, startSlot, count, uavs, initialCounts);
}

void STDMETHODCALLTYPE H_CSSetShader(ID3D11DeviceContext* c, void* shader, ID3D11ClassInstance* const* classInstances, UINT count) {
	if (g_replaying) return o_CSSetShader(c, shader, classInstances, count);
	if (ShouldRecord(c)) RecSetShader(RS_CS, shader, classInstances, count);
	if (c == s_context) s_csClass = ShaderTableClassifyCompute(shader);
	if (CaptureRecording(c)) CaptureSetShader(ST_CS, shader);
	o_CSSetShader(c, shader, classInstances, count);
}

void STDMETHODCALLTYPE H_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
	if (g_replaying) return o_Dispatch(c, x, y, z);
	if (ShouldRecord(c)) RecDispatch(x, y, z, c == s_context ? s_csClass : 0);
	if (c == s_context) {
		GpuTimingCompute(c, "", s_csClass);
		GpuTimingWork(c, GPU_WORK_DISPATCH);
	}
	if (CaptureRecording(c)) CaptureDispatch(x, y, z);
	bool runsViewFix = c == s_context && s_csClass != CS_OTHER;
	if (runsViewFix) {  // the fix's own calls aren't the game's: not recorded (playback runs the fix again)
		++g_modCallsInProgress;
		ViewFixBeforeDispatch(c, s_csClass);
		--g_modCallsInProgress;
	}
	o_Dispatch(c, x, y, z);
	if (runsViewFix) {
		++g_modCallsInProgress;
		ViewFixAfterDispatch(c);
		--g_modCallsInProgress;
	}
}

HRESULT STDMETHODCALLTYPE H_CreateVertexShader(ID3D11Device* device, const void* bytecode, SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, void** out) {
	HRESULT hr = o_CreateVertexShader(device, bytecode, bytecodeLength, linkage, out);
	if (device == s_device && SUCCEEDED(hr) && out) ShaderTableRemember(*out, bytecode, bytecodeLength, ST_VS);
	return hr;
}
HRESULT STDMETHODCALLTYPE H_CreatePixelShader(ID3D11Device* device, const void* bytecode, SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, void** out) {
	HRESULT hr = o_CreatePixelShader(device, bytecode, bytecodeLength, linkage, out);
	if (device == s_device && SUCCEEDED(hr) && out) ShaderTableRemember(*out, bytecode, bytecodeLength, ST_PS);
	return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateComputeShader(ID3D11Device* device, const void* bytecode, SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, void** out) {
	HRESULT hr = o_CreateComputeShader(device, bytecode, bytecodeLength, linkage, out);
	if (device == s_device && SUCCEEDED(hr) && out) ShaderTableRemember(*out, bytecode, bytecodeLength, ST_CS);
	return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data,
											ID3D11Texture2D** out) {
	HRESULT hr = o_CreateTexture2D(device, desc, data, out);
	if (device == s_device && SUCCEEDED(hr) && out) TextureTagsOnCreate(*out, desc, data);
	return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateDeferredContext(ID3D11Device* device, UINT flags, ID3D11DeviceContext** out) {
	HRESULT hr = o_CreateDeferredContext(device, flags, out);
	if (device == s_device && SUCCEEDED(hr)) {
		int32_t count = ++s_deferredContextCount;
		Log("d3d hooks: game created deferred context #%d (multithreaded rendering)", (int)count);
	}
	return hr;
}
}  // namespace

// Frame queue: by default DXGI lets the game's CPU run up to 3 frames ahead of the GPU. The head
// pose is read when the CPU starts a frame, so each queued frame adds a frame time of delay before
// it is drawn. [Render] MaxFrameLatency (1 by default; 0 = leave the game's) is applied to the
// game's device and checked every 2 s in case the game sets its own.
void FrameLatencyTick() {
	static DWORD lastCheck;
	static int appliedSetting = -1;
	static UINT gameDefault;  // the game's own queue length, seen before we changed it (0 = not yet)
	DWORD now = GetTickCount();
	if (!s_device || (now - lastCheck < 2000 && appliedSetting == g_cfg.maxFrameLatency)) return;
	lastCheck = now;
	ComPtr<IDXGIDevice1> dxgiDevice;
	if (FAILED(s_device->QueryInterface(__uuidof(IDXGIDevice1), &dxgiDevice)) || !dxgiDevice) return;
	UINT current = 0;
	dxgiDevice->GetMaximumFrameLatency(&current);
	if (!gameDefault) gameDefault = current ? current : 3;
	if (g_cfg.maxFrameLatency == 0 && appliedSetting > 0 && current != gameDefault) {  // back to the game's own
		dxgiDevice->SetMaximumFrameLatency(gameDefault);
		current = gameDefault;
	}
	if (g_cfg.maxFrameLatency > 0 && current != (UINT)g_cfg.maxFrameLatency) {
		HRESULT hr = dxgiDevice->SetMaximumFrameLatency((UINT)g_cfg.maxFrameLatency);
		static int logCount;
		if (logCount++ < 5)
			Log("frame queue: the game's device queued up to %u frames; now %d (0x%08x)%s", current, g_cfg.maxFrameLatency,
				(unsigned)hr, logCount == 5 ? " (not logged again)" : "");
	} else if (appliedSetting != g_cfg.maxFrameLatency) {
		Log("frame queue: up to %u frames (MaxFrameLatency=%d%s)", current, g_cfg.maxFrameLatency,
			g_cfg.maxFrameLatency ? "" : ": the game's own");
	}
	appliedSetting = g_cfg.maxFrameLatency;
}

void D3dHooksInstall(ID3D11Device* gameDevice) {
	static std::atomic<bool> installed = false;
	if (installed.exchange(true)) return;
	if (!g_cfg.probe) Log("frame capture key disabled ([Debug] Probe=0)");
	s_device = gameDevice;
	gameDevice->GetImmediateContext(&s_context);  // keep the reference: lives as long as the game's device
	CaptureSetDevice(s_device, s_context);
	ShaderTableInit();

	void** deviceVtable = *(void***)gameDevice;
	void** contextVtable = *(void***)s_context;
	int hooked = 0, total = 0;
	auto hook = [&](void** vtable, int index, void* detour, void* original, const char* name) {
		total++;
		hooked += HookFunction(vtable[index], detour, (void**)original, name);
	};
	hook(deviceVtable, DEV_CreateTexture2D, (void*)&H_CreateTexture2D, &o_CreateTexture2D, "CreateTexture2D");
	hook(deviceVtable, DEV_CreateVertexShader, (void*)&H_CreateVertexShader, &o_CreateVertexShader, "CreateVertexShader");
	hook(deviceVtable, DEV_CreatePixelShader, (void*)&H_CreatePixelShader, &o_CreatePixelShader, "CreatePixelShader");
	hook(deviceVtable, DEV_CreateComputeShader, (void*)&H_CreateComputeShader, &o_CreateComputeShader, "CreateComputeShader");
	hook(deviceVtable, DEV_CreateDeferredContext, (void*)&H_CreateDeferredContext, &o_CreateDeferredContext,
		 "CreateDeferredContext");
	hook(contextVtable, CTX_VSSetConstantBuffers, (void*)&H_SetConstantBuffers<ST_VS>, &o_SetConstantBuffers[ST_VS], "VSSetConstantBuffers");
	hook(contextVtable, CTX_PSSetConstantBuffers, (void*)&H_SetConstantBuffers<ST_PS>, &o_SetConstantBuffers[ST_PS], "PSSetConstantBuffers");
	hook(contextVtable, CTX_GSSetConstantBuffers, (void*)&H_SetConstantBuffers<ST_GS>, &o_SetConstantBuffers[ST_GS], "GSSetConstantBuffers");
	hook(contextVtable, CTX_CSSetConstantBuffers, (void*)&H_SetConstantBuffers<ST_CS>, &o_SetConstantBuffers[ST_CS], "CSSetConstantBuffers");
	hook(contextVtable, CTX_VSSetShader, (void*)&H_VSSetShader, &o_VSSetShader, "VSSetShader");
	hook(contextVtable, CTX_PSSetShader, (void*)&H_PSSetShader, &o_PSSetShader, "PSSetShader");
	hook(contextVtable, CTX_Draw, (void*)&H_Draw, &o_Draw, "Draw");
	hook(contextVtable, CTX_DrawIndexed, (void*)&H_DrawIndexed, &o_DrawIndexed, "DrawIndexed");
	hook(contextVtable, CTX_DrawInstanced, (void*)&H_DrawInstanced, &o_DrawInstanced, "DrawInstanced");
	hook(contextVtable, CTX_DrawIndexedInstanced, (void*)&H_DrawIndexedInstanced, &o_DrawIndexedInstanced,
		 "DrawIndexedInstanced");
	hook(contextVtable, CTX_Map, (void*)&H_Map, &o_Map, "Map");
	hook(contextVtable, CTX_Unmap, (void*)&H_Unmap, &o_Unmap, "Unmap");
	hook(contextVtable, CTX_UpdateSubresource, (void*)&H_UpdateSubresource, &o_UpdateSubresource, "UpdateSubresource");
	hook(contextVtable, CTX_OMSetRenderTargets, (void*)&H_OMSetRenderTargets, &o_OMSetRenderTargets, "OMSetRenderTargets");
	hook(contextVtable, CTX_RSSetViewports, (void*)&H_RSSetViewports, &o_RSSetViewports, "RSSetViewports");
	hook(contextVtable, CTX_ExecuteCommandList, (void*)&H_ExecuteCommandList, &o_ExecuteCommandList, "ExecuteCommandList");
	hook(contextVtable, CTX_Dispatch, (void*)&H_Dispatch, &o_Dispatch, "Dispatch");
	hook(contextVtable, CTX_PSSetShaderResources, (void*)&H_PSSetShaderResources, &o_PSSetShaderResources, "PSSetShaderResources");
	hook(contextVtable, CTX_CSSetShaderResources, (void*)&H_CSSetShaderResources, &o_CSSetShaderResources, "CSSetShaderResources");
	hook(contextVtable, CTX_CSSetUnorderedAccessViews, (void*)&H_CSSetUnorderedAccessViews, &o_CSSetUnorderedAccessViews, "CSSetUnorderedAccessViews");
	hook(contextVtable, CTX_CSSetShader, (void*)&H_CSSetShader, &o_CSSetShader, "CSSetShader");
	ReplayInstall(s_context);
	Log("D3D11 hooks ready: %d/%d installed (capture key 0x%x)", hooked, total, (unsigned)g_cfg.keyCapture);
}

void D3dHooksFollowDevice(ID3D11Device* presentingDevice) {
	if (!s_device || presentingDevice == s_device) return;
	Log("d3d hooks: the presenting device differs from the first created device - following the presenting one");
	if (s_context) s_context->Release();
	s_device = presentingDevice;
	s_device->GetImmediateContext(&s_context);
	CaptureSetDevice(s_device, s_context);
}

void OriginalDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) { o_Dispatch(c, x, y, z); }

void OriginalDraw(ID3D11DeviceContext* c, const DrawArgs& args) {
	switch (args.kind) {
		case DRAW: o_Draw(c, args.arg0, args.arg1); break;
		case DRAW_INDEXED: o_DrawIndexed(c, args.arg0, args.arg1, args.arg2); break;
		case DRAW_INSTANCED: o_DrawInstanced(c, args.arg0, args.arg1, (UINT)args.arg2, args.arg3); break;
		case DRAW_INDEXED_INSTANCED: o_DrawIndexedInstanced(c, args.arg0, args.arg1, (UINT)args.arg2, (INT)args.arg3, args.arg4); break;
	}
}

void OriginalSetRenderTargets(ID3D11DeviceContext* c, UINT count, ID3D11RenderTargetView* const* rtvs,
							  ID3D11DepthStencilView* dsv) {
	o_OMSetRenderTargets(c, count, rtvs, dsv);
}

const void* PatchConstantBufferForReplay(ID3D11Resource* res, const void* data, UINT size, float* scratch) {
	data = StereoPatchUpdate(size, data, scratch);
	return GameEffectsPatchUpdate(res, data, size, scratch);
}
