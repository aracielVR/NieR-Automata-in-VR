// frame_handoff.cpp - hooks IDXGISwapChain::Present (inline detour via MinHook) and copies each
// finished game frame into a texture shared with the VR thread. The game is never made to wait.
#include "render/frame_handoff.h"
#include "core/com.h"
#include "core/locks.h"
#include "core/config.h"
#include "core/d3d11_proxy.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/main.h"
#include "debug/hot_module.h"
#include "debug/position_finder.h"
#include "debug/frame_capture.h"
#include "debug/gpu_timing.h"
#include "render/camera.h"
#include "render/d3d_hooks.h"
#include "game/first_person.h"
#include "render/extra_resolutions.h"
#include "render/game_resolution.h"
#include "render/foveated.h"
#include "render/frame_share.h"
#include "render/game_effects.h"
#include "ui/hud_layer.h"
#include "render/replay.h"
#include "render/reproject.h"
#include "render/upscale.h"
#include "xr/xr_main.h"
#include <atomic>
#include <string.h>

FrameShare g_share;

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
namespace {
constexpr int32_t kPresentIndex = 8;  // IUnknown(3) + IDXGIObject(4) + IDXGIDeviceSubObject(1) -> Present
constexpr int32_t kMaxTargets = 2;

PFN_Present o_Present[kMaxTargets];
void* s_presentTargets[kMaxTargets];
std::atomic<int32_t> s_presentHookCount;
SRWLOCK s_presentHookLock = SRWLOCK_INIT;
DWORD s_presentDepthTls = TLS_OUT_OF_INDEXES;  // per-thread Present nesting depth
}  // namespace

ID3D11DeviceContext* g_replayCtx;

typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
namespace {
constexpr int32_t kResizeBuffersIndex = 13;
PFN_ResizeBuffers o_ResizeBuffers;
std::atomic<int32_t> s_resizeLogs;

// Game-side capture state (only touched at nesting depth 0 on the presenting thread)
//
// The VR thread owns a ring of kRingSize shared textures. Each frame the backbuffer is copied into
// the next free ring slot and an event query is issued behind the copy; once the GPU has finished it
// (polled without blocking), the frame is published with its eye/pose tag. The game device cannot
// use keyed mutexes (DXGI 1.0 factory), so the query is the synchronisation: a slot is rewritten
// once the ring comes round to it again (several game frames), normally long after the VR thread
// has copied it (nothing checks that it has).
ID3D11Device* s_device;
ID3D11Texture2D* s_ring[kRingSize];
ID3D11Texture2D* s_ringScene[kRingSize];
ID3D11Texture2D* s_ringKey[kRingSize];  // upscaling: the scene image before upscaling, for the HUD key
ID3D11Query* s_copyQueries[kRingSize];
bool s_slotPending[kRingSize];
FrameTag s_slotTag[kRingSize];
int32_t s_openedGen = -1;
int32_t s_nextSlot;
UINT s_requestedW, s_requestedH;
UINT s_requestedSceneW, s_requestedSceneH;  // scene-only images: enlarged when upscaling (upscale.cpp)
bool s_requestedKey;                        // upscaling: the scene images before upscaling too
DXGI_FORMAT s_requestedFormat;
bool s_loggedNotD3D11, s_loggedOtherDevice;
uint32_t s_presentCount;
std::atomic<int32_t> s_openFailLogs, s_nestedLogs, s_ringFullLogs;

DXGI_FORMAT ConcreteFormat(DXGI_FORMAT format) {
	switch (format) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default: return format;
	}
}

void ReleaseRing() {
	for (int i = 0; i < kRingSize; i++) {
		SafeRelease(s_ring[i]);
		SafeRelease(s_ringScene[i]);
		SafeRelease(s_ringKey[i]);
		SafeRelease(s_copyQueries[i]);
		s_slotPending[i] = false;
	}
}

void PublishSlotLocked(int slot) {
	const FrameTag& tag = s_slotTag[slot];
	int kind = (tag.eye >= 0 && tag.patched) ? tag.eye : 2;
	if (g_share.ringGen == s_openedGen) {
		g_share.latest[kind].seq = ++g_share.lastSeq;
		g_share.latest[kind].slot = slot;
		g_share.latest[kind].tag = tag;
	}
}

// Publishes ring slots whose copy the GPU has finished. Never blocks. The two images of a Synchronized
// frame (same pairId) are published together, once both are done, so the headset never shows the
// left eye of one game frame with the right eye of another (the right image is finished a replay
// later than the left one).
void PollPending() {
	bool finished[kRingSize] = {};
	for (int i = 0; i < kRingSize; i++)
		finished[i] = s_slotPending[i] && s_copyQueries[i] &&
					  g_replayCtx->GetData(s_copyQueries[i], nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
	SectionLock guard(&g_share.lock);
	for (int i = 0; i < kRingSize; i++) {
		if (!finished[i] || !s_slotPending[i]) continue;
		uint32_t pair = s_slotTag[i].pairId;
		if (!pair) {
			s_slotPending[i] = false;
			PublishSlotLocked(i);
			continue;
		}
		int partner = -1;
		for (int k = 0; k < kRingSize; k++)
			if (k != i && s_slotPending[k] && s_slotTag[k].pairId == pair) partner = k;
		if (partner < 0) {  // partner already gone (shouldn't happen): publish alone
			s_slotPending[i] = false;
			PublishSlotLocked(i);
		} else if (finished[partner]) {
			int leftSlot = s_slotTag[i].eye == 0 ? i : partner, rightSlot = leftSlot == i ? partner : i;
			s_slotPending[leftSlot] = s_slotPending[rightSlot] = false;
			PublishSlotLocked(leftSlot);
			PublishSlotLocked(rightSlot);
		}
	}
}
}  // namespace

// From the game's draws (render thread, every 128 draws): frames finished by the GPU are handed to
// the headset right away instead of at the next Present (up to a game frame less latency).
void CapturePollMidFrame() {
	static uint32_t drawCalls;
	if (++drawCalls % 128 || !s_ring[0] || !g_replayCtx) return;
	PollPending();
}

namespace {
int FreeSlots() {
	int count = 0;
	for (int i = 0; i < kRingSize; i++) count += !s_slotPending[i];
	return count;
}

// One-time probe of what the game's device supports (logged for diagnostics).
void ProbeGameDevice(ID3D11Device* device) {
	Log("game device creation flags 0x%x", device->GetCreationFlags());
	struct {
		const char* name;
		UINT bind, misc;
	} tests[] = {
		{"plain", D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0},
		{"shared", D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, D3D11_RESOURCE_MISC_SHARED},
		{"shared+keyedmutex", D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX},
	};
	for (auto& test : tests) {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = desc.Height = 64;
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = test.bind;
		desc.MiscFlags = test.misc;
		ComPtr<ID3D11Texture2D> tex;
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex);
		Log("game device probe: %s texture -> 0x%08x", test.name, (unsigned)hr);
	}
}

void OnFirstGameDevice(ID3D11Device* device) {
	s_device = device;  // keeps the reference from GetDevice
	// s_device->CreateDeferredContext(0, &g_replayCtx);
	s_device->GetImmediateContext(&g_replayCtx);
	ComPtr<IDXGIDevice> dxgiDevice;
	if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), &dxgiDevice))) {
		ComPtr<IDXGIAdapter> adapter;
		if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
			DXGI_ADAPTER_DESC desc;
			if (SUCCEEDED(adapter->GetDesc(&desc))) {
				g_share.gameLuid = desc.AdapterLuid;
				g_share.gameLuidValid = true;
				Log("game GPU: %s (LUID %08x:%08x, %u MB VRAM)", Utf8(desc.Description).s, (unsigned)desc.AdapterLuid.HighPart,
					(unsigned)desc.AdapterLuid.LowPart, (unsigned)(desc.DedicatedVideoMemory >> 20));
			}
			ComPtr<IDXGIFactory1> factory1;
			HRESULT hr = adapter->GetParent(__uuidof(IDXGIFactory1), &factory1);
			Log("game adapter's factory is DXGI %s", SUCCEEDED(hr) ? "1.1+" : "1.0");
		}
	}
	Log("game device feature level 0x%x", (unsigned)device->GetFeatureLevel());
	D3dHooksFollowDevice(device);
	ProbeGameDevice(device);
	Log("starting VR thread");
	XrStartThread();
}

// Copies a finished image (and the scene-only image, if any) into the next free ring slot, tagged
// with the eye the stereo code says was just drawn. full = null: no full image (a reprojected eye
// while upscaling: only its scene image is shown). key: upscaling, the scene image before upscaling.
int PushToRing(ID3D11Texture2D* full, bool msaa, DXGI_FORMAT format, ID3D11Texture2D* scene, bool noUI,
			   const FrameTag* tagOverride = nullptr, ID3D11Texture2D* key = nullptr) {
	int slot = -1;
	for (int k = 0; k < kRingSize && slot < 0; k++)
		if (!s_slotPending[(s_nextSlot + k) % kRingSize]) slot = (s_nextSlot + k) % kRingSize;
	if (slot < 0) {
		if (LogBudget(s_ringFullLogs, 3)) Log("note: capture ring full (GPU behind), skipped a frame");
		return -1;
	}
	if (full && msaa)
		g_replayCtx->ResolveSubresource(s_ring[slot], 0, full, 0, format);
	else if (full)
		g_replayCtx->CopyResource(s_ring[slot], full);
	if (scene) g_replayCtx->CopyResource(s_ringScene[slot], scene);
	if (scene && key && s_ringKey[slot]) g_replayCtx->CopyResource(s_ringKey[slot], key);
	g_replayCtx->End(s_copyQueries[slot]);
	if (tagOverride)
		s_slotTag[slot] = *tagOverride;
	else
		StereoGetFrameTag(&s_slotTag[slot]);
	s_slotTag[slot].hasScene = scene != nullptr;
	s_slotTag[slot].noUI = noUI;
	s_slotTag[slot].pairId = 0;
	s_slotPending[slot] = true;
	s_nextSlot = (slot + 1) % kRingSize;
	return slot;
}
uint32_t s_lastPairId;
uint32_t s_droppedPairs;
DWORD s_dropReportTick;

void CaptureFrame(IDXGISwapChain* swapChain) {
	ComPtr<ID3D11Device> device;
	if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), &device)) || !device) {
		if (!s_loggedNotD3D11) Log("note: a non-D3D11 swapchain presented (ignored)");
		s_loggedNotD3D11 = true;
		return;
	}
	if (!s_device) {
		OnFirstGameDevice(device.Get());
	} else {
		if (device.Get() != s_device) {
			if (!s_loggedOtherDevice) Log("note: Present from a second D3D11 device (overlay?) ignored");
			s_loggedOtherDevice = true;
			return;
		}
	}

	ComPtr<ID3D11Texture2D> backbuffer;
	if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backbuffer)) || !backbuffer) return;
	D3D11_TEXTURE2D_DESC desc;
	backbuffer->GetDesc(&desc);
	DXGI_FORMAT format = ConcreteFormat(desc.Format);
	if (s_presentCount++ == 0) {
		Log("first Present: backbuffer %ux%u format %d msaa %u", desc.Width, desc.Height, (int)desc.Format, desc.SampleDesc.Count);
		ExtraResolutionsOnFirstPresent(swapChain);
	}
	StereoSetAspect((float)desc.Width / (float)desc.Height);
	FirstPersonSetGameSize(desc.Width, desc.Height);
	FoveatedSetGameSize(desc.Width, desc.Height);
	UiSetBackbuffer(backbuffer.Get());
	ReprojectSetBackbufferSize(desc.Width, desc.Height);
	UINT sceneW, sceneH;  // size of this frame's scene-only images
	// upscaling (also DLAA, which keeps the size): the scene images are DLSS's
	const bool upscaling = UpscaleBeginPresent(g_replayCtx, desc.Width, desc.Height, &sceneW, &sceneH);

	// 1. Ask the VR thread for a ring matching the backbuffer (only when it changes).
	if (desc.Width != s_requestedW || desc.Height != s_requestedH || format != s_requestedFormat || sceneW != s_requestedSceneW || sceneH != s_requestedSceneH ||
		upscaling != s_requestedKey) {
		s_requestedW = desc.Width;
		s_requestedH = desc.Height;
		s_requestedSceneW = sceneW;
		s_requestedSceneH = sceneH;
		s_requestedKey = upscaling;
		s_requestedFormat = format;
		ReleaseRing();
		{
			SectionLock guard(&g_share.lock);
			g_share.requestW = desc.Width;
			g_share.requestH = desc.Height;
			g_share.requestSceneW = sceneW;
			g_share.requestSceneH = sceneH;
			g_share.requestKey = upscaling;
			g_share.requestFormat = format;
			g_share.requestGen++;
		}
		if (upscaling)
			Log("requesting %ux%u format %d capture textures, scene images upscaled to %ux%u", desc.Width, desc.Height, (int)format,
				sceneW, sceneH);
		else
			Log("requesting %ux%u format %d capture textures", desc.Width, desc.Height, (int)format);
	}

	// 2. Open the ring once the VR thread has published it.
	EnterCriticalSection(&g_share.lock);
	HANDLE handles[kRingSize], sceneHandles[kRingSize], keyHandles[kRingSize];
	memcpy(handles, g_share.ring, sizeof(handles));
	memcpy(sceneHandles, g_share.ringScene, sizeof(sceneHandles));
	memcpy(keyHandles, g_share.ringKey, sizeof(keyHandles));
	int32_t gen = g_share.ringGen;
	bool current = g_share.forRequestGen == g_share.requestGen;
	LeaveCriticalSection(&g_share.lock);
	if (handles[0] && current && gen != s_openedGen) {
		ReleaseRing();
		s_openedGen = gen;  // one attempt per published ring
		HRESULT hr = S_OK;
		D3D11_QUERY_DESC queryDesc = {D3D11_QUERY_EVENT, 0};
		for (int i = 0; i < kRingSize && SUCCEEDED(hr); i++) {
			hr = s_device->OpenSharedResource(handles[i], __uuidof(ID3D11Texture2D), (void**)&s_ring[i]);
			if (SUCCEEDED(hr))
				hr = s_device->OpenSharedResource(sceneHandles[i], __uuidof(ID3D11Texture2D), (void**)&s_ringScene[i]);
			if (SUCCEEDED(hr) && keyHandles[i])
				hr = s_device->OpenSharedResource(keyHandles[i], __uuidof(ID3D11Texture2D), (void**)&s_ringKey[i]);
			if (SUCCEEDED(hr)) hr = s_device->CreateQuery(&queryDesc, &s_copyQueries[i]);
		}
		if (FAILED(hr)) {
			if (LogBudget(s_openFailLogs, 5))
				Log("ERROR: game device could not open the capture textures (0x%08x)", (unsigned)hr);
			ReleaseRing();
		} else {
			Log("game side: capture ring opened (generation %d)", (int)gen);
		}
	}

	// 3. Publish finished copies, then copy this frame into the next free slot.
	if (s_ring[0]) {
		PollPending();
		ID3D11Texture2D* scene = UiSceneTexture();
		bool synchronized = StereoSynchronizedFrame() && ReplayAvailable();
		if (synchronized && FreeSlots() < 2) {
			// Synchronized and the GPU is behind: drop this game frame for the headset (both eyes keep
			// the previous pair) and skip the replay, which lets the GPU catch up.
			s_droppedPairs++;
		} else if (!synchronized && g_cfg.depthReproject && StereoFrameEye() >= 0 && desc.SampleDesc.Count == 1 && FreeSlots() >= 2 &&
				   (!upscaling || scene)) {
			// Alternating with depth reprojection: this frame's eye, plus the other eye's last
			// image moved to this frame's camera, handed to the headset as a pair. Upscaling: the
			// upscaled images are moved.
			GpuTimingMark(g_replayCtx, "upscale");
			ID3D11Texture2D* upscaledScene = UpscaleScene(g_replayCtx, StereoFrameEye(), scene);
			GpuTimingMark(g_replayCtx, "eye to headset");
			int eyeSlot = PushToRing(backbuffer.Get(), false, format, upscaledScene, false, nullptr, scene);
			FrameTag tag;
			GpuTimingMark(g_replayCtx, "depth reprojection");
			ID3D11Texture2D* warped = eyeSlot >= 0 && (!upscaling || upscaledScene) ? ReprojectOtherEye(g_replayCtx, upscaledScene, backbuffer.Get(), &tag) : nullptr;
			int otherSlot = warped ? PushToRing(upscaling ? nullptr : warped, false, format, warped, true, &tag) : -1;
			if (otherSlot >= 0) {
				uint32_t pairId = ++s_lastPairId ? s_lastPairId : ++s_lastPairId;
				s_slotTag[eyeSlot].pairId = s_slotTag[otherSlot].pairId = pairId;
			}
		} else {
			GpuTimingMark(g_replayCtx, "upscale");
			ID3D11Texture2D* upscaledScene = UpscaleScene(g_replayCtx, StereoFrameEye(), scene);
			GpuTimingMark(g_replayCtx, "eye to headset");
			int left = PushToRing(backbuffer.Get(), desc.SampleDesc.Count > 1, format, upscaledScene, false, nullptr, scene);
			// Synchronized: the same frame again for the right eye (replay.cpp), into the next slot.
			if (synchronized && left >= 0) {
				bool inBackbuffer = false;
				int right = -1;
				if (ReplaySecondEye(g_replayCtx, &inBackbuffer)) {
					if (inBackbuffer) {
						GpuTimingMark(g_replayCtx, "R eye to headset");
						right = PushToRing(backbuffer.Get(), desc.SampleDesc.Count > 1, format, nullptr, false);
					} else if (scene) {
						GpuTimingMark(g_replayCtx, "R upscale");
						UpscaleCaptureDepth(g_replayCtx, 1);  // the replay left the right eye's depth behind
						ID3D11Texture2D* upscaledRight = UpscaleScene(g_replayCtx, 1, scene);
						GpuTimingMark(g_replayCtx, "R eye to headset");
						right = PushToRing(scene, false, format, upscaledRight, true);
					}
				}
				if (right >= 0) {
					uint32_t pairId = ++s_lastPairId ? s_lastPairId : ++s_lastPairId;
					s_slotTag[left].pairId = s_slotTag[right].pairId = pairId;
				}
			}
		}
		if (s_droppedPairs && GetTickCount() - s_dropReportTick > 30000) {
			s_dropReportTick = GetTickCount();
			Log("synchronized: %u game frames not shown in the last 30 s (GPU behind: lower the game's resolution)",
				s_droppedPairs);
			s_droppedPairs = 0;
		}
	}
}

// Right after the game's Present: the copy has been submitted with it.
void AfterPresent() {
	// The game installs its own unhandled-exception filter after ours, so a crash never reached our
	// report. Put ours back every few seconds: it chains to the game's, so nothing is taken away.
	static DWORD s_filterRefreshed;
	DWORD now = GetTickCount();
	if (now - s_filterRefreshed > 3000) {
		s_filterRefreshed = now;
		InstallCrashHandler();
	}
	GameResolutionTick();
	FrameLatencyTick();
	if (s_ring[0]) PollPending();
	UiBeginFrame();
	GameEffectsBeginFrame();
	StereoBeginFrame();
	if (g_replayCtx) {
		ReplayFrameStart(g_replayCtx, StereoSynchronizedFrame());
		GpuTimingPoll(g_replayCtx);
		GpuTimingFrameStart(g_replayCtx);
	}
	ReplayReport();
}

HRESULT PresentCommon(int hookIndex, void* returnAddress, IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
	size_t depth = (size_t)TlsGetValue(s_presentDepthTls);
	TlsSetValue(s_presentDepthTls, (void*)(depth + 1));
	if (depth == 0) {
		static bool guaranteed = false;
		if (!guaranteed) {
			// Leave stack room for crash reporting on the render thread.
			ULONG size = 64 * 1024;
			SetThreadStackGuarantee(&size);
			guaranteed = true;
		}
		if (!(flags & DXGI_PRESENT_TEST)) {
			ReplayStop(false);  // Synchronized: the game's frame is complete
			if (g_replayCtx) GpuTimingMark(g_replayCtx, "present");
			if (g_replayCtx) FoveatedSceneEnd(g_replayCtx);  // a frame without UI: off before our own passes
			CaptureBeforePresent();
			PositionFinderPoll();
			HotModulePoll();
			CaptureFrame(swapChain);
			if (g_replayCtx) GpuTimingFrameEnd(g_replayCtx);
		}
		if (syncInterval && !g_cfg.gameVsync) {
			static bool logged = false;
			if (!logged) Log("game V-sync: the game presents with sync interval %u; ignored (GameVsync=0)", syncInterval);
			logged = true;
			syncInterval = 0;
		}
	} else {
		if (LogBudget(s_nestedLogs, 5)) {
			// Present re-entered on the same thread (some overlay/wrapper calls it from inside Present).
			char caller[300];
			DescribeAddress(returnAddress, caller, sizeof(caller));
			Log("note: nested Present (depth %u, hook %d) called from %s - not capturing", (unsigned)depth, hookIndex, caller);
		}
		if (depth >= 32) {
			// Runaway recursion. Break the loop instead of overflowing the stack.
			static bool logged = false;
			if (!logged) Log("ERROR: Present recursion depth %u - breaking the loop", (unsigned)depth);
			logged = true;
			TlsSetValue(s_presentDepthTls, (void*)depth);
			return S_OK;
		}
	}
	HRESULT hr = o_Present[hookIndex](swapChain, syncInterval, flags);
	if (depth == 0 && !(flags & DXGI_PRESENT_TEST)) {
		AfterPresent();
		CaptureAfterPresent();
	}
	TlsSetValue(s_presentDepthTls, (void*)depth);
	return hr;
}

HRESULT STDMETHODCALLTYPE HookedPresent0(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
	return PresentCommon(0, __builtin_return_address(0), swapChain, syncInterval, flags);
}
HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
	return PresentCommon(1, __builtin_return_address(0), swapChain, syncInterval, flags);
}

void InstallPresentHook(void* target, const char* how) {
	char where[300];
	DescribeAddress(target, where, sizeof(where));
	ExclusiveLock guard(&s_presentHookLock);
	for (int32_t i = 0; i < s_presentHookCount; i++)
		if (s_presentTargets[i] == target) {
			return;
		}
	if (target == (void*)&HookedPresent0 || target == (void*)&HookedPresent1) {
		return;
	}
	if (s_presentHookCount >= kMaxTargets) {
		Log("ERROR: more Present implementations than expected (%s)", where);
		return;
	}
	if (s_presentDepthTls == TLS_OUT_OF_INDEXES) s_presentDepthTls = TlsAlloc();
	int32_t index = s_presentHookCount;
	void* detour = index == 0 ? (void*)&HookedPresent0 : (void*)&HookedPresent1;
	if (!HookFunction(target, detour, (void**)&o_Present[index], "Present")) {
		return;
	}
	s_presentTargets[index] = target;
	s_presentHookCount = index + 1;
	Log("Present hooked (%s) at %s", how, where);
}

// The backbuffer changes size: every feature frees its textures of the old size first.
HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format,
											  UINT flags) {
	RememberGameSize(width, height);
	UiOnResizeBuffers();
	ReplayRelease();
	ReprojectRelease();
	UpscaleRelease();
	HRESULT hr = o_ResizeBuffers(swapChain, count, width, height, format, flags);
	if (LogBudget(s_resizeLogs, 8))
		Log("swapchain: ResizeBuffers %ux%u format %d -> 0x%08x", width, height, (int)format, (unsigned)hr);
	return hr;
}
}  // namespace

void HookFromSwapChain(IDXGISwapChain* swapChain) { InstallPresentHook((*(void***)swapChain)[kPresentIndex], "game swapchain"); }

void HookWithDummySwapChain() {
	static std::atomic<bool> installed = false;
	if (installed.exchange(true)) return;

	WNDCLASSEXW windowClass = {};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = DefWindowProcW;
	windowClass.hInstance = g_hSelf;
	windowClass.lpszClassName = L"NieRVR_Dummy";
	RegisterClassExW(&windowClass);
	HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, g_hSelf,
								nullptr);
	if (!hwnd) {
		Log("ERROR: dummy window failed (%u)", (unsigned)GetLastError());
		return;
	}
	DXGI_SWAP_CHAIN_DESC desc = {};
	desc.BufferDesc.Width = 64;
	desc.BufferDesc.Height = 64;
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 1;
	desc.OutputWindow = hwnd;
	desc.Windowed = TRUE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	ComPtr<IDXGISwapChain> swapChain;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	HRESULT hr = E_FAIL;
	const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
	for (D3D_DRIVER_TYPE driverType : types) {
		hr = g_realCreateDeviceAndSwapChain(nullptr, driverType, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain, &device,
											nullptr, &context);
		if (SUCCEEDED(hr)) break;
	}
	if (SUCCEEDED(hr) && swapChain) {
		InstallPresentHook((*(void***)swapChain.Get())[kPresentIndex], "via dummy swapchain");
		HookFunction((*(void***)swapChain.Get())[kResizeBuffersIndex], (void*)&HookedResizeBuffers, (void**)&o_ResizeBuffers,
					 "IDXGISwapChain::ResizeBuffers");
		ExtraResolutionsHookSwapChain(swapChain.Get());
	} else {
		Log("ERROR: dummy swapchain creation failed 0x%08x - VR will not start", (unsigned)hr);
	}
	swapChain.Reset();  // a swapchain goes before its window
	context.Reset();
	device.Reset();
	DestroyWindow(hwnd);
	UnregisterClassW(windowClass.lpszClassName, g_hSelf);
}
