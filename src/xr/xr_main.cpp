// xr_main.cpp - the VR thread: OpenXR instance, system and session, swapchains, events, and the thread's loop.
#include "xr/xr_main.h"
#include "core/locks.h"
#include "xr/xr_thread.h"
#include "core/config.h"
#include "core/log.h"
#include "core/main.h"
#include "input/vr_controllers.h"
#include "render/camera.h"
#include "xr/runtime_loader.h"
#include <atomic>
#include <stdio.h>

VrState g_vr;
std::atomic<DWORD> g_xrThreadId;

namespace {
// Error helpers
const char* XrStr(XrResult result) {
	static char buf[XR_MAX_RESULT_STRING_SIZE + 16];
	if (g_vr.instance && g_vr.xrResultToString && XR_SUCCEEDED(g_vr.xrResultToString(g_vr.instance, result, buf))) return buf;
	snprintf(buf, sizeof(buf), "XrResult(%d)", (int)result);
	return buf;
}
}  // namespace

bool XrCheck(XrResult result, const char* what, bool perFrame) {
	if (XR_SUCCEEDED(result)) return true;
	if (!perFrame || g_vr.errorCount < 20 || (g_vr.errorCount % 1000) == 0)
		Log("OpenXR error: %s -> %s%s", what, XrStr(result), perFrame && g_vr.errorCount == 19 ? " (further per-frame errors throttled)" : "");
	if (perFrame) g_vr.errorCount++;
	if (result == XR_ERROR_SESSION_LOST) g_vr.sessionLost = true;
	if (result == XR_ERROR_INSTANCE_LOST || result == XR_ERROR_RUNTIME_FAILURE) g_vr.instanceLost = true;
	return false;
}
namespace {
// Instance & system
bool InitInstance() {
	g_vr.getProcAddr = XrLoadEntry();
	if (!g_vr.getProcAddr) return false;

	PFN_xrEnumerateInstanceExtensionProperties enumExtensions = nullptr;
	PFN_xrCreateInstance createInstance = nullptr;
	g_vr.getProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumExtensions);
	g_vr.getProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&createInstance);
	if (!enumExtensions || !createInstance) {
		Log("ERROR: OpenXR: runtime is missing core functions");
		return false;
	}
	uint32_t extensionCount = 0;
	if (!XR_CHECK(enumExtensions(nullptr, 0, &extensionCount, nullptr))) return false;
	static XrExtensionProperties extensions[256];
	if (extensionCount > 256) extensionCount = 256;
	for (uint32_t i = 0; i < extensionCount; i++) {
		extensions[i] = {};
		extensions[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	}
	if (!XR_CHECK(enumExtensions(nullptr, extensionCount, &extensionCount, extensions))) return false;
	bool hasD3D11 = false, hasPico = false, hasHandTracking = false;
	for (uint32_t i = 0; i < extensionCount; i++) {
		LogV("OpenXR extension: %s v%u", extensions[i].extensionName, extensions[i].extensionVersion);
		if (!lstrcmpA(extensions[i].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) hasD3D11 = true;
		if (!lstrcmpA(extensions[i].extensionName, "XR_BD_controller_interaction")) hasPico = true;  // Pico 4 / Neo3 controllers
		if (!lstrcmpA(extensions[i].extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME)) hasHandTracking = true;  // finger curls
	}
	if (!hasD3D11) {
		Log("ERROR: OpenXR runtime does not support Direct3D 11 (XR_KHR_D3D11_enable)");
		return false;
	}

	const char* enabled[3] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
	uint32_t enabledCount = 1;
	if (hasPico) enabled[enabledCount++] = "XR_BD_controller_interaction";
	if (hasHandTracking) enabled[enabledCount++] = XR_EXT_HAND_TRACKING_EXTENSION_NAME;
	XrInstanceCreateInfo createInfo = {};
	createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
	lstrcpynA(createInfo.applicationInfo.applicationName, "NieRVR", XR_MAX_APPLICATION_NAME_SIZE);
	createInfo.applicationInfo.applicationVersion = 1;
	lstrcpynA(createInfo.applicationInfo.engineName, "NieRVR", XR_MAX_ENGINE_NAME_SIZE);
	createInfo.applicationInfo.engineVersion = 1;
	createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	createInfo.enabledExtensionCount = enabledCount;
	createInfo.enabledExtensionNames = enabled;
	XrResult result = createInstance(&createInfo, &g_vr.instance);
	if (result == XR_ERROR_API_VERSION_UNSUPPORTED) {
		Log("OpenXR: runtime rejected API 1.0, retrying with 1.1");
		createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_1;
		result = createInstance(&createInfo, &g_vr.instance);
	}
	if (!XrCheck(result, "xrCreateInstance")) {
		g_vr.instance = XR_NULL_HANDLE;
		return false;
	}

	bool ok = true;
#define LOAD_FN(n)                                                                                 \
	if (XR_FAILED(g_vr.getProcAddr(g_vr.instance, #n, (PFN_xrVoidFunction*)&g_vr.n)) || !g_vr.n) { \
		Log("ERROR: OpenXR: missing function " #n);                                                \
		ok = false;                                                                                \
	}
	XR_INSTANCE_FUNCS(LOAD_FN)
#undef LOAD_FN
	if (!ok) return false;
	VrInputInit(g_vr.getProcAddr, g_vr.instance, hasPico, hasHandTracking);

	XrInstanceProperties props = {};
	props.type = XR_TYPE_INSTANCE_PROPERTIES;
	if (XR_SUCCEEDED(g_vr.xrGetInstanceProperties(g_vr.instance, &props)))
		Log("OpenXR runtime: %s %u.%u.%u", props.runtimeName, (unsigned)XR_VERSION_MAJOR(props.runtimeVersion),
			(unsigned)XR_VERSION_MINOR(props.runtimeVersion), (unsigned)XR_VERSION_PATCH(props.runtimeVersion));
	return true;
}

bool InitSystem() {
	static bool loggedWaiting = false;
	XrSystemGetInfo getInfo = {};
	getInfo.type = XR_TYPE_SYSTEM_GET_INFO;
	getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrResult result = g_vr.xrGetSystem(g_vr.instance, &getInfo, &g_vr.system);
	if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
		if (!loggedWaiting) Log("OpenXR: headset not available yet (is it connected and awake?) - waiting");
		loggedWaiting = true;
		return false;
	}
	if (!XR_CHECK(result)) return false;
	loggedWaiting = false;

	XrSystemProperties props = {};
	props.type = XR_TYPE_SYSTEM_PROPERTIES;
	if (XR_SUCCEEDED(g_vr.xrGetSystemProperties(g_vr.instance, g_vr.system, &props))) {
		Log("headset: %s (vendor 0x%x), max swapchain %ux%u, max layers %u, position tracking %d", props.systemName,
			props.vendorId, props.graphicsProperties.maxSwapchainImageWidth, props.graphicsProperties.maxSwapchainImageHeight,
			props.graphicsProperties.maxLayerCount, (int)props.trackingProperties.positionTracking);
		g_vr.maxSwapchainW = props.graphicsProperties.maxSwapchainImageWidth;
		g_vr.maxSwapchainH = props.graphicsProperties.maxSwapchainImageHeight;
	}

	// the mod renders a stereo pair: the system must offer the primary stereo view configuration
	{
		XrViewConfigurationType types[16];
		uint32_t typeCount = 0;
		bool stereo = false;
		if (XR_SUCCEEDED(g_vr.xrEnumerateViewConfigurations(g_vr.instance, g_vr.system, 16, &typeCount, types)))
			for (uint32_t i = 0; i < typeCount; i++) stereo |= types[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		if (!stereo) {
			Log("ERROR: this OpenXR system has no stereo view configuration (%u others)", typeCount);
			return false;
		}
	}

	// the per-eye image size the runtime recommends (for the resolution hint)
	{
		XrViewConfigurationView configViews[2] = {};
		configViews[0].type = configViews[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
		uint32_t viewCount = 0;
		if (XR_SUCCEEDED(g_vr.xrEnumerateViewConfigurationViews(g_vr.instance, g_vr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
																&viewCount, configViews)) &&
			viewCount == 2) {
			g_vr.recommendedW = configViews[0].recommendedImageRectWidth;
			g_vr.recommendedH = configViews[0].recommendedImageRectHeight;
			Log("headset recommends %ux%u per eye (max %ux%u)", g_vr.recommendedW, g_vr.recommendedH, configViews[0].maxImageRectWidth,
				configViews[0].maxImageRectHeight);
		}
	}

	XrGraphicsRequirementsD3D11KHR requirements = {};
	requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
	if (!XR_CHECK(g_vr.xrGetD3D11GraphicsRequirementsKHR(g_vr.instance, g_vr.system, &requirements))) return false;
	g_vr.headsetGpuLuid = requirements.adapterLuid;
	g_vr.minFeatureLevel = requirements.minFeatureLevel;
	Log("headset GPU LUID %08x:%08x, min feature level 0x%x", (unsigned)requirements.adapterLuid.HighPart,
		(unsigned)requirements.adapterLuid.LowPart, (unsigned)requirements.minFeatureLevel);
	return true;
}

// After OpenXR tells us which GPU drives the headset: it must be the game's GPU.
bool CheckHeadsetGpu() {
	if (g_vr.headsetGpuLuid.LowPart != g_vr.deviceLuid.LowPart || g_vr.headsetGpuLuid.HighPart != g_vr.deviceLuid.HighPart) {
		Log("ERROR: the headset is driven by a different GPU than the game. On laptops/multi-GPU systems, set "
			"NieRAutomata.exe to the high-performance GPU in Windows Settings > Display > Graphics.");
		return false;
	}
	if (g_vr.deviceFeatureLevel < g_vr.minFeatureLevel) {
		Log("ERROR: GPU feature level 0x%x is below the runtime's minimum 0x%x", (unsigned)g_vr.deviceFeatureLevel,
			(unsigned)g_vr.minFeatureLevel);
		return false;
	}
	return true;
}

// Session
void DestroySwap(Swapchain& swap) {
	for (uint32_t i = 0; i < kMaxImages; i++) SafeRelease(swap.rtv[i]);
	if (swap.handle) g_vr.xrDestroySwapchain(swap.handle);
	swap.handle = XR_NULL_HANDLE;
	swap.count = swap.w = swap.h = 0;
}

void DestroyPanel(Panel& panel) {
	DestroySwap(panel.swap);
	SafeRelease(panel.rtv);
	SafeRelease(panel.tex);
	panel.shown = false;
}

void DestroyAllSwaps() {
	DestroySwap(g_vr.screenSwap);
	DestroySwap(g_vr.screenEyeSwap[0]);
	DestroySwap(g_vr.screenEyeSwap[1]);
	DestroySwap(g_vr.eyeSwap[0]);
	DestroySwap(g_vr.eyeSwap[1]);
	DestroySwap(g_vr.hudSwap);
	DestroyPanel(g_vr.menuPanel);
	DestroyPanel(g_vr.overlayPanel);
}

void DestroySession() {
	VrInputDetach();
	DestroyAllSwaps();
	{
		ExclusiveLock guard(&g_stereo.lock);
		g_stereo.valid = false;
	}
	if (g_vr.localSpace) g_vr.xrDestroySpace(g_vr.localSpace);
	if (g_vr.viewSpace) g_vr.xrDestroySpace(g_vr.viewSpace);
	if (g_vr.session) g_vr.xrDestroySession(g_vr.session);
	g_vr.localSpace = g_vr.viewSpace = XR_NULL_HANDLE;
	g_vr.session = XR_NULL_HANDLE;
	g_vr.running = false;
	g_vr.sessionLost = false;
	g_vr.screenPlaced = false;
	g_vr.state = XR_SESSION_STATE_UNKNOWN;
}

void DestroyInstance() {
	DestroySession();
	VrInputShutdown();
	if (g_vr.instance) g_vr.xrDestroyInstance(g_vr.instance);
	g_vr.instance = XR_NULL_HANDLE;
	g_vr.system = XR_NULL_SYSTEM_ID;
	g_vr.instanceLost = false;
}

bool InitSession() {
	XrGraphicsBindingD3D11KHR binding = {};
	binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
	binding.device = g_vr.device;
	XrSessionCreateInfo createInfo = {};
	createInfo.type = XR_TYPE_SESSION_CREATE_INFO;
	createInfo.next = &binding;
	createInfo.systemId = g_vr.system;
	if (!XR_CHECK(g_vr.xrCreateSession(g_vr.instance, &createInfo, &g_vr.session))) {
		g_vr.session = XR_NULL_HANDLE;
		return false;
	}
	XrReferenceSpaceCreateInfo spaceInfo = {};
	spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	spaceInfo.poseInReferenceSpace.orientation.w = 1;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!XR_CHECK(g_vr.xrCreateReferenceSpace(g_vr.session, &spaceInfo, &g_vr.localSpace))) return false;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!XR_CHECK(g_vr.xrCreateReferenceSpace(g_vr.session, &spaceInfo, &g_vr.viewSpace))) return false;
	VrInputAttach(g_vr.session);

	uint32_t count = 0;
	XrEnvironmentBlendMode modes[8];
	g_vr.blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	if (XR_SUCCEEDED(g_vr.xrEnumerateEnvironmentBlendModes(g_vr.instance, g_vr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
														   8, &count, modes)) &&
		count > 0)
		g_vr.blendMode = modes[0];

	// Pick a swapchain format: prefer sRGB so the runtime displays the game's colours exactly.
	int64_t formats[64];
	count = 0;
	if (!XR_CHECK(g_vr.xrEnumerateSwapchainFormats(g_vr.session, 64, &count, formats))) return false;
	const int64_t preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
								 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
	g_vr.swapFormat = 0;
	for (int64_t format : preferred) {
		for (uint32_t i = 0; i < count && !g_vr.swapFormat; i++)
			if (formats[i] == format) g_vr.swapFormat = format;
		if (g_vr.swapFormat) break;
	}
	if (!g_vr.swapFormat) {
		Log("ERROR: runtime offers no usable 8-bit swapchain format (%u formats)", count);
		return false;
	}
	Log("OpenXR session created (blend mode %d, swapchain format %d)", (int)g_vr.blendMode, (int)g_vr.swapFormat);
	g_vr.errorCount = 0;
	return true;
}
}  // namespace

bool EnsureSwap(Swapchain& swap, uint32_t width, uint32_t height, const char* what, bool copyTarget) {
	// Respect runtime limits, keep aspect ratio
	uint32_t maxW = g_vr.maxSwapchainW ? g_vr.maxSwapchainW : 4096, maxH = g_vr.maxSwapchainH ? g_vr.maxSwapchainH : 4096;
	if (width > maxW) {
		height = (uint32_t)((uint64_t)height * maxW / width);
		width = maxW;
	}
	if (height > maxH) {
		width = (uint32_t)((uint64_t)width * maxH / height);
		height = maxH;
	}
	if (swap.handle && swap.w == width && swap.h == height) return true;
	if ((int32_t)(GetTickCount() - swap.retryAt) < 0) return false;
	swap.retryAt = GetTickCount() + 2000;  // after a failure, retry at most every 2 s
	DestroySwap(swap);

	XrSwapchainCreateInfo createInfo = {};
	createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
							(copyTarget ? XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT : 0);
	createInfo.format = g_vr.swapFormat;
	createInfo.sampleCount = 1;
	createInfo.width = width;
	createInfo.height = height;
	createInfo.faceCount = 1;
	createInfo.arraySize = 1;
	createInfo.mipCount = 1;
	if (!XR_CHECK(g_vr.xrCreateSwapchain(g_vr.session, &createInfo, &swap.handle))) {
		swap.handle = XR_NULL_HANDLE;
		return false;
	}
	XrSwapchainImageD3D11KHR images[kMaxImages];
	for (uint32_t i = 0; i < kMaxImages; i++) {
		images[i] = {};
		images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
	}
	uint32_t imageCount = 0;
	if (!XR_CHECK(g_vr.xrEnumerateSwapchainImages(swap.handle, kMaxImages, &imageCount, (XrSwapchainImageBaseHeader*)images))) {
		DestroySwap(swap);
		return false;
	}
	for (uint32_t i = 0; i < imageCount; i++) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = (DXGI_FORMAT)g_vr.swapFormat;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		HRESULT hr = g_vr.device->CreateRenderTargetView(images[i].texture, &rtvDesc, &swap.rtv[i]);
		if (FAILED(hr)) {
			Log("ERROR: CreateRenderTargetView on swapchain image %u failed 0x%08x", i, (unsigned)hr);
			DestroySwap(swap);
			return false;
		}
	}
	swap.count = imageCount;
	swap.w = width;
	swap.h = height;
	swap.retryAt = GetTickCount();  // the 2 s wait is only for after a failure
	Log("%s swapchain %ux%u, %u images", what, width, height, imageCount);
	return true;
}

bool RenderInto(Swapchain& swap, ID3D11ShaderResourceView* srv, ID3D11ShaderResourceView* scene, const WarpParams* warp, bool asScreen) {
	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = {};
	acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
	if (!XR_CHECK_FRAME(g_vr.xrAcquireSwapchainImage(swap.handle, &acquireInfo, &imageIndex))) return false;
	XrSwapchainImageWaitInfo waitInfo = {};
	waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
	waitInfo.timeout = XR_INFINITE_DURATION;
	bool waited = XR_CHECK_FRAME(g_vr.xrWaitSwapchainImage(swap.handle, &waitInfo));
	if (!waited) return false;  // spec: an image may only be released after a successful wait
	if (imageIndex < swap.count) Blit(swap.rtv[imageIndex], swap.w, swap.h, srv, scene, IsSrgb(g_vr.swapFormat), warp, asScreen);
	XrSwapchainImageReleaseInfo releaseInfo = {};
	releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
	XR_CHECK_FRAME(g_vr.xrReleaseSwapchainImage(swap.handle, &releaseInfo));
	return true;
}

bool RenderScreenInto(Swapchain& swap, ID3D11ShaderResourceView* srv, const ScreenParams& screen) {
	return RenderInto(swap, srv, nullptr, (const WarpParams*)&screen, true);
}

namespace {
// Events & frame loop
const char* StateName(XrSessionState state) {
	switch (state) {
		case XR_SESSION_STATE_IDLE: return "IDLE";
		case XR_SESSION_STATE_READY: return "READY";
		case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
		case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
		case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
		case XR_SESSION_STATE_STOPPING: return "STOPPING";
		case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
		case XR_SESSION_STATE_EXITING: return "EXITING";
		default: return "UNKNOWN";
	}
}

void PollEvents() {
	XrEventDataBuffer event;
	while (true) {
		// the runtime overwrites the type with the event's own: it must be reset before every call
		event = {};
		event.type = XR_TYPE_EVENT_DATA_BUFFER;
		if (g_vr.xrPollEvent(g_vr.instance, &event) != XR_SUCCESS) break;
		switch (event.type) {
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				const XrEventDataSessionStateChanged* changed = (const XrEventDataSessionStateChanged*)&event;
				g_vr.state = changed->state;
				Log("session state -> %s", StateName(changed->state));
				if (changed->state == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo beginInfo = {};
					beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
					beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if (XR_CHECK(g_vr.xrBeginSession(g_vr.session, &beginInfo))) g_vr.running = true;
				} else if (changed->state == XR_SESSION_STATE_STOPPING) {
					XR_CHECK(g_vr.xrEndSession(g_vr.session));
					g_vr.running = false;
				} else if (changed->state == XR_SESSION_STATE_EXITING) {
					Log("the VR runtime closed our session; restart the game to re-enter VR");
					g_vr.exitRequested = true;
					g_vr.sessionLost = true;
				} else if (changed->state == XR_SESSION_STATE_LOSS_PENDING) {
					g_vr.sessionLost = true;
				}
				break;
			}
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				Log("OpenXR instance loss pending (runtime shutting down?)");
				g_vr.instanceLost = true;
				break;
			case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
				const XrEventDataReferenceSpaceChangePending* pending = (const XrEventDataReferenceSpaceChangePending*)&event;
				if (pending->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
					Log("runtime recentered the view; re-placing the screen");
					g_vr.recenterRequested = true;
				}
				break;
			}
			case XR_TYPE_EVENT_DATA_EVENTS_LOST:
				Log("OpenXR: events lost");
				VrInputProfileChanged();  // one of them may have been a controller change
				break;
			case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
				VrInputProfileChanged();
				break;
			default:
				break;
		}
	}
}

bool TimeReached(DWORD at) { return (int32_t)(GetTickCount() - at) >= 0; }

DWORD WINAPI XrThreadMain(void*) {
	g_xrThreadId = GetCurrentThreadId();
	InstallCrashHandler();  // in case the game replaced it during startup
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	g_vr.statStartMs = NowMs();
	Log("VR thread started");
	g_stereo.enabled = g_cfg.stereo != 0;
	Log("stereo is %s at start (toggle key 0x%x)", g_stereo.enabled ? "ON" : "OFF", (unsigned)g_cfg.keyStereo);

	if (!InitDevice()) {
		Log("VR disabled for this session (see errors above). The game keeps running normally.");
		return 0;
	}

	// Every wait in this loop is short so the capture texture is always served promptly.
	DWORD instanceRetryAt = 0, systemRetryAt = 0, sessionRetryAt = 0, instanceRetryDelay = 10000;
	bool xrDisabled = false;
	while (true) {
		ServiceShare();
		if (xrDisabled) {
			Sleep(50);
			continue;
		}
		if (!g_vr.instance) {
			if (!TimeReached(instanceRetryAt)) {
				if (KeyPressed(g_cfg.keyRecenter) && GameIsForeground()) {
					Log("retrying OpenXR now (recenter key)");
					instanceRetryAt = GetTickCount();
				}
				Sleep(50);
				continue;
			}
			if (!InitInstance()) {
				if (g_vr.instance) DestroyInstance();
				// Back off (10 s, 20 s, 40 s ... up to 5 min): each attempt may (re)start the VR
				// runtime, which is unwelcome when playing without a headset. F8 retries at once.
				DWORD wait = instanceRetryDelay;
				instanceRetryDelay = instanceRetryDelay >= 150000 ? 300000 : instanceRetryDelay * 2;
				Log("OpenXR not available - retrying in %u s (or press the recenter key to retry now)",
					(unsigned)(wait / 1000));
				instanceRetryAt = GetTickCount() + wait;
				continue;
			}
			instanceRetryDelay = 10000;
		}
		if (g_vr.system == XR_NULL_SYSTEM_ID) {
			if (!TimeReached(systemRetryAt)) {
				Sleep(50);
				continue;
			}
			if (!InitSystem()) {
				g_vr.system = XR_NULL_SYSTEM_ID;
				systemRetryAt = GetTickCount() + 2000;
				continue;
			}
			if (!CheckHeadsetGpu()) {
				xrDisabled = true;
				Log("VR disabled for this session. The game keeps running normally.");
				continue;
			}
		}
		if (!g_vr.session) {
			if (g_vr.exitRequested || !TimeReached(sessionRetryAt)) {
				Sleep(50);
				continue;
			}
			if (!InitSession()) {
				DestroySession();
				if (g_vr.instanceLost) DestroyInstance();
				sessionRetryAt = GetTickCount() + 3000;
				continue;
			}
		}

		PollEvents();
		if (g_vr.instanceLost) {
			DestroyInstance();
			instanceRetryAt = GetTickCount() + 3000;
			continue;
		}
		if (g_vr.sessionLost) {
			DestroySession();
			// spec: after a lost session the system may have changed (headset unplugged, another one):
			// get it (and its graphics requirements) again before creating a new session
			g_vr.system = XR_NULL_SYSTEM_ID;
			sessionRetryAt = GetTickCount() + 1000;
			systemRetryAt = GetTickCount() + 1000;
			continue;
		}
		if (!g_vr.running) {
			Sleep(10);
			continue;
		}
		Frame();
	}
}
}  // namespace

void XrStartThread() {
	static std::atomic<bool> started = false;
	if (started.exchange(true)) return;
	HANDLE thread = CreateThread(nullptr, 0, XrThreadMain, nullptr, 0, nullptr);
	if (thread) CloseHandle(thread);
	else Log("ERROR: could not create VR thread (%u)", (unsigned)GetLastError());
}
