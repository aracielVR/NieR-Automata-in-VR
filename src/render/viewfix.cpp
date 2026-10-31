// viewfix.cpp - keeps NieR's CPU-computed *view-space* data consistent with the eye camera.
#include "render/viewfix.h"
#include "core/com.h"
#include "core/config.h"
#include "core/log.h"
#include "core/pure.h"
#include "render/camera.h"
#include "render/replay.h"
#include "ui/menu.h"
#include <stdio.h>
#include <string.h>
#include "render/d3d_hooks.h"
#include <d3dcompiler.h>

namespace {
constexpr char kCSLights[] = R"(
cbuffer P : register(b0) { row_major float4x4 T; row_major float4x4 Tinv; uint first; uint count; uint2 pad; };
struct L { float4 a, b, c; };
StructuredBuffer<L> inL : register(t0);
RWStructuredBuffer<L> outL : register(u0);
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (id.x >= count) return;
	L l = inL[id.x];
	l.b.xyz = mul(T, float4(l.b.xyz, 1)).xyz;   // view-space position (radius in a.w unchanged)
	outL[first + id.x] = l;
}
)";
constexpr char kCSProbes[] = R"(
cbuffer P : register(b0) {
	row_major float4x4 T; row_major float4x4 Tinv; uint first; uint count; uint2 pad;
	uint4 on0; uint4 on1;   // GI volumes: bit per slot that stays on (others are marked empty)
};
struct C { float4 a, b, c, d, e, f; };
StructuredBuffer<C> inC : register(t0);
RWStructuredBuffer<C> outC : register(u0);
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (id.x >= count) return;
	C p = inC[id.x];
	uint w = id.x >> 5;
	uint bits = w < 4 ? on0[w] : on1[w - 4];
	if (((bits >> (id.x & 31)) & 1) == 0) p.a.w = 0;  // the game's pass skips slots whose a.w is 0
	p.b = mul(p.b, Tinv);  // rows of the view -> box matrix, now taking eye view-space positions
	p.c = mul(p.c, Tinv);
	p.d = mul(p.d, Tinv);
	outC[first + id.x] = p;
}
)";

struct Fix {
	ID3D11ComputeShader* cs;
	ID3D11ShaderResourceView* lenSrv[257];  // probes: views of buf ending after slot n-1 (made on demand)
	ID3D11Buffer* buf;
	ID3D11ShaderResourceView* srv;
	ID3D11UnorderedAccessView* uav;
	D3D11_BUFFER_DESC desc;
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
	int32_t slot;  // the game's SRV slot for this data
	UINT stride;
	const char* name;
	bool logged, failed;
};
Fix s_fixes[2] = {{nullptr, {}, nullptr, nullptr, nullptr, {}, {}, 5, 48, "point lights", false, false},
				  {nullptr, {}, nullptr, nullptr, nullptr, {}, {}, 1, 96, "reflection probes", false, false}};

constexpr int32_t kGiRing = 4;
struct GiCopy {
	ID3D11Buffer* buf;
	bool pending;
	UINT first, count;
};
GiCopy s_giRing[kGiRing];
UINT s_giStagingSize;
int32_t s_giNext;
pure::GiPick s_giPick;  // the latest decision
bool s_giHave;          // s_giPick is valid
DWORD s_giLogTick, s_giInfoTick;
int32_t s_giUsedMin = 9999, s_giUsedMax = -1, s_giHighMax = -1, s_giLenMin = 9999, s_giLenMax = -1, s_giSlots;
float s_giEntries[256 * 24];

ID3D11Buffer* s_fixConstants;
bool s_shadersReady, s_shadersFailed;
ID3D11ShaderResourceView* s_restoreSrv;
int32_t s_restoreSlot = -1;

bool CreateShaders(ID3D11Device* device) {
	if (s_shadersReady || s_shadersFailed) return s_shadersReady;
	HMODULE compilerDll = LoadLibraryW(L"d3dcompiler_47.dll");
	pD3DCompile compile = compilerDll ? (pD3DCompile)(void*)GetProcAddress(compilerDll, "D3DCompile") : nullptr;
	const char* entry[2] = {"Lights", "Probes"};
	const char* source[2] = {kCSLights, kCSProbes};
	for (int i = 0; i < 2 && compile; i++) {
		ID3DBlob *blob = nullptr, *errors = nullptr;
		HRESULT hr = compile(source[i], lstrlenA(source[i]), entry[i], nullptr, nullptr, "main", "cs_5_0",
							 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
		if (SUCCEEDED(hr)) hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_fixes[i].cs);
		if (FAILED(hr))
			Log("ERROR: view fix shader %s: 0x%08x %s", entry[i], (unsigned)hr,
				errors ? (const char*)errors->GetBufferPointer() : "");
		if (blob) blob->Release();
		if (errors) errors->Release();
	}
	D3D11_BUFFER_DESC desc = {};
	desc.ByteWidth = 176;  // = sizeof the parameter struct below
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(device->CreateBuffer(&desc, nullptr, &s_fixConstants))) s_fixConstants = nullptr;
	s_shadersReady = s_fixConstants && s_fixes[0].cs && s_fixes[1].cs;
	s_shadersFailed = !s_shadersReady;
	if (s_shadersFailed) Log("view fix: unavailable (shader compiler or resources missing); lighting may show tiles");
	return s_shadersReady;
}

// Our copy of the game's buffer behind `src`: same size/stride, plus UAV, and an SRV like the game's.
bool EnsureCopy(ID3D11Device* device, Fix& fix, ID3D11ShaderResourceView* src, UINT* first, UINT* count) {
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
	src->GetDesc(&srvDesc);
	if (srvDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER && srvDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX) return false;
	ComPtr<ID3D11Resource> res;
	src->GetResource(&res);
	if (!res) return false;
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	D3D11_BUFFER_DESC desc = {};
	if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) ((ID3D11Buffer*)res.Get())->GetDesc(&desc);
	if (dim != D3D11_RESOURCE_DIMENSION_BUFFER || desc.StructureByteStride != fix.stride ||
		!(desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED))
		return false;
	*first = srvDesc.Buffer.FirstElement;
	*count = srvDesc.Buffer.NumElements;
	if (fix.buf && fix.desc.ByteWidth == desc.ByteWidth && memcmp(&fix.srvDesc, &srvDesc, sizeof(srvDesc)) == 0) return true;
	for (auto& view : fix.lenSrv) SafeRelease(view);
	SafeRelease(fix.uav);
	SafeRelease(fix.srv);
	SafeRelease(fix.buf);
	D3D11_BUFFER_DESC copyDesc = {};
	copyDesc.ByteWidth = desc.ByteWidth;
	copyDesc.Usage = D3D11_USAGE_DEFAULT;
	copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	copyDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	copyDesc.StructureByteStride = fix.stride;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.NumElements = desc.ByteWidth / fix.stride;
	bool ok = SUCCEEDED(device->CreateBuffer(&copyDesc, nullptr, &fix.buf)) &&
			  SUCCEEDED(device->CreateShaderResourceView(fix.buf, &srvDesc, &fix.srv)) &&
			  SUCCEEDED(device->CreateUnorderedAccessView(fix.buf, &uavDesc, &fix.uav));
	if (!ok) {
		SafeRelease(fix.uav);
		SafeRelease(fix.srv);
		SafeRelease(fix.buf);
		return false;
	}
	fix.desc = desc;
	fix.srvDesc = srvDesc;
	return true;
}

// Each frame (not in the synchronized replay): copy the game's GI volume buffer into the ring, and use
// the oldest copy the GPU has finished to decide which slots stay on.
void GiReadback(ID3D11DeviceContext* c, ID3D11Device* device, ID3D11ShaderResourceView* src, UINT first, UINT count) {
	// newest finished copy
	for (int k = 0; k < kGiRing; k++) {
		GiCopy& copy = s_giRing[(s_giNext + k) % kGiRing];  // oldest first
		if (!copy.pending) continue;
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (c->Map(copy.buf, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK) break;
		UINT entries = copy.count > 256 ? 256 : copy.count;
		if ((copy.first + entries) * 96 <= s_giStagingSize) {
			memcpy(s_giEntries, (const uint8_t*)mapped.pData + copy.first * 96, entries * 96);
			int keep = g_cfg.giVolumes >= 128 ? 256 : g_cfg.giVolumes;
			s_giPick = pure::gi_pick(s_giEntries, (int)entries, keep, 8);
			s_giHave = true;
			s_giSlots = (int)copy.count;
			if (s_giPick.used < s_giUsedMin) s_giUsedMin = s_giPick.used;
			if (s_giPick.used > s_giUsedMax) s_giUsedMax = s_giPick.used;
			if (s_giPick.highest > s_giHighMax) s_giHighMax = s_giPick.highest;
			if (s_giPick.length < s_giLenMin) s_giLenMin = s_giPick.length;
			if (s_giPick.length > s_giLenMax) s_giLenMax = s_giPick.length;
		}
		c->Unmap(copy.buf, 0);
		copy.pending = false;
	}
	// this frame's copy
	ComPtr<ID3D11Resource> res;
	src->GetResource(&res);
	if (!res) return;
	D3D11_BUFFER_DESC desc;
	((ID3D11Buffer*)res.Get())->GetDesc(&desc);
	if (s_giStagingSize != desc.ByteWidth) {
		for (auto& copy : s_giRing) {
			SafeRelease(copy.buf);
			copy.pending = false;
		}
		s_giStagingSize = 0;
		D3D11_BUFFER_DESC stagingDesc = {};
		stagingDesc.ByteWidth = desc.ByteWidth;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		stagingDesc.MiscFlags = desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		stagingDesc.StructureByteStride = desc.StructureByteStride;
		bool ok = true;
		for (auto& copy : s_giRing) ok = ok && SUCCEEDED(device->CreateBuffer(&stagingDesc, nullptr, &copy.buf));
		if (!ok) {  // try a plain buffer
			for (auto& copy : s_giRing) SafeRelease(copy.buf);
			stagingDesc.MiscFlags = 0;
			stagingDesc.StructureByteStride = 0;
			ok = true;
			for (auto& copy : s_giRing) ok = ok && SUCCEEDED(device->CreateBuffer(&stagingDesc, nullptr, &copy.buf));
		}
		if (ok) s_giStagingSize = desc.ByteWidth;
		else
			for (auto& copy : s_giRing) SafeRelease(copy.buf);
	}
	GiCopy& copy = s_giRing[s_giNext];
	if (s_giStagingSize && !copy.pending) {
		c->CopyResource(copy.buf, res.Get());
		copy.pending = true;
		copy.first = first;
		copy.count = count;
		s_giNext = (s_giNext + 1) % kGiRing;
	}
}

void GiReport() {
	if (!s_giHave) return;
	DWORD now = GetTickCount();
	if (now - s_giInfoTick > 500) {
		s_giInfoTick = now;
		char info[160];
		if (s_giPick.dropping)
			snprintf(info, sizeof(info), "GI volumes: %d in use, %d nearest kept; the pass checks slots 0-%d of %d",
					 s_giPick.used, s_giPick.kept, s_giPick.length - 1, s_giSlots);
		else
			snprintf(info, sizeof(info), "GI volumes: %d in use (last slot %d), all kept; the pass checks slots 0-%d of %d",
					 s_giPick.used, s_giPick.highest, s_giPick.length - 1, s_giSlots);
		MenuSetInfo(INFO_VIEWFIX, info);
	}
	if (now - s_giLogTick < 30000 || s_giUsedMax < 0) return;
	s_giLogTick = now;
	Log("GI volumes (30 s): %d-%d of %d slots in use, last used slot up to %d; the game's pass checked %d-%d slots "
		"(GIVolumes=%d: %s)",
		s_giUsedMin, s_giUsedMax, s_giSlots, s_giHighMax, s_giLenMin, s_giLenMax, g_cfg.giVolumes,
		g_cfg.giVolumes >= 128 ? "all kept, only the empty tail skipped" : "the nearest kept, the rest switched off");
	s_giUsedMin = 9999;
	s_giUsedMax = s_giHighMax = s_giLenMax = -1;
	s_giLenMin = 9999;
}
}  // namespace

// kind: 1 = light culling, 2 = probe selection (GI volumes). Called right before the game's dispatch.
void ViewFixBeforeDispatch(ID3D11DeviceContext* c, int kind) {
	s_restoreSlot = -1;
	if (kind < 1 || kind > 2) return;
	float T[16], Tinv[16];
	bool view = StereoGetViewFix(T, Tinv);  // false in mono frames: the data already matches
	bool isGi = kind == 2;                  // GI volumes: always read back; slots switched off / loop cut once a pick exists
	if (!view && !isGi) return;
	if (!view) {
		for (int i = 0; i < 16; i++) T[i] = Tinv[i] = (i % 5 == 0) ? 1.f : 0.f;
	}
	ComPtr<ID3D11Device> device;
	c->GetDevice(&device);
	if (!device) return;
	Fix& fix = s_fixes[kind - 1];
	ComPtr<ID3D11ShaderResourceView> src;
	UINT first = 0, count = 0;
	bool ok = CreateShaders(device.Get());
	if (ok) {
		c->CSGetShaderResources(fix.slot, 1, &src);
		ok = src.Get() && EnsureCopy(device.Get(), fix, src.Get(), &first, &count) && count > 0;
	}
	if (!ok) {
		if (!fix.failed && s_shadersReady) {
			fix.failed = true;
			Log("view fix: %s buffer not in the expected format; left unchanged", fix.name);
		}
		return;
	}
	if (isGi && !g_replaying) GiReadback(c, device.Get(), src.Get(), first, count);  // the replay reuses this frame's pick
	if (isGi) GiReport();
	// GI volumes: the view the game's pass gets ends after the last slot left on
	ID3D11ShaderResourceView* use = fix.srv;
	uint32_t on[8];
	for (auto& word : on) word = 0xffffffffu;
	bool trim = isGi && s_giHave && count <= 256 && s_giSlots == (int)count;
	if (trim) {
		UINT len = (UINT)s_giPick.length;
		if (len > count) len = count;
		if (!fix.lenSrv[len]) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = fix.srvDesc;
			srvDesc.Buffer.NumElements = len;  // same place in the BUFFER and BUFFEREX views
			if (FAILED(device->CreateShaderResourceView(fix.buf, &srvDesc, &fix.lenSrv[len]))) fix.lenSrv[len] = nullptr;
		}
		if (fix.lenSrv[len]) {
			use = fix.lenSrv[len];
			memcpy(on, s_giPick.mask, sizeof(on));
		} else {
			trim = false;
		}
	}
	if (!view && !trim) {  // flat frame, nothing to change yet
		return;
	}
	struct {
		float T[16], Tinv[16];
		UINT first, count, pad[2];
		uint32_t on[8];
	} constants;
	static_assert(sizeof(constants) == 176, "matches s_fixConstants ByteWidth and the HLSL cbuffer");
	memcpy(constants.T, T, 64);
	memcpy(constants.Tinv, Tinv, 64);
	constants.first = first;
	constants.count = count;
	constants.pad[0] = constants.pad[1] = 0;
	memcpy(constants.on, on, sizeof(on));
	c->UpdateSubresource(s_fixConstants, 0, nullptr, &constants, 0, 0);

	// save the game's compute state we touch
	ComPtr<ID3D11ComputeShader> gameCs;
	ID3D11ClassInstance* classInstances[8] = {};
	UINT classInstanceCount = 8;
	c->CSGetShader(&gameCs, classInstances, &classInstanceCount);
	ComPtr<ID3D11ShaderResourceView> gameSrv0;
	c->CSGetShaderResources(0, 1, &gameSrv0);
	ComPtr<ID3D11UnorderedAccessView> gameUav0;
	c->CSGetUnorderedAccessViews(0, 1, &gameUav0);
	ComPtr<ID3D11Buffer> gameCb0;
	c->CSGetConstantBuffers(0, 1, &gameCb0);

	ID3D11ShaderResourceView* none = nullptr;
	c->CSSetShaderResources(0, 1, &none);  // the game's t0 may alias nothing of ours, but be safe
	c->CSSetShader(fix.cs, nullptr, 0);
	c->CSSetConstantBuffers(0, 1, &s_fixConstants);
	c->CSSetShaderResources(0, 1, src.GetAddressOf());
	UINT keepCounts = (UINT)-1;
	c->CSSetUnorderedAccessViews(0, 1, &fix.uav, &keepCounts);
	OriginalDispatch(c, (count + 63) / 64, 1, 1);
	ID3D11UnorderedAccessView* noUav = nullptr;
	c->CSSetUnorderedAccessViews(0, 1, &noUav, &keepCounts);

	// restore, and let the game's pass read our copy
	c->CSSetShader(gameCs.Get(), classInstances, classInstanceCount);
	c->CSSetConstantBuffers(0, 1, gameCb0.GetAddressOf());
	c->CSSetShaderResources(0, 1, gameSrv0.GetAddressOf());
	c->CSSetUnorderedAccessViews(0, 1, gameUav0.GetAddressOf(), &keepCounts);
	c->CSSetShaderResources(fix.slot, 1, &use);
	s_restoreSrv = src.Detach();  // keeps our reference until after the dispatch
	s_restoreSlot = fix.slot;
	for (UINT i = 0; i < classInstanceCount && i < 8; i++)
		if (classInstances[i]) classInstances[i]->Release();
	if (!fix.logged && view) {
		fix.logged = true;
		Log("view fix: %s moved into the eye's view space (%u entries) for the game's compute pass", fix.name, count);
	}
	static bool trimLogged;
	if (trim && !trimLogged) {
		trimLogged = true;
		Log("GI volumes: the game's GI pass now checks slots 0-%d of %u (%d in use, %d kept; GIVolumes=%d)",
			s_giPick.length - 1, count, s_giPick.used, s_giPick.kept, g_cfg.giVolumes);
	}
}

void ViewFixAfterDispatch(ID3D11DeviceContext* c) {
	if (s_restoreSlot < 0) return;
	c->CSSetShaderResources((UINT)s_restoreSlot, 1, &s_restoreSrv);
	if (s_restoreSrv) s_restoreSrv->Release();
	s_restoreSrv = nullptr;
	s_restoreSlot = -1;
}
