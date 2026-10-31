#include "render/dlss.h"
#include "core/locks.h"
#include "core/download.h"
#include "core/log.h"
#include "core/main.h"
#include <atomic>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {
typedef int NgxResult;  // NVSDK_NGX_Result
bool NgxOk(NgxResult result) { return (result & 0xFFF00000) != 0xBAD00000; }
struct NgxParams;
struct NgxFeature;

typedef NgxResult (*PFN_Init_Ext)(uint64_t appId, const wchar_t* dataPath, ID3D11Device*, int sdkVersion,
								  const NgxParams*);
typedef NgxResult (*PFN_Params)(NgxParams**);
typedef NgxResult (*PFN_DestroyParams)(NgxParams*);
typedef NgxResult (*PFN_Create)(ID3D11DeviceContext*, int feature, NgxParams*, NgxFeature**);
typedef NgxResult (*PFN_Eval)(ID3D11DeviceContext*, const NgxFeature*, const NgxParams*, void* progress);
typedef NgxResult (*PFN_Release)(NgxFeature*);
typedef NgxResult (*PFN_Optimal)(NgxParams*);

constexpr int32_t kSdkVersion = 0x15;  // NVSDK_NGX_Version_API (1.5)
constexpr int32_t kFeatureSuperSampling = 1;
constexpr uint64_t kAppId = 0x4E69657252ull;  // any id: the driver accepts custom ones

PFN_Init_Ext s_ngxInit;
PFN_Params s_ngxGetCapabilities, s_ngxAllocParams;
PFN_DestroyParams s_ngxDestroyParams;
PFN_Create s_ngxCreateFeature;
PFN_Eval s_ngxEvaluate;
PFN_Release s_ngxReleaseFeature;

// The DLSS file: pinned to a commit of github.com/NVIDIA/DLSS, checked by SHA256 (DLSS 310.9.16).
#define DLSS_COMMIT L"9981c6350dc58c0ce21f024fd65e31e851c31689"
constexpr wchar_t kDllUrl[] = L"https://github.com/NVIDIA/DLSS/raw/" DLSS_COMMIT L"/lib/Windows_x86_64/rel/nvngx_dlss.dll";
constexpr char kDllSha[] = "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e";
constexpr wchar_t kLicUrl[] = L"https://raw.githubusercontent.com/NVIDIA/DLSS/" DLSS_COMMIT L"/LICENSE.txt";
constexpr char kLicSha[] = "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0";
constexpr int64_t kDllSize = 58956912;
std::atomic<int32_t> s_fileGen;  // bumps when a new nvngx_dlss.dll arrives
int32_t s_triedGen = -1;     // s_fileGen when DLSS last tried to start
std::atomic<bool> s_downloadRunning, s_downloadCancel;
std::atomic<int64_t> s_downloadGot, s_downloadTotal;
SRWLOCK s_fileLock = SRWLOCK_INIT;
char s_downloadError[96];  // why the last download failed
char s_fileVersion[32];    // version of the file in the game's folder (VR thread)
int32_t s_fileVersionGen = -1;
std::atomic<bool> s_hashRunning;
std::atomic<int32_t> s_hashedGen = -1;  // s_fileGen that s_fileMatchesSha is for
std::atomic<bool> s_fileMatchesSha;  // the file in the game's folder is the one kDllSha names

// vtable slots (see the top of the file)
enum { SLOT_PTR = 0,
	   SLOT_D3D11_RESOURCE = 2,
	   SLOT_INT = 3,
	   SLOT_UINT = 4,
	   SLOT_FLOAT = 6,
	   SLOT_GET = 8 };
template <class F>
F Method(NgxParams* params, int slot) { return (F)(*(void***)params)[slot]; }
void SetUint(NgxParams* params, const char* name, uint32_t value) { Method<void (*)(NgxParams*, const char*, uint32_t)>(params, SLOT_UINT)(params, name, value); }
void SetInt(NgxParams* params, const char* name, int value) { Method<void (*)(NgxParams*, const char*, int)>(params, SLOT_INT)(params, name, value); }
void SetFloat(NgxParams* params, const char* name, float value) { Method<void (*)(NgxParams*, const char*, float)>(params, SLOT_FLOAT)(params, name, value); }
void SetResource(NgxParams* params, const char* name, ID3D11Resource* value) { Method<void (*)(NgxParams*, const char*, ID3D11Resource*)>(params, SLOT_D3D11_RESOURCE)(params, name, value); }
NgxResult GetInt(NgxParams* params, const char* name, int* value) { return Method<NgxResult (*)(NgxParams*, const char*, int*)>(params, SLOT_GET + SLOT_INT)(params, name, value); }

bool s_tried, s_ready;
char s_status[128] = "DLSS: not started";

struct Feature {
	NgxFeature* handle;
	NgxParams* params;
	UINT renderW, renderH, outputW, outputH;
	int32_t quality, model;
	bool inverted;
} s_features[2];

void SetStatus(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void SetStatus(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(s_status, sizeof(s_status), fmt, args);
	va_end(args);
	Log("%s", s_status);
}

void ReleaseFeature(Feature& feature) {
	if (feature.handle) s_ngxReleaseFeature(feature.handle);
	if (feature.params) s_ngxDestroyParams(feature.params);
	feature = {};
}

// Our quality index -> NVSDK_NGX_PerfQuality_Value
int PerfQuality(int quality) {
	static const int values[] = {5 /*DLAA*/, 2 /*MaxQuality*/, 1 /*Balanced*/, 0 /*MaxPerf*/, 3 /*UltraPerformance*/};
	return values[quality < 0 ? 0 : (quality > 4 ? 4 : quality)];
}

// A file in the game's folder (next to its exe: where NGX looks for nvngx_dlss.dll).
void GamePath(const wchar_t* name, wchar_t out[MAX_PATH]) {
	GetModuleFileNameW(nullptr, out, MAX_PATH);
	wchar_t* slash = wcsrchr(out, L'\\');
	snwprintf(slash ? slash + 1 : out, MAX_PATH - (slash ? slash + 1 - out : 0), L"%ls", name);
}

bool DllPresent() {
	wchar_t path[MAX_PATH];
	GamePath(L"nvngx_dlss.dll", path);
	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

void SetDownloadError(const char* text) {
	ExclusiveLock guard(&s_fileLock);
	snprintf(s_downloadError, sizeof(s_downloadError), "%s", text);
}

DWORD WINAPI DownloadThread(void*) {
	wchar_t dll[MAX_PATH], lic[MAX_PATH], old[MAX_PATH];
	GamePath(L"nvngx_dlss.dll", dll);
	GamePath(L"nvngx_dlss_LICENSE.txt", lic);
	GamePath(L"nvngx_dlss.dll.old", old);
	// Redownload: Windows can't replace a loaded DLL, but it can rename it, and a running DLSS keeps
	// using the renamed file.
	DeleteFileW(old);
	bool movedAside = MoveFileExW(dll, old, MOVEFILE_REPLACE_EXISTING);
	Log("DLSS: downloading nvngx_dlss.dll from github.com/NVIDIA/DLSS");
	char err[96] = "";
	std::atomic<int64_t> licGot = 0, licTotal = 0;
	bool ok = HttpDownload(kLicUrl, lic, kLicSha, &licGot, &licTotal, &s_downloadCancel, err, sizeof(err)) &&
			  HttpDownload(kDllUrl, dll, kDllSha, &s_downloadGot, &s_downloadTotal, &s_downloadCancel, err, sizeof(err));
	SetDownloadError(ok || s_downloadCancel ? "" : err);  // a cancel isn't an error
	Log("DLSS: download %s%s", ok ? "finished (checksum OK)" : "failed: ", ok ? "" : err);
	if (ok) {
		s_fileMatchesSha = true;  // HttpDownload checked it
		s_hashedGen = ++s_fileGen;
	} else if (movedAside) {
		MoveFileExW(old, dll, 0);  // keep the file that was there
	} else if (!DllPresent()) {
		DeleteFileW(lic);
	}
	s_downloadRunning = false;
	return 0;
}

// Compares the installed file with kDllSha: after kDllSha is moved to a newer DLSS, the menu offers an update.
DWORD WINAPI HashThread(void* fileGen) {
	wchar_t dll[MAX_PATH];
	GamePath(L"nvngx_dlss.dll", dll);
	char hex[65];
	bool matches = FileSha256(dll, hex) && _stricmp(hex, kDllSha) == 0;
	if (!matches) Log("DLSS: nvngx_dlss.dll isn't the version this mod expects: update available");
	s_fileMatchesSha = matches;
	s_hashedGen = (int32_t)(intptr_t)fileGen;
	s_hashRunning = false;
	return 0;
}

// File version of a loaded DLL from its version resource ("310.9.16"), without version.dll.
void ModuleVersion(HMODULE module, char* out, size_t cap) {
	snprintf(out, cap, "?");
	HRSRC resource = module ? FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16) /*RT_VERSION*/) : nullptr;
	HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
	const uint8_t* data = loaded ? (const uint8_t*)LockResource(loaded) : nullptr;
	DWORD size = resource ? SizeofResource(module, resource) : 0;
	for (DWORD i = 0; data && i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4) {
		const VS_FIXEDFILEINFO* info = (const VS_FIXEDFILEINFO*)(data + i);
		if (info->dwSignature != 0xFEEF04BD) continue;
		snprintf(out, cap, "%u.%u.%u", (unsigned)HIWORD(info->dwFileVersionMS), (unsigned)LOWORD(info->dwFileVersionMS),
				 (unsigned)HIWORD(info->dwFileVersionLS));
		return;
	}
}
}  // namespace

bool DlssInit(ID3D11Device* device) {
	if (s_tried && !s_ready && s_triedGen != s_fileGen) s_tried = false;  // the file arrived: try again
	if (s_tried) return s_ready;
	s_tried = true;
	s_triedGen = s_fileGen;
	if (!DllPresent()) {
		SetStatus("DLSS: not downloaded yet (menu: Rendering, DLSS file)");
		return false;
	}
	wchar_t dir[MAX_PATH] = {};
	DWORD size = sizeof(dir);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath",
					 RRF_RT_REG_SZ, nullptr, dir, &size) != ERROR_SUCCESS) {
		SetStatus("DLSS: no NVIDIA driver with DLSS support found");
		return false;
	}
	wchar_t path[MAX_PATH];
	snwprintf(path, MAX_PATH, L"%ls\\_nvngx.dll", dir);
	HMODULE ngxLoader = LoadLibraryW(path);
	if (!ngxLoader) {
		SetStatus("DLSS: could not load the driver's _nvngx.dll (error %u)", (unsigned)GetLastError());
		return false;
	}
#define LOAD(var, name) var = (decltype(var))(void*)GetProcAddress(ngxLoader, "NVSDK_NGX_D3D11_" name)
	LOAD(s_ngxInit, "Init_Ext");
	LOAD(s_ngxGetCapabilities, "GetCapabilityParameters");
	LOAD(s_ngxAllocParams, "AllocateParameters");
	LOAD(s_ngxDestroyParams, "DestroyParameters");
	LOAD(s_ngxCreateFeature, "CreateFeature");
	LOAD(s_ngxEvaluate, "EvaluateFeature");
	LOAD(s_ngxReleaseFeature, "ReleaseFeature");
#undef LOAD
	if (!s_ngxInit || !s_ngxGetCapabilities || !s_ngxAllocParams || !s_ngxDestroyParams || !s_ngxCreateFeature || !s_ngxEvaluate || !s_ngxReleaseFeature) {
		SetStatus("DLSS: the driver's NGX loader lacks D3D11 functions (old driver?)");
		return false;
	}
	NgxResult result = s_ngxInit(kAppId, g_modDir, device, kSdkVersion, nullptr);
	if (!NgxOk(result)) {
		SetStatus("DLSS: NGX did not start (0x%08x)", (unsigned)result);
		return false;
	}
	NgxParams* caps = nullptr;
	int avail = 0, needDriver = 0;
	if (NgxOk(s_ngxGetCapabilities(&caps)) && caps) {
		GetInt(caps, "SuperSampling.Available", &avail);
		GetInt(caps, "SuperSampling.NeedsUpdatedDriver", &needDriver);
		s_ngxDestroyParams(caps);
	}
	if (avail != 1) {  // TODO this is busted
		SetStatus("%s", needDriver ? "DLSS: needs a newer NVIDIA driver" : "DLSS: not supported by this graphics card");
		return false;
	}
	s_ready = true;
	char ver[32];
	ModuleVersion(GetModuleHandleW(L"nvngx_dlss.dll"), ver, sizeof(ver));  // loaded by the capability check
	if (ver[0] && ver[0] != '?') snprintf(s_status, sizeof(s_status), "DLSS %s", ver);
	else snprintf(s_status, sizeof(s_status), "DLSS is running");  // its file's version wasn't readable ("?" confused JJ)
	Log("DLSS: ready (nvngx_dlss.dll %s; NGX loader %s)", ver, Utf8(path).s);
	return true;
}

bool DlssPrepare(ID3D11DeviceContext* c, int eye, UINT renderW, UINT renderH, UINT outputW, UINT outputH, int quality, int model,
				 bool inverted) {
	if (!s_ready || eye < 0 || eye > 1) return false;
	Feature& feature = s_features[eye];
	if (feature.handle && feature.renderW == renderW && feature.renderH == renderH && feature.outputW == outputW && feature.outputH == outputH && feature.quality == quality && feature.model == model &&
		feature.inverted == inverted)
		return true;
	ReleaseFeature(feature);
	if (!NgxOk(s_ngxAllocParams(&feature.params)) || !feature.params) return false;
	const uint32_t preset = model ? 11 /*K, transformer*/ : 5 /*E, CNN*/;
	static const char* const hints[] = {"DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality",
										"DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance",
										"DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality"};
	for (const char* hint : hints) SetUint(feature.params, hint, preset);
	SetUint(feature.params, "CreationNodeMask", 1);
	SetUint(feature.params, "VisibilityNodeMask", 1);
	SetUint(feature.params, "Width", renderW);
	SetUint(feature.params, "Height", renderH);
	SetUint(feature.params, "OutWidth", outputW);
	SetUint(feature.params, "OutHeight", outputH);
	SetInt(feature.params, "PerfQualityValue", PerfQuality(quality));
	SetInt(feature.params, "RTXValue", 0);
	// motion vectors at render size; the game's depth convention
	SetInt(feature.params, "DLSS.Feature.Create.Flags", (1 << 1) /*MVLowRes*/ | (inverted ? 1 << 3 /*DepthInverted*/ : 0));
	SetInt(feature.params, "DLSS.Enable.Output.Subrects", 0);
	NgxResult result = s_ngxCreateFeature(c, kFeatureSuperSampling, feature.params, &feature.handle);
	if (!NgxOk(result) || !feature.handle) {
		Log("DLSS: could not create the %s eye's upscaler %ux%u -> %ux%u (0x%08x)", eye ? "right" : "left", renderW, renderH, outputW,
			outputH, (unsigned)result);
		ReleaseFeature(feature);
		return false;
	}
	feature.renderW = renderW, feature.renderH = renderH, feature.outputW = outputW, feature.outputH = outputH, feature.quality = quality, feature.model = model, feature.inverted = inverted;
	Log("DLSS: %s eye %ux%u -> %ux%u, quality %d, preset %s", eye ? "right" : "left", renderW, renderH, outputW, outputH, quality,
		model ? "K (transformer)" : "E (CNN)");
	return true;
}

bool DlssEvaluate(ID3D11DeviceContext* c, int eye, const DlssInputs& inputs) {
	if (!s_ready || eye < 0 || eye > 1 || !s_features[eye].handle) return false;
	Feature& feature = s_features[eye];
	NgxParams* params = feature.params;
	SetResource(params, "Color", inputs.color);
	SetResource(params, "Output", inputs.output);
	SetResource(params, "Depth", inputs.depth);
	SetResource(params, "MotionVectors", inputs.motion);
	SetFloat(params, "Jitter.Offset.X", inputs.jitterX);
	SetFloat(params, "Jitter.Offset.Y", inputs.jitterY);
	SetFloat(params, "Sharpness", 0);
	SetInt(params, "Reset", inputs.reset ? 1 : 0);
	SetFloat(params, "MV.Scale.X", 1);
	SetFloat(params, "MV.Scale.Y", 1);
	SetUint(params, "DLSS.Render.Subrect.Dimensions.Width", feature.renderW);
	SetUint(params, "DLSS.Render.Subrect.Dimensions.Height", feature.renderH);
	SetFloat(params, "DLSS.Pre.Exposure", 1);
	SetFloat(params, "DLSS.Exposure.Scale", 1);
	NgxResult result = s_ngxEvaluate(c, feature.handle, params, nullptr);
	if (!NgxOk(result)) {
		static std::atomic<int32_t> budget;
		if (LogBudget(budget, 5)) Log("DLSS: evaluate failed for the %s eye (0x%08x)", eye ? "right" : "left", (unsigned)result);
		return false;
	}
	return true;
}

void DlssReleaseFeatures() {
	if (!s_ready) return;
	for (auto& feature : s_features) ReleaseFeature(feature);
}

const char* DlssStatus() { return s_status; }

// ---- the menu's DLSS file row (VR thread) ----
void DlssFileGet(DlssFileState* out) {
	out->got = s_downloadGot;
	int64_t total = s_downloadTotal;
	out->total = total > 0 ? total : kDllSize;
	out->text[0] = 0;
	if (s_downloadRunning) {
		out->state = DF_DOWNLOADING;
		return;
	}
	static bool removedOldFile;
	if (!removedOldFile) {  // left by a redownload in an earlier session; same thread as DlssFileDownload
		removedOldFile = true;
		wchar_t old[MAX_PATH];
		GamePath(L"nvngx_dlss.dll.old", old);
		DeleteFileW(old);
	}
	if (DllPresent()) {
		int32_t fileGen = s_fileGen;
		if (s_hashedGen != fileGen && !s_hashRunning.exchange(true)) {
			HANDLE thread = CreateThread(nullptr, 0, HashThread, (void*)(INT_PTR)fileGen, 0, nullptr);
			if (thread)
				CloseHandle(thread);
			else
				s_hashRunning = false;
		}
		bool outdated = s_hashedGen == fileGen && !s_fileMatchesSha;
		out->state = outdated ? DF_UPDATE_AVAILABLE : DF_INSTALLED;
		if (s_fileVersionGen != s_fileGen) {  // read the version without running the DLL
			s_fileVersionGen = s_fileGen;
			wchar_t path[MAX_PATH];
			GamePath(L"nvngx_dlss.dll", path);
			HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
			ModuleVersion(module, s_fileVersion, sizeof(s_fileVersion));
			if (module) FreeLibrary(module);
		}
		snprintf(out->text, sizeof(out->text), "%s", s_fileVersion);
		return;
	}
	out->state = DF_MISSING;
	SharedLock guard(&s_fileLock);
	snprintf(out->text, sizeof(out->text), "%s", s_downloadError);  // why the last try failed
}

void DlssFileDownload() {
	if (s_downloadRunning.exchange(true)) return;
	s_downloadCancel = false;
	s_downloadGot = 0;
	s_downloadTotal = 0;
	SetDownloadError("");
	HANDLE thread = CreateThread(nullptr, 0, DownloadThread, nullptr, 0, nullptr);
	if (thread) CloseHandle(thread);
	else s_downloadRunning = false;
}

void DlssFileCancel() { s_downloadCancel = true; }
