// replay.cpp - rerender draw calls from the other eyes perspective
#include "render/replay.h"
#include "core/com.h"
#include "core/locks.h"
#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include "debug/frame_capture.h"
#include "debug/gpu_timing.h"
#include "render/camera.h"
#include "render/foveated.h"
#include "ui/hud_layer.h"
#include "render/viewfix.h"
#include "ui/menu.h"
#include "render/frame_handoff.h"
#include "render/game_effects.h"
#include <atomic>
#include <iterator>
#include <stdio.h>
#include <string.h>
#include "render/d3d_hooks.h"
#include <smmintrin.h>
#include <d3d11_1.h>

std::atomic<bool> g_replaying;           // playback in progress: every hook passes straight through
std::atomic<bool> g_recording;           // the recorder is taking calls
ID3D11DeviceContext* g_gameContext;  // the game's immediate context
std::atomic<int32_t> g_modCallsInProgress;  // mod-internal calls in progress (not recorded)

namespace {
enum Op : uint16_t {
	OP_SET_CB = 1,
	OP_SET_SRV,
	OP_SET_SAMPLERS,
	OP_SET_SHADER,
	OP_CS_UAV,
	OP_IA_LAYOUT,
	OP_IA_VB,
	OP_IA_IB,
	OP_IA_TOPOLOGY,
	OP_DRAW,
	OP_DRAW_AUTO,
	OP_DRAW_INDIRECT,
	OP_DISPATCH,
	OP_DISPATCH_INDIRECT,
	OP_OM_RT,
	OP_OM_RT_UAV,
	OP_OM_BLEND,
	OP_OM_DS,
	OP_RS_STATE,
	OP_RS_VP,
	OP_RS_SCISSOR,
	OP_SO_TARGETS,
	OP_COPY_REGION,
	OP_COPY,
	OP_UPDATE,
	OP_COPY_COUNT,
	OP_CLEAR_RTV,
	OP_CLEAR_UAV_UINT,
	OP_CLEAR_UAV_FLOAT,
	OP_CLEAR_DSV,
	OP_GENERATE_MIPS,
	OP_MIN_LOD,
	OP_RESOLVE,
	OP_MAP,
	OP_PREDICATION,
	OP_CLEAR_STATE,
	OP_DISCARD_RES,
	OP_DISCARD_VIEW,
	OP_NEW_GENERATION,
	OP_COUNT
};
const char* const kOpNames[OP_COUNT] = {"?", "SetConstantBuffers", "SetShaderResources", "SetSamplers", "SetShader",
										"CSSetUnorderedAccessViews", "IASetInputLayout", "IASetVertexBuffers", "IASetIndexBuffer", "IASetPrimitiveTopology",
										"Draw*", "DrawAuto", "Draw*Indirect", "Dispatch", "DispatchIndirect", "OMSetRenderTargets",
										"OMSetRenderTargetsAndUAVs", "OMSetBlendState", "OMSetDepthStencilState", "RSSetState", "RSSetViewports",
										"RSSetScissorRects", "SOSetTargets", "CopySubresourceRegion", "CopyResource", "UpdateSubresource",
										"CopyStructureCount", "ClearRenderTargetView", "ClearUAVUint", "ClearUAVFloat", "ClearDepthStencilView",
										"GenerateMips", "SetResourceMinLOD", "ResolveSubresource", "Map", "SetPredication", "ClearState",
										"DiscardResource", "DiscardView", "dynamic buffer refill"};
struct CmdHeader {
	uint16_t op;
	uint16_t arg;          // small argument (stage, kind, ...)
	uint32_t payloadSize;  // payload bytes after this header
};

constexpr size_t kArenaBytes = 192u << 20;  // per frame: commands + uploaded data
constexpr size_t kMaxRefs = 1 << 18;
constexpr UINT kMaxUploadBytes = 16u << 20;  // a single Map/Update larger than this cancels the frame

uint8_t* s_arena;
size_t s_arenaUsed;
IUnknown** s_refs;
size_t s_refCount;
SRWLOCK s_recordLock = SRWLOCK_INIT;
bool s_hooksReady;           // hooks installed, memory allocated
bool s_recordingValid;       // the recording can be played back
const char* s_cancelReason;  // why not (logged)
bool s_stoppedAtUi;          // recording ended at the first UI draw
uint32_t s_opCounts[OP_COUNT];
uint32_t s_frameCommands;
double s_uploadCopyMs;  // time spent copying upload data while recording (this frame)

// ---- statistics, logged every 30 s -----------------------------------------------------------
uint32_t s_statFrames, s_statReplayed, s_statCancelled, s_statNoCamera;
double s_statReplayMs, s_statReplayMaxMs, s_statCopyMs, s_statCommands, s_statBytes, s_statShadowDrawsSkipped;
DWORD s_lastReportTick;
char s_lastCancelReason[160];
bool s_loggedFirstFrame;

void CancelRecording(const char* reason) {
	if (s_recordingValid) {
		s_recordingValid = false;
		s_cancelReason = reason;
	}
	g_recording = false;
}

// ---- arena -----------------------------------------------------------------------------------
uint8_t* AppendCommand(Op op, uint16_t arg, size_t payloadBytes) {
	size_t total = sizeof(CmdHeader) + ((payloadBytes + 7) & ~(size_t)7);
	if (s_arenaUsed + total > kArenaBytes) {
		CancelRecording("recording memory full");
		return nullptr;
	}
	CmdHeader* header = (CmdHeader*)(s_arena + s_arenaUsed);
	header->op = op;
	header->arg = arg;
	header->payloadSize = (uint32_t)((payloadBytes + 7) & ~(size_t)7);
	uint8_t* payload = s_arena + s_arenaUsed + sizeof(CmdHeader);
	s_arenaUsed += total;
	if (op < OP_COUNT) s_opCounts[op]++;
	s_frameCommands++;
	return payload;
}
void HoldRef(IUnknown* obj) {
	if (!obj) return;
	if (s_refCount >= kMaxRefs) {
		CancelRecording("too many objects referenced");
		return;
	}
	obj->AddRef();
	s_refs[s_refCount++] = obj;
}
void ReleaseHeldRefs() {
	for (size_t i = 0; i < s_refCount; i++) s_refs[i]->Release();
	s_refCount = 0;
}

// Takes the lock and checks the recorder is still on (a call may race with ReplayStop).
struct RecordLock {
	bool recording;
	RecordLock() {
		AcquireSRWLockExclusive(&s_recordLock);
		recording = g_recording && s_recordingValid;
	}
	~RecordLock() { ReleaseSRWLockExclusive(&s_recordLock); }
};

// ---- formats: bytes of a row of `w` texels and number of rows for `h` texel rows ---------------
bool FormatInfo(DXGI_FORMAT format, UINT* bytesPerBlock, UINT* blockDim) {
	*blockDim = 1;
	switch (format) {
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32G32B32A32_SINT:
			*bytesPerBlock = 16;
			return true;
		case DXGI_FORMAT_R32G32B32_TYPELESS:
		case DXGI_FORMAT_R32G32B32_FLOAT:
		case DXGI_FORMAT_R32G32B32_UINT:
		case DXGI_FORMAT_R32G32B32_SINT:
			*bytesPerBlock = 12;
			return true;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R16G16B16A16_SINT:
		case DXGI_FORMAT_R32G32_TYPELESS:
		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R32G32_UINT:
		case DXGI_FORMAT_R32G32_SINT:
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			*bytesPerBlock = 8;
			return true;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R10G10B10A2_UINT:
		case DXGI_FORMAT_R11G11B10_FLOAT:
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UINT:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
		case DXGI_FORMAT_R8G8B8A8_SINT:
		case DXGI_FORMAT_R16G16_TYPELESS:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_UINT:
		case DXGI_FORMAT_R16G16_SNORM:
		case DXGI_FORMAT_R16G16_SINT:
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R32_UINT:
		case DXGI_FORMAT_R32_SINT:
		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
			*bytesPerBlock = 4;
			return true;
		case DXGI_FORMAT_R8G8_TYPELESS:
		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R8G8_UINT:
		case DXGI_FORMAT_R8G8_SNORM:
		case DXGI_FORMAT_R8G8_SINT:
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_UINT:
		case DXGI_FORMAT_R16_SNORM:
		case DXGI_FORMAT_R16_SINT:
		case DXGI_FORMAT_B5G6R5_UNORM:
		case DXGI_FORMAT_B5G5R5A1_UNORM:
		case DXGI_FORMAT_B4G4R4A4_UNORM:
			*bytesPerBlock = 2;
			return true;
		case DXGI_FORMAT_R8_TYPELESS:
		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_R8_UINT:
		case DXGI_FORMAT_R8_SNORM:
		case DXGI_FORMAT_R8_SINT:
		case DXGI_FORMAT_A8_UNORM:
			*bytesPerBlock = 1;
			return true;
		case DXGI_FORMAT_BC1_TYPELESS:
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
		case DXGI_FORMAT_BC4_TYPELESS:
		case DXGI_FORMAT_BC4_UNORM:
		case DXGI_FORMAT_BC4_SNORM:
			*bytesPerBlock = 8;
			*blockDim = 4;
			return true;
		case DXGI_FORMAT_BC2_TYPELESS:
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
		case DXGI_FORMAT_BC3_TYPELESS:
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
		case DXGI_FORMAT_BC5_TYPELESS:
		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC5_SNORM:
		case DXGI_FORMAT_BC6H_TYPELESS:
		case DXGI_FORMAT_BC6H_UF16:
		case DXGI_FORMAT_BC6H_SF16:
		case DXGI_FORMAT_BC7_TYPELESS:
		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			*bytesPerBlock = 16;
			*blockDim = 4;
			return true;
		default: return false;
	}
}

// Extent of a subresource (or a box in it): w x h x d texels, its format, and whether it's a buffer.
struct Extent {
	bool isBuffer;
	UINT width, height, depth;
	DXGI_FORMAT format;
	UINT byteWidth, bind, usage;
};
bool GetExtent(ID3D11Resource* res, UINT sub, Extent* e) {
	*e = {};
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	switch (dim) {
		case D3D11_RESOURCE_DIMENSION_BUFFER: {
			D3D11_BUFFER_DESC desc;
			((ID3D11Buffer*)res)->GetDesc(&desc);
			e->isBuffer = true;
			e->width = e->byteWidth = desc.ByteWidth;
			e->height = e->depth = 1;
			e->bind = desc.BindFlags;
			e->usage = desc.Usage;
			return true;
		}
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
			D3D11_TEXTURE1D_DESC desc;
			((ID3D11Texture1D*)res)->GetDesc(&desc);
			UINT mip = sub % (desc.MipLevels ? desc.MipLevels : 1);
			e->width = desc.Width >> mip ? desc.Width >> mip : 1;
			e->height = e->depth = 1;
			e->format = desc.Format;
			e->usage = desc.Usage;
			return true;
		}
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
			D3D11_TEXTURE2D_DESC desc;
			((ID3D11Texture2D*)res)->GetDesc(&desc);
			UINT mip = sub % (desc.MipLevels ? desc.MipLevels : 1);
			e->width = desc.Width >> mip ? desc.Width >> mip : 1;
			e->height = desc.Height >> mip ? desc.Height >> mip : 1;
			e->depth = 1;
			e->format = desc.Format;
			e->usage = desc.Usage;
			return true;
		}
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
			D3D11_TEXTURE3D_DESC desc;
			((ID3D11Texture3D*)res)->GetDesc(&desc);
			UINT mip = sub % (desc.MipLevels ? desc.MipLevels : 1);
			e->width = desc.Width >> mip ? desc.Width >> mip : 1;
			e->height = desc.Height >> mip ? desc.Height >> mip : 1;
			e->depth = desc.Depth >> mip ? desc.Depth >> mip : 1;
			e->format = desc.Format;
			e->usage = desc.Usage;
			return true;
		}
		default: return false;
	}
}
// Bytes of source data an upload of w x h x d texels reads, with the given pitches.
bool UploadBytes(const Extent& e, UINT width, UINT height, UINT depth, UINT rowPitch, UINT depthPitch, size_t* out) {
	if (e.isBuffer) {
		*out = width;
		return true;
	}
	UINT bytesPerBlock, blockDim;
	if (!FormatInfo(e.format, &bytesPerBlock, &blockDim)) return false;
	UINT blocksWide = (width + blockDim - 1) / blockDim, blocksHigh = (height + blockDim - 1) / blockDim;
	size_t rowBytes = (size_t)blocksWide * bytesPerBlock;
	size_t sliceBytes = blocksHigh ? (size_t)rowPitch * (blocksHigh - 1) + rowBytes : 0;
	*out = depth ? (size_t)depthPitch * (depth - 1) + sliceBytes : 0;
	return true;
}

// Fast copy out of mapped (possibly write-combined) memory.
__attribute__((target("sse4.1"))) void CopyFromMapped(void* dst, const void* src, size_t bytes) {
	const uint8_t* from = (const uint8_t*)src;
	uint8_t* to = (uint8_t*)dst;
	if ((((uintptr_t)from) & 15) == 0) {
		size_t blocks = bytes / 64;
		for (size_t i = 0; i < blocks; i++) {
			__m128i q0 = _mm_stream_load_si128((__m128i*)(from + 0));
			__m128i q1 = _mm_stream_load_si128((__m128i*)(from + 16));
			__m128i q2 = _mm_stream_load_si128((__m128i*)(from + 32));
			__m128i q3 = _mm_stream_load_si128((__m128i*)(from + 48));
			_mm_storeu_si128((__m128i*)(to + 0), q0);
			_mm_storeu_si128((__m128i*)(to + 16), q1);
			_mm_storeu_si128((__m128i*)(to + 32), q2);
			_mm_storeu_si128((__m128i*)(to + 48), q3);
			from += 64;
			to += 64;
		}
		bytes -= blocks * 64;
	}
	memcpy(to, from, bytes);
}

// ---- open Maps (between Map and Unmap) --------------------------------------------------------
struct OpenMap {
	ID3D11Resource* res;
	UINT sub;
	D3D11_MAP type;
	UINT flags;
	D3D11_MAPPED_SUBRESOURCE mapped;
};
OpenMap s_openMaps[64];

// Payloads of OP_MAP and OP_UPDATE; the uploaded bytes follow.
struct MapCommand {
	void* res;
	uint32_t sub, type, flags, rowPitch, depthPitch, bytes, isConstantBuffer, pad;
};
struct UpdateCommand {
	void* res;
	uint32_t sub, hasBox;
	D3D11_BOX box;
	uint32_t rowPitch, depthPitch, bytes, isConstantBuffer;
};
}  // namespace

// Recorder entry points (called from the hooks before the original call, lock taken inside)
struct SlotRange {  // payload of Set{CB,SRV,Samp} and similar: start, count, then pointers
	uint32_t start, count;
};

void RecSetSlots(int op, int stage, UINT start, UINT count, void* const* objs) {
	RecordLock lock;
	if (!lock.recording) return;
	if (count > 128) count = 128;
	uint8_t* payload = AppendCommand((Op)op, (uint16_t)stage, sizeof(SlotRange) + count * 8);
	if (!payload) return;
	SlotRange range = {start, count};
	memcpy(payload, &range, sizeof(range));
	for (UINT i = 0; i < count; i++) {
		void* obj = objs ? objs[i] : nullptr;
		memcpy(payload + sizeof(range) + i * 8, &obj, 8);
		HoldRef((IUnknown*)obj);
	}
}
void RecSetCB(int stage, UINT start, UINT count, ID3D11Buffer* const* buffers) { RecSetSlots(OP_SET_CB, stage, start, count, (void* const*)buffers); }
void RecSetSRV(int stage, UINT start, UINT count, ID3D11ShaderResourceView* const* views) {
	RecSetSlots(OP_SET_SRV, stage, start, count, (void* const*)views);
}
void RecSetSamplers(int stage, UINT start, UINT count, ID3D11SamplerState* const* samplers) {
	RecSetSlots(OP_SET_SAMPLERS, stage, start, count, (void* const*)samplers);
}
void RecSetShader(int stage, void* shader, ID3D11ClassInstance* const* classInstances, UINT count) {
	RecordLock lock;
	if (!lock.recording) return;
	if (count > 16) count = 16;
	uint8_t* payload = AppendCommand(OP_SET_SHADER, (uint16_t)stage, 16 + count * 8);
	if (!payload) return;
	uint64_t header[2] = {(uint64_t)(uintptr_t)shader, count};
	memcpy(payload, header, 16);
	HoldRef((IUnknown*)shader);
	for (UINT i = 0; i < count; i++) {
		void* obj = classInstances ? classInstances[i] : nullptr;
		memcpy(payload + 16 + i * 8, &obj, 8);
		HoldRef((IUnknown*)obj);
	}
}
void RecCSSetUAV(UINT start, UINT count, ID3D11UnorderedAccessView* const* views, const UINT* initialCounts) {
	RecordLock lock;
	if (!lock.recording) return;
	if (count > 64) count = 64;
	uint8_t* payload = AppendCommand(OP_CS_UAV, initialCounts ? 1 : 0, sizeof(SlotRange) + count * 12);
	if (!payload) return;
	SlotRange range = {start, count};
	memcpy(payload, &range, sizeof(range));
	for (UINT i = 0; i < count; i++) {
		void* obj = views ? views[i] : nullptr;
		memcpy(payload + sizeof(range) + i * 8, &obj, 8);
		HoldRef((IUnknown*)obj);
		UINT initialCount = initialCounts ? initialCounts[i] : (UINT)-1;
		memcpy(payload + sizeof(range) + count * 8 + i * 4, &initialCount, 4);
	}
}
void RecDraw(const DrawArgs& args) {
	RecordLock lock;
	if (!lock.recording) return;
	uint8_t* payload = AppendCommand(OP_DRAW, (uint16_t)args.kind, 20);
	if (!payload) return;
	uint32_t fields[5] = {args.arg0, args.arg1, (uint32_t)args.arg2, args.arg3, args.arg4};
	memcpy(payload, fields, 20);
}
void RecDispatch(UINT x, UINT y, UINT z, int viewFixKind) {
	RecordLock lock;
	if (!lock.recording) return;
	uint8_t* payload = AppendCommand(OP_DISPATCH, (uint16_t)viewFixKind, 12);
	if (!payload) return;
	uint32_t groups[3] = {x, y, z};
	memcpy(payload, groups, 12);
}
void RecSetRenderTargets(UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
	RecordLock lock;
	if (!lock.recording) return;
	if (count > 8) count = 8;
	uint8_t* payload = AppendCommand(OP_OM_RT, (uint16_t)count, 8 + count * 8);
	if (!payload) return;
	memcpy(payload, &dsv, 8);
	HoldRef(dsv);
	for (UINT i = 0; i < count; i++) {
		void* obj = rtvs ? rtvs[i] : nullptr;
		memcpy(payload + 8 + i * 8, &obj, 8);
		HoldRef((IUnknown*)obj);
	}
}
void RecSetViewports(UINT count, const D3D11_VIEWPORT* viewports) {
	RecordLock lock;
	if (!lock.recording) return;
	if (count > 16) count = 16;
	uint8_t* payload = AppendCommand(OP_RS_VP, (uint16_t)count, count * sizeof(D3D11_VIEWPORT));
	if (payload && viewports) memcpy(payload, viewports, count * sizeof(D3D11_VIEWPORT));
}
void RecUnsupported(const char* what) {
	RecordLock lock;
	if (!lock.recording) return;
	CancelRecording(what);
}

// ---- dynamic vertex/index/structured buffers ------------------------------------------------
// Copying their contents on the CPU at every Map could be huge (a particle buffer written in many
// small NO_OVERWRITE pieces). Instead each "generation" of such a buffer (from a DISCARD, or its
// first Map in the frame, to the next DISCARD or the end of the recording) is copied once on the
// GPU into a snapshot buffer when it ends: NO_OVERWRITE never changes data the GPU still uses, so
// the final contents serve every draw of that generation. Playback binds the snapshot instead.
namespace {
struct Generation {
	ID3D11Buffer* buffer;
	ID3D11Buffer* snapshot;
	bool open;
};
Generation s_generations[1024];
int32_t s_generationCount;
struct PooledBuffer {
	UINT byteWidth, bind, misc, stride;
	ID3D11Buffer* buffer;
	bool inUse;
};
PooledBuffer s_pool[1024];
int32_t s_poolCount;
size_t s_poolBytes;
constexpr size_t kPoolMaxBytes = 768u << 20;

bool IsDynamicBuffer(ID3D11Resource* res, D3D11_BUFFER_DESC* desc) {
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
	((ID3D11Buffer*)res)->GetDesc(desc);
	return desc->Usage == D3D11_USAGE_DYNAMIC && !(desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER);
}
ID3D11Buffer* TakePooledBuffer(const D3D11_BUFFER_DESC& desc) {
	for (int i = 0; i < s_poolCount; i++) {
		PooledBuffer& pooled = s_pool[i];
		if (!pooled.inUse && pooled.byteWidth == desc.ByteWidth && pooled.bind == desc.BindFlags && pooled.misc == desc.MiscFlags &&
			pooled.stride == desc.StructureByteStride) {
			pooled.inUse = true;
			return pooled.buffer;
		}
	}
	if (s_poolCount >= 1024 || s_poolBytes + desc.ByteWidth > kPoolMaxBytes) return nullptr;
	D3D11_BUFFER_DESC defaultDesc = desc;
	defaultDesc.Usage = D3D11_USAGE_DEFAULT;
	defaultDesc.CPUAccessFlags = 0;
	ComPtr<ID3D11Device> device;
	g_gameContext->GetDevice(&device);
	ID3D11Buffer* buffer = nullptr;
	HRESULT hr = device ? device->CreateBuffer(&defaultDesc, nullptr, &buffer) : E_FAIL;
	if (FAILED(hr) || !buffer) return nullptr;
	s_pool[s_poolCount++] = {desc.ByteWidth, desc.BindFlags, desc.MiscFlags, desc.StructureByteStride, buffer, true};
	s_poolBytes += desc.ByteWidth;
	return buffer;
}
// GPU copy of the generation's final contents (recorder off or suspended while issuing it).
void CloseGeneration(Generation& gen) {
	if (!gen.open) return;
	gen.open = false;
	++g_modCallsInProgress;
	g_gameContext->CopyResource(gen.snapshot, gen.buffer);
	--g_modCallsInProgress;
}
Generation* OpenGenerationOf(ID3D11Resource* res) {
	for (int i = s_generationCount - 1; i >= 0; i--)
		if (s_generations[i].buffer == (ID3D11Buffer*)res) return s_generations[i].open ? &s_generations[i] : nullptr;
	return nullptr;
}
}  // namespace

// Before the game's Map: a DISCARD ends the buffer's current generation.
void RecBeforeMap(ID3D11Resource* res, D3D11_MAP type) {
	if (type != D3D11_MAP_WRITE_DISCARD) return;
	RecordLock lock;
	if (!lock.recording) return;
	D3D11_BUFFER_DESC desc;
	if (!IsDynamicBuffer(res, &desc)) return;
	if (Generation* gen = OpenGenerationOf(res)) CloseGeneration(*gen);
}

// Map: remember where the game writes; Unmap copies what it wrote (before any patching).
void RecMap(ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, const D3D11_MAPPED_SUBRESOURCE* mapped) {
	if (type == D3D11_MAP_READ || !mapped || !mapped->pData) return;
	RecordLock lock;
	if (!lock.recording) return;
	D3D11_BUFFER_DESC desc;
	if (IsDynamicBuffer(res, &desc)) {
		if (OpenGenerationOf(res)) return;  // NO_OVERWRITE into the current generation
		if (s_generationCount >= 1024) {
			CancelRecording("too many dynamic buffer refills");
			return;
		}
		ID3D11Buffer* snapshot = TakePooledBuffer(desc);
		if (!snapshot) {
			CancelRecording("out of snapshot buffers for dynamic buffers");
			return;
		}
		int genIndex = s_generationCount++;
		s_generations[genIndex] = {(ID3D11Buffer*)res, snapshot, true};
		uint8_t* payload = AppendCommand(OP_NEW_GENERATION, 0, 16);
		if (!payload) return;
		uint64_t ids[2] = {(uint64_t)(uintptr_t)res, (uint64_t)genIndex};
		memcpy(payload, ids, 16);
		HoldRef(res);
		return;
	}
	for (auto& open : s_openMaps)
		if (!open.res) {
			open = {res, sub, type, flags, *mapped};
			return;
		}
	CancelRecording("too many buffers mapped at once");
}
void RecUnmap(ID3D11Resource* res, UINT sub) {
	RecordLock lock;
	if (!lock.recording) return;
	OpenMap entry = {};
	for (auto& open : s_openMaps)
		if (open.res == res && open.sub == sub) {
			entry = open;
			open.res = nullptr;
			break;
		}
	if (!entry.res) return;  // a READ map, or mapped before the recording started
	Extent extent;
	size_t bytes = 0;
	if (!GetExtent(res, sub, &extent) || !UploadBytes(extent, extent.width, extent.height, extent.depth, entry.mapped.RowPitch, entry.mapped.DepthPitch, &bytes)) {
		CancelRecording("a mapped texture has a format the recorder doesn't know");
		return;
	}
	if (bytes > kMaxUploadBytes) {
		CancelRecording("a mapped resource is too large to record");
		return;
	}
	MapCommand cmd = {res, sub, (uint32_t)entry.type, entry.flags, entry.mapped.RowPitch, entry.mapped.DepthPitch, (uint32_t)bytes,
					  (uint32_t)(extent.isBuffer && (extent.bind & D3D11_BIND_CONSTANT_BUFFER) ? 1 : 0), 0};
	uint8_t* payload = AppendCommand(OP_MAP, 0, sizeof(cmd) + bytes);
	if (!payload) return;
	memcpy(payload, &cmd, sizeof(cmd));
	double startMs = NowMs();
	CopyFromMapped(payload + sizeof(cmd), entry.mapped.pData, bytes);
	s_uploadCopyMs += NowMs() - startMs;
	HoldRef(res);
}
void RecUpdate(ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT rowPitch, UINT depthPitch) {
	RecordLock lock;
	if (!lock.recording || !res || !data) return;
	Extent extent;
	if (!GetExtent(res, sub, &extent)) {
		CancelRecording("UpdateSubresource on an unknown resource type");
		return;
	}
	UINT width = box ? box->right - box->left : extent.width, height = box ? box->bottom - box->top : extent.height,
		 depth = box ? box->back - box->front : extent.depth;
	if (box && (box->right <= box->left || box->bottom <= box->top || box->back <= box->front)) return;  // no-op
	size_t bytes = 0;
	if (!UploadBytes(extent, width, height, depth, rowPitch, depthPitch, &bytes)) {
		CancelRecording("UpdateSubresource on a texture format the recorder doesn't know");
		return;
	}
	if (bytes > kMaxUploadBytes) {
		CancelRecording("an UpdateSubresource too large to record");
		return;
	}
	UpdateCommand cmd = {res, sub, box ? 1u : 0u, box ? *box : D3D11_BOX{}, rowPitch, depthPitch, (uint32_t)bytes,
						 (uint32_t)(extent.isBuffer && (extent.bind & D3D11_BIND_CONSTANT_BUFFER) && !box ? 1 : 0)};
	uint8_t* payload = AppendCommand(OP_UPDATE, 0, sizeof(cmd) + bytes);
	if (!payload) return;
	memcpy(payload, &cmd, sizeof(cmd));
	memcpy(payload + sizeof(cmd), data, bytes);
	HoldRef(res);
}

// Hooks for the context methods d3d_hooks.cpp doesn't hook already
namespace {
typedef void(STDMETHODCALLTYPE* PFN_SetSlots)(ID3D11DeviceContext* c, UINT, UINT, void* const*);
typedef void(STDMETHODCALLTYPE* PFN_SetShader)(ID3D11DeviceContext* c, void*, ID3D11ClassInstance* const*, UINT);
// slot setters: op, stage, vtable index
struct SlotHook {
	int32_t op, stage, index;
	const char* name;
	PFN_SetSlots orig;
};
SlotHook s_slotHooks[] = {
	{OP_SET_SAMPLERS, RS_PS, 10, "PSSetSamplers", nullptr},
	{OP_SET_SRV, RS_VS, 25, "VSSetShaderResources", nullptr},
	{OP_SET_SAMPLERS, RS_VS, 26, "VSSetSamplers", nullptr},
	{OP_SET_SRV, RS_GS, 31, "GSSetShaderResources", nullptr},
	{OP_SET_SAMPLERS, RS_GS, 32, "GSSetSamplers", nullptr},
	{OP_SET_SRV, RS_HS, 59, "HSSetShaderResources", nullptr},
	{OP_SET_SAMPLERS, RS_HS, 61, "HSSetSamplers", nullptr},
	{OP_SET_CB, RS_HS, 62, "HSSetConstantBuffers", nullptr},
	{OP_SET_SRV, RS_DS, 63, "DSSetShaderResources", nullptr},
	{OP_SET_SAMPLERS, RS_DS, 65, "DSSetSamplers", nullptr},
	{OP_SET_CB, RS_DS, 66, "DSSetConstantBuffers", nullptr},
	{OP_SET_SAMPLERS, RS_CS, 70, "CSSetSamplers", nullptr},
};
static_assert(std::size(s_slotHooks) == 12, "H_Slots<0..11> below");
template <int I>
void STDMETHODCALLTYPE H_Slots(ID3D11DeviceContext* c, UINT startSlot, UINT count, void* const* objs) {
	if (ShouldRecord(c)) RecSetSlots(s_slotHooks[I].op, s_slotHooks[I].stage, startSlot, count, objs);
	// the frame capture sees vertex-shader resources too (the effect system's per-object data is read there)
	if (s_slotHooks[I].op == OP_SET_SRV && s_slotHooks[I].stage == RS_VS && !g_replaying && CaptureRecording(c))
		CaptureShaderResources(ST_VS, startSlot, count, (ID3D11ShaderResourceView* const*)objs);
	s_slotHooks[I].orig(c, startSlot, count, objs);
}
struct ShaderHook {
	int32_t stage, index;
	const char* name;
	PFN_SetShader orig;
};
ShaderHook s_shaderHooks[] = {{RS_GS, 23, "GSSetShader", nullptr}, {RS_HS, 60, "HSSetShader", nullptr}, {RS_DS, 64, "DSSetShader", nullptr}};
template <int I>
void STDMETHODCALLTYPE H_Shader(ID3D11DeviceContext* c, void* shader, ID3D11ClassInstance* const* classInstances, UINT count) {
	if (ShouldRecord(c)) RecSetShader(s_shaderHooks[I].stage, shader, classInstances, count);
	s_shaderHooks[I].orig(c, shader, classInstances, count);
}

// The rest, one by one.
typedef void(STDMETHODCALLTYPE* PFN_IASetInputLayout)(ID3D11DeviceContext* c, ID3D11InputLayout*);
typedef void(STDMETHODCALLTYPE* PFN_IASetVertexBuffers)(ID3D11DeviceContext* c, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
typedef void(STDMETHODCALLTYPE* PFN_IASetIndexBuffer)(ID3D11DeviceContext* c, ID3D11Buffer*, DXGI_FORMAT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_IASetPrimitiveTopology)(ID3D11DeviceContext* c, D3D11_PRIMITIVE_TOPOLOGY);
typedef void(STDMETHODCALLTYPE* PFN_SetPredication)(ID3D11DeviceContext* c, ID3D11Predicate*, BOOL);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRTUAV)(ID3D11DeviceContext* c, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
												UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
typedef void(STDMETHODCALLTYPE* PFN_OMSetBlendState)(ID3D11DeviceContext* c, ID3D11BlendState*, const FLOAT[4], UINT);
typedef void(STDMETHODCALLTYPE* PFN_OMSetDepthStencilState)(ID3D11DeviceContext* c, ID3D11DepthStencilState*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_SOSetTargets)(ID3D11DeviceContext* c, UINT, ID3D11Buffer* const*, const UINT*);
typedef void(STDMETHODCALLTYPE* PFN_DrawAuto)(ID3D11DeviceContext* c);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndirect)(ID3D11DeviceContext* c, ID3D11Buffer*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_RSSetState)(ID3D11DeviceContext* c, ID3D11RasterizerState*);
typedef void(STDMETHODCALLTYPE* PFN_RSSetScissorRects)(ID3D11DeviceContext* c, UINT, const D3D11_RECT*);
typedef void(STDMETHODCALLTYPE* PFN_CopySubresourceRegion)(ID3D11DeviceContext* c, ID3D11Resource*, UINT, UINT, UINT, UINT,
														   ID3D11Resource*, UINT, const D3D11_BOX*);
typedef void(STDMETHODCALLTYPE* PFN_CopyResource)(ID3D11DeviceContext* c, ID3D11Resource*, ID3D11Resource*);
typedef void(STDMETHODCALLTYPE* PFN_CopyStructureCount)(ID3D11DeviceContext* c, ID3D11Buffer*, UINT, ID3D11UnorderedAccessView*);
typedef void(STDMETHODCALLTYPE* PFN_ClearRTV)(ID3D11DeviceContext* c, ID3D11RenderTargetView*, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE* PFN_ClearUAVUint)(ID3D11DeviceContext* c, ID3D11UnorderedAccessView*, const UINT[4]);
typedef void(STDMETHODCALLTYPE* PFN_ClearUAVFloat)(ID3D11DeviceContext* c, ID3D11UnorderedAccessView*, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE* PFN_ClearDSV)(ID3D11DeviceContext* c, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
typedef void(STDMETHODCALLTYPE* PFN_GenerateMips)(ID3D11DeviceContext* c, ID3D11ShaderResourceView*);
typedef void(STDMETHODCALLTYPE* PFN_SetResourceMinLOD)(ID3D11DeviceContext* c, ID3D11Resource*, FLOAT);
typedef void(STDMETHODCALLTYPE* PFN_Resolve)(ID3D11DeviceContext* c, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
typedef void(STDMETHODCALLTYPE* PFN_ClearState)(ID3D11DeviceContext* c);
typedef void(STDMETHODCALLTYPE* PFN_DiscardResource)(ID3D11DeviceContext* c, ID3D11Resource*);
typedef void(STDMETHODCALLTYPE* PFN_DiscardView)(ID3D11DeviceContext* c, ID3D11View*);
typedef void(STDMETHODCALLTYPE* PFN_Any)();

PFN_IASetInputLayout o_IASetInputLayout;
PFN_IASetVertexBuffers o_IASetVertexBuffers;
PFN_IASetIndexBuffer o_IASetIndexBuffer;
PFN_IASetPrimitiveTopology o_IASetPrimitiveTopology;
PFN_SetPredication o_SetPredication;
PFN_OMSetRTUAV o_OMSetRTUAV;
PFN_OMSetBlendState o_OMSetBlendState;
PFN_OMSetDepthStencilState o_OMSetDepthStencilState;
PFN_SOSetTargets o_SOSetTargets;
PFN_DrawAuto o_DrawAuto;
PFN_DrawIndirect o_DrawIndexedInstancedIndirect, o_DrawInstancedIndirect, o_DispatchIndirect;
PFN_RSSetState o_RSSetState;
PFN_RSSetScissorRects o_RSSetScissorRects;
PFN_CopySubresourceRegion o_CopySubresourceRegion;
PFN_CopyResource o_CopyResource;
PFN_CopyStructureCount o_CopyStructureCount;
PFN_ClearRTV o_ClearRTV;
PFN_ClearUAVUint o_ClearUAVUint;
PFN_ClearUAVFloat o_ClearUAVFloat;
PFN_ClearDSV o_ClearDSV;
PFN_GenerateMips o_GenerateMips;
PFN_SetResourceMinLOD o_SetResourceMinLOD;
PFN_Resolve o_Resolve;
PFN_ClearState o_ClearState;
PFN_DiscardResource o_DiscardResource;
PFN_DiscardView o_DiscardView;
PFN_Any o_Ctx1[8];

void STDMETHODCALLTYPE H_IASetInputLayout(ID3D11DeviceContext* c, ID3D11InputLayout* layout) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_IA_LAYOUT, 0, 8) : nullptr;
		if (payload) {
			memcpy(payload, &layout, 8);
			HoldRef(layout);
		}
	}
	o_IASetInputLayout(c, layout);
}
void STDMETHODCALLTYPE H_IASetVertexBuffers(ID3D11DeviceContext* c, UINT startSlot, UINT count, ID3D11Buffer* const* buffers, const UINT* strides, const UINT* offsets) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		UINT kept = count > 32 ? 32 : count;
		uint8_t* payload = lock.recording ? AppendCommand(OP_IA_VB, 0, 8 + kept * 16) : nullptr;
		if (payload) {
			uint32_t head[2] = {startSlot, kept};
			memcpy(payload, head, 8);
			for (UINT i = 0; i < kept; i++) {
				void* obj = buffers ? buffers[i] : nullptr;
				UINT stride = strides ? strides[i] : 0, offset = offsets ? offsets[i] : 0;
				memcpy(payload + 8 + i * 8, &obj, 8);
				memcpy(payload + 8 + kept * 8 + i * 4, &stride, 4);
				memcpy(payload + 8 + kept * 12 + i * 4, &offset, 4);
				HoldRef((IUnknown*)obj);
			}
		}
	}
	o_IASetVertexBuffers(c, startSlot, count, buffers, strides, offsets);
}
void STDMETHODCALLTYPE H_IASetIndexBuffer(ID3D11DeviceContext* c, ID3D11Buffer* buffer, DXGI_FORMAT format, UINT offset) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_IA_IB, 0, 16) : nullptr;
		if (payload) {
			uint32_t fields[2] = {(uint32_t)format, offset};
			memcpy(payload, &buffer, 8);
			memcpy(payload + 8, fields, 8);
			HoldRef(buffer);
		}
	}
	o_IASetIndexBuffer(c, buffer, format, offset);
}
void STDMETHODCALLTYPE H_IASetPrimitiveTopology(ID3D11DeviceContext* c, D3D11_PRIMITIVE_TOPOLOGY topology) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		if (lock.recording) AppendCommand(OP_IA_TOPOLOGY, (uint16_t)topology, 0);
	}
	o_IASetPrimitiveTopology(c, topology);
}
void STDMETHODCALLTYPE H_SetPredication(ID3D11DeviceContext* c, ID3D11Predicate* predicate, BOOL value) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_PREDICATION, (uint16_t)(value ? 1 : 0), 8) : nullptr;
		if (payload) {
			memcpy(payload, &predicate, 8);
			HoldRef(predicate);
		}
	}
	o_SetPredication(c, predicate, value);
}
void STDMETHODCALLTYPE H_OMSetRTUAV(ID3D11DeviceContext* c, UINT rtvCount, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
									UINT uavStart, UINT uavCount, ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		UINT rtvsKept = rtvCount == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL ? 0 : (rtvCount > 8 ? 8 : rtvCount);
		UINT uavsKept = uavCount == D3D11_KEEP_UNORDERED_ACCESS_VIEWS ? 0 : (uavCount > 8 ? 8 : uavCount);
		uint8_t* payload = lock.recording ? AppendCommand(OP_OM_RT_UAV, 0, 24 + rtvsKept * 8 + uavsKept * 12) : nullptr;
		if (payload) {
			uint32_t head[4] = {rtvCount, uavStart, uavCount, initialCounts ? 1u : 0u};
			memcpy(payload, head, 16);
			memcpy(payload + 16, &dsv, 8);
			if (rtvCount != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) HoldRef(dsv);
			for (UINT i = 0; i < rtvsKept; i++) {
				void* obj = rtvs ? rtvs[i] : nullptr;
				memcpy(payload + 24 + i * 8, &obj, 8);
				HoldRef((IUnknown*)obj);
			}
			for (UINT i = 0; i < uavsKept; i++) {
				void* obj = uavs ? uavs[i] : nullptr;
				UINT initialCount = initialCounts ? initialCounts[i] : (UINT)-1;
				memcpy(payload + 24 + rtvsKept * 8 + i * 8, &obj, 8);
				memcpy(payload + 24 + rtvsKept * 8 + uavsKept * 8 + i * 4, &initialCount, 4);
				HoldRef((IUnknown*)obj);
			}
		}
	}
	if (c == g_gameContext && !g_replaying && rtvCount != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
		GpuTimingTargets(c, rtvCount, rtvs, dsv, "");
		FoveatedOnTargets(c, rtvCount, rtvs, dsv);
	}
	o_OMSetRTUAV(c, rtvCount, rtvs, dsv, uavStart, uavCount, uavs, initialCounts);
}
void STDMETHODCALLTYPE H_OMSetBlendState(ID3D11DeviceContext* c, ID3D11BlendState* blendState, const FLOAT blendFactor[4], UINT sampleMask) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_OM_BLEND, (uint16_t)(blendFactor ? 1 : 0), 8 + 16 + 4) : nullptr;
		if (payload) {
			memcpy(payload, &blendState, 8);
			if (blendFactor) memcpy(payload + 8, blendFactor, 16);
			memcpy(payload + 24, &sampleMask, 4);
			HoldRef(blendState);
		}
	}
	o_OMSetBlendState(c, blendState, blendFactor, sampleMask);
}
void STDMETHODCALLTYPE H_OMSetDepthStencilState(ID3D11DeviceContext* c, ID3D11DepthStencilState* depthState, UINT stencilRef) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_OM_DS, 0, 12) : nullptr;
		if (payload) {
			memcpy(payload, &depthState, 8);
			memcpy(payload + 8, &stencilRef, 4);
			HoldRef(depthState);
		}
	}
	o_OMSetDepthStencilState(c, depthState, stencilRef);
}
void STDMETHODCALLTYPE H_SOSetTargets(ID3D11DeviceContext* c, UINT count, ID3D11Buffer* const* buffers, const UINT* offsets) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		UINT kept = count > 4 ? 4 : count;
		uint8_t* payload = lock.recording ? AppendCommand(OP_SO_TARGETS, (uint16_t)kept, kept * 12) : nullptr;
		if (payload)
			for (UINT i = 0; i < kept; i++) {
				void* obj = buffers ? buffers[i] : nullptr;
				UINT offset = offsets ? offsets[i] : 0;
				memcpy(payload + i * 8, &obj, 8);
				memcpy(payload + kept * 8 + i * 4, &offset, 4);
				HoldRef((IUnknown*)obj);
			}
	}
	o_SOSetTargets(c, count, buffers, offsets);
}
void STDMETHODCALLTYPE H_DrawAuto(ID3D11DeviceContext* c) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		if (lock.recording) AppendCommand(OP_DRAW_AUTO, 0, 0);
	}
	o_DrawAuto(c);
}
void RecIndirect(Op op, uint16_t indexed, ID3D11Buffer* args, UINT alignedOffset) {
	RecordLock lock;
	uint8_t* payload = lock.recording ? AppendCommand(op, indexed, 12) : nullptr;
	if (payload) {
		memcpy(payload, &args, 8);
		memcpy(payload + 8, &alignedOffset, 4);
		HoldRef(args);
	}
}
void STDMETHODCALLTYPE H_DrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT alignedOffset) {
	if (ShouldRecord(c)) RecIndirect(OP_DRAW_INDIRECT, 1, args, alignedOffset);
	o_DrawIndexedInstancedIndirect(c, args, alignedOffset);
}
void STDMETHODCALLTYPE H_DrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT alignedOffset) {
	if (ShouldRecord(c)) RecIndirect(OP_DRAW_INDIRECT, 0, args, alignedOffset);
	o_DrawInstancedIndirect(c, args, alignedOffset);
}
void STDMETHODCALLTYPE H_DispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT alignedOffset) {
	if (ShouldRecord(c)) RecIndirect(OP_DISPATCH_INDIRECT, 0, args, alignedOffset);
	o_DispatchIndirect(c, args, alignedOffset);
}
void STDMETHODCALLTYPE H_RSSetState(ID3D11DeviceContext* c, ID3D11RasterizerState* state) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_RS_STATE, 0, 8) : nullptr;
		if (payload) {
			memcpy(payload, &state, 8);
			HoldRef(state);
		}
	}
	o_RSSetState(c, state);
}
void STDMETHODCALLTYPE H_RSSetScissorRects(ID3D11DeviceContext* c, UINT count, const D3D11_RECT* rects) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		UINT kept = count > 16 ? 16 : count;
		uint8_t* payload = lock.recording ? AppendCommand(OP_RS_SCISSOR, (uint16_t)kept, kept * sizeof(D3D11_RECT)) : nullptr;
		if (payload && rects) memcpy(payload, rects, kept * sizeof(D3D11_RECT));
	}
	o_RSSetScissorRects(c, count, rects);
}
void STDMETHODCALLTYPE H_CopySubresourceRegion(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstSub, UINT x, UINT y, UINT z,
											   ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_COPY_REGION, (uint16_t)(box ? 1 : 0), 16 + 20 + sizeof(D3D11_BOX)) : nullptr;
		if (payload) {
			memcpy(payload, &dst, 8);
			memcpy(payload + 8, &src, 8);
			uint32_t fields[5] = {dstSub, x, y, z, srcSub};
			memcpy(payload + 16, fields, 20);
			if (box) memcpy(payload + 36, box, sizeof(*box));
			HoldRef(dst);
			HoldRef(src);
		}
	}
	GpuTimingWork(c, GPU_WORK_COPY);
	o_CopySubresourceRegion(c, dst, dstSub, x, y, z, src, srcSub, box);
}
void STDMETHODCALLTYPE H_CopyResource(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_COPY, 0, 16) : nullptr;
		if (payload) {
			memcpy(payload, &dst, 8);
			memcpy(payload + 8, &src, 8);
			HoldRef(dst);
			HoldRef(src);
		}
	}
	GpuTimingWork(c, GPU_WORK_COPY);
	o_CopyResource(c, dst, src);
}
void STDMETHODCALLTYPE H_CopyStructureCount(ID3D11DeviceContext* c, ID3D11Buffer* dst, UINT dstOffset, ID3D11UnorderedAccessView* uav) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_COPY_COUNT, 0, 20) : nullptr;
		if (payload) {
			memcpy(payload, &dst, 8);
			memcpy(payload + 8, &uav, 8);
			memcpy(payload + 16, &dstOffset, 4);
			HoldRef(dst);
			HoldRef(uav);
		}
	}
	o_CopyStructureCount(c, dst, dstOffset, uav);
}
void RecClear(Op op, IUnknown* view, const void* values16) {
	RecordLock lock;
	uint8_t* payload = lock.recording ? AppendCommand(op, 0, 24) : nullptr;
	if (payload) {
		memcpy(payload, &view, 8);
		memcpy(payload + 8, values16, 16);
		HoldRef(view);
	}
}
void STDMETHODCALLTYPE H_ClearRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, const FLOAT color[4]) {
	if (ShouldRecord(c)) RecClear(OP_CLEAR_RTV, rtv, color);
	GpuTimingWork(c, GPU_WORK_CLEAR);
	o_ClearRTV(c, rtv, color);
}
void STDMETHODCALLTYPE H_ClearUAVUint(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* uav, const UINT values[4]) {
	if (ShouldRecord(c)) RecClear(OP_CLEAR_UAV_UINT, uav, values);
	GpuTimingWork(c, GPU_WORK_CLEAR);
	o_ClearUAVUint(c, uav, values);
}
void STDMETHODCALLTYPE H_ClearUAVFloat(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* uav, const FLOAT values[4]) {
	if (ShouldRecord(c)) RecClear(OP_CLEAR_UAV_FLOAT, uav, values);
	GpuTimingWork(c, GPU_WORK_CLEAR);
	o_ClearUAVFloat(c, uav, values);
}
void STDMETHODCALLTYPE H_ClearDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_CLEAR_DSV, 0, 20) : nullptr;
		if (payload) {
			memcpy(payload, &dsv, 8);
			uint32_t stencil32 = stencil;
			memcpy(payload + 8, &flags, 4);
			memcpy(payload + 12, &depth, 4);
			memcpy(payload + 16, &stencil32, 4);
			HoldRef(dsv);
		}
	}
	GpuTimingWork(c, GPU_WORK_CLEAR);
	o_ClearDSV(c, dsv, flags, depth, stencil);
}
void STDMETHODCALLTYPE H_GenerateMips(ID3D11DeviceContext* c, ID3D11ShaderResourceView* srv) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_GENERATE_MIPS, 0, 8) : nullptr;
		if (payload) {
			memcpy(payload, &srv, 8);
			HoldRef(srv);
		}
	}
	GpuTimingWork(c, GPU_WORK_OTHER);
	o_GenerateMips(c, srv);
}
void STDMETHODCALLTYPE H_SetResourceMinLOD(ID3D11DeviceContext* c, ID3D11Resource* res, FLOAT minLod) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_MIN_LOD, 0, 12) : nullptr;
		if (payload) {
			memcpy(payload, &res, 8);
			memcpy(payload + 8, &minLod, 4);
			HoldRef(res);
		}
	}
	o_SetResourceMinLOD(c, res, minLod);
}
void STDMETHODCALLTYPE H_Resolve(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT srcSub, DXGI_FORMAT format) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_RESOLVE, 0, 28) : nullptr;
		if (payload) {
			memcpy(payload, &dst, 8);
			memcpy(payload + 8, &src, 8);
			uint32_t fields[3] = {dstSub, srcSub, (uint32_t)format};
			memcpy(payload + 16, fields, 12);
			HoldRef(dst);
			HoldRef(src);
		}
	}
	GpuTimingWork(c, GPU_WORK_OTHER);
	o_Resolve(c, dst, dstSub, src, srcSub, format);
}
void STDMETHODCALLTYPE H_ClearState(ID3D11DeviceContext* c) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		if (lock.recording) AppendCommand(OP_CLEAR_STATE, 0, 0);
	}
	o_ClearState(c);
}
void STDMETHODCALLTYPE H_DiscardResource(ID3D11DeviceContext* c, ID3D11Resource* res) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_DISCARD_RES, 0, 8) : nullptr;
		if (payload) {
			memcpy(payload, &res, 8);
			HoldRef(res);
		}
	}
	o_DiscardResource(c, res);
}
void STDMETHODCALLTYPE H_DiscardView(ID3D11DeviceContext* c, ID3D11View* view) {
	if (ShouldRecord(c)) {
		RecordLock lock;
		uint8_t* payload = lock.recording ? AppendCommand(OP_DISCARD_VIEW, 0, 8) : nullptr;
		if (payload) {
			memcpy(payload, &view, 8);
			HoldRef(view);
		}
	}
	o_DiscardView(c, view);
}
// D3D11.1 calls the recorder can't replay: seeing one in the window cancels the second eye.
// (Pass-through trampolines: all of these take only integer/pointer arguments, so forwarding ten
// pointer-sized arguments hands each original exactly what it was called with.)
template <int I>
void STDMETHODCALLTYPE H_Ctx1(ID3D11DeviceContext* c, void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
							  void* a8, void* a9) {
	static const char* const kNames[8] = {"CopySubresourceRegion1", "UpdateSubresource1", "VSSetConstantBuffers1",
										  "PSSetConstantBuffers1", "CSSetConstantBuffers1", "ClearView",
										  "SwapDeviceContextState", "GSSetConstantBuffers1"};
	if (ShouldRecord(c)) RecUnsupported(kNames[I]);
	// (CopySubresourceRegion1 has 9 arguments after the context; the extra ones are ignored)
	((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext * c, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*)) o_Ctx1[I])(
		c, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}

constexpr UINT kSrvSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;      // 128
constexpr UINT kCbSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;  // 14
constexpr UINT kSamplerSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;         // 16
constexpr UINT kUavSlots = D3D11_PS_CS_UAV_REGISTER_COUNT;                    // 8 (feature level 11.0)
struct StageState {
	ID3D11DeviceChild* shader;
	ID3D11ClassInstance* classInstances[16];
	UINT classInstanceCount;
	ID3D11Buffer* constantBuffers[kCbSlots];
	ID3D11ShaderResourceView* srvs[kSrvSlots];
	ID3D11SamplerState* samplers[kSamplerSlots];
};
struct PipelineState {
	bool valid;
	ID3D11InputLayout* inputLayout;
	ID3D11Buffer* vertexBuffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT], offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	ID3D11Buffer* indexBuffer;
	DXGI_FORMAT indexFormat;
	UINT indexOffset;
	D3D11_PRIMITIVE_TOPOLOGY topology;
	StageState stages[RS_COUNT];
	ID3D11UnorderedAccessView* csUavs[kUavSlots];
	ID3D11RasterizerState* rasterizer;
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT viewportCount;
	D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT scissorCount;
	ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView* dsv;
	ID3D11UnorderedAccessView* omUavs[kUavSlots];
	ID3D11BlendState* blendState;
	FLOAT blendFactor[4];
	UINT sampleMask;
	ID3D11DepthStencilState* depthState;
	UINT stencilRef;
	ID3D11Buffer* soTargets[D3D11_SO_BUFFER_SLOT_COUNT];
	ID3D11Predicate* predicate;
	BOOL predicateValue;
};
PipelineState s_recordStartState, s_presentState, s_finalPassState;

template <typename T>
void ReleaseAll(T** arr, UINT count) {
	for (UINT i = 0; i < count; i++)
		if (arr[i]) {
			arr[i]->Release();
			arr[i] = nullptr;
		}
}
void ReleasePipelineState(PipelineState& state) {
	if (!state.valid) return;
	SafeRelease(state.inputLayout);
	ReleaseAll(state.vertexBuffers, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT);
	SafeRelease(state.indexBuffer);
	for (auto& stage : state.stages) {
		SafeRelease(stage.shader);
		ReleaseAll(stage.classInstances, 16);
		ReleaseAll(stage.constantBuffers, kCbSlots);
		ReleaseAll(stage.srvs, kSrvSlots);
		ReleaseAll(stage.samplers, kSamplerSlots);
	}
	ReleaseAll(state.csUavs, kUavSlots);
	SafeRelease(state.rasterizer);
	ReleaseAll(state.rtvs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
	SafeRelease(state.dsv);
	ReleaseAll(state.omUavs, kUavSlots);
	SafeRelease(state.blendState);
	SafeRelease(state.depthState);
	ReleaseAll(state.soTargets, D3D11_SO_BUFFER_SLOT_COUNT);
	SafeRelease(state.predicate);
	state.valid = false;
}

void CapturePipelineState(ID3D11DeviceContext* c, PipelineState& state) {
	ReleasePipelineState(state);
	state = {};
	c->IAGetInputLayout(&state.inputLayout);
	c->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, state.vertexBuffers, state.strides, state.offsets);
	c->IAGetIndexBuffer(&state.indexBuffer, &state.indexFormat, &state.indexOffset);
	c->IAGetPrimitiveTopology(&state.topology);
	StageState* stages = state.stages;
	stages[RS_VS].classInstanceCount = stages[RS_HS].classInstanceCount = stages[RS_DS].classInstanceCount = stages[RS_GS].classInstanceCount = stages[RS_PS].classInstanceCount = stages[RS_CS].classInstanceCount = 16;
	c->VSGetShader((ID3D11VertexShader**)&stages[RS_VS].shader, stages[RS_VS].classInstances, &stages[RS_VS].classInstanceCount);
	c->HSGetShader((ID3D11HullShader**)&stages[RS_HS].shader, stages[RS_HS].classInstances, &stages[RS_HS].classInstanceCount);
	c->DSGetShader((ID3D11DomainShader**)&stages[RS_DS].shader, stages[RS_DS].classInstances, &stages[RS_DS].classInstanceCount);
	c->GSGetShader((ID3D11GeometryShader**)&stages[RS_GS].shader, stages[RS_GS].classInstances, &stages[RS_GS].classInstanceCount);
	c->PSGetShader((ID3D11PixelShader**)&stages[RS_PS].shader, stages[RS_PS].classInstances, &stages[RS_PS].classInstanceCount);
	c->CSGetShader((ID3D11ComputeShader**)&stages[RS_CS].shader, stages[RS_CS].classInstances, &stages[RS_CS].classInstanceCount);
	c->VSGetConstantBuffers(0, kCbSlots, stages[RS_VS].constantBuffers);
	c->HSGetConstantBuffers(0, kCbSlots, stages[RS_HS].constantBuffers);
	c->DSGetConstantBuffers(0, kCbSlots, stages[RS_DS].constantBuffers);
	c->GSGetConstantBuffers(0, kCbSlots, stages[RS_GS].constantBuffers);
	c->PSGetConstantBuffers(0, kCbSlots, stages[RS_PS].constantBuffers);
	c->CSGetConstantBuffers(0, kCbSlots, stages[RS_CS].constantBuffers);
	c->VSGetShaderResources(0, kSrvSlots, stages[RS_VS].srvs);
	c->HSGetShaderResources(0, kSrvSlots, stages[RS_HS].srvs);
	c->DSGetShaderResources(0, kSrvSlots, stages[RS_DS].srvs);
	c->GSGetShaderResources(0, kSrvSlots, stages[RS_GS].srvs);
	c->PSGetShaderResources(0, kSrvSlots, stages[RS_PS].srvs);
	c->CSGetShaderResources(0, kSrvSlots, stages[RS_CS].srvs);
	c->VSGetSamplers(0, kSamplerSlots, stages[RS_VS].samplers);
	c->HSGetSamplers(0, kSamplerSlots, stages[RS_HS].samplers);
	c->DSGetSamplers(0, kSamplerSlots, stages[RS_DS].samplers);
	c->GSGetSamplers(0, kSamplerSlots, stages[RS_GS].samplers);
	c->PSGetSamplers(0, kSamplerSlots, stages[RS_PS].samplers);
	c->CSGetSamplers(0, kSamplerSlots, stages[RS_CS].samplers);
	c->CSGetUnorderedAccessViews(0, kUavSlots, state.csUavs);
	c->RSGetState(&state.rasterizer);
	state.viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	c->RSGetViewports(&state.viewportCount, state.viewports);
	state.scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	c->RSGetScissorRects(&state.scissorCount, state.scissors);
	c->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, state.rtvs, &state.dsv, 0, kUavSlots,
												 state.omUavs);
	c->OMGetBlendState(&state.blendState, state.blendFactor, &state.sampleMask);
	c->OMGetDepthStencilState(&state.depthState, &state.stencilRef);
	c->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, state.soTargets);
	c->GetPredication(&state.predicate, &state.predicateValue);
	state.valid = true;
}

void ApplyPipelineState(ID3D11DeviceContext* c, const PipelineState& state) {
	if (!state.valid) return;
	c->IASetInputLayout(state.inputLayout);
	c->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, state.vertexBuffers, state.strides, state.offsets);
	c->IASetIndexBuffer(state.indexBuffer, state.indexFormat, state.indexOffset);
	c->IASetPrimitiveTopology(state.topology);
	const StageState* stages = state.stages;
	c->VSSetShader((ID3D11VertexShader*)stages[RS_VS].shader, stages[RS_VS].classInstances, stages[RS_VS].classInstanceCount);
	c->HSSetShader((ID3D11HullShader*)stages[RS_HS].shader, stages[RS_HS].classInstances, stages[RS_HS].classInstanceCount);
	c->DSSetShader((ID3D11DomainShader*)stages[RS_DS].shader, stages[RS_DS].classInstances, stages[RS_DS].classInstanceCount);
	c->GSSetShader((ID3D11GeometryShader*)stages[RS_GS].shader, stages[RS_GS].classInstances, stages[RS_GS].classInstanceCount);
	c->PSSetShader((ID3D11PixelShader*)stages[RS_PS].shader, stages[RS_PS].classInstances, stages[RS_PS].classInstanceCount);
	c->CSSetShader((ID3D11ComputeShader*)stages[RS_CS].shader, stages[RS_CS].classInstances, stages[RS_CS].classInstanceCount);
	c->VSSetConstantBuffers(0, kCbSlots, stages[RS_VS].constantBuffers);
	c->HSSetConstantBuffers(0, kCbSlots, stages[RS_HS].constantBuffers);
	c->DSSetConstantBuffers(0, kCbSlots, stages[RS_DS].constantBuffers);
	c->GSSetConstantBuffers(0, kCbSlots, stages[RS_GS].constantBuffers);
	c->PSSetConstantBuffers(0, kCbSlots, stages[RS_PS].constantBuffers);
	c->CSSetConstantBuffers(0, kCbSlots, stages[RS_CS].constantBuffers);
	// Outputs first (a resource bound as output is unbound from inputs, and the reverse), then
	// the inputs, so each ends up exactly as captured.
	{
		UINT firstUav = kUavSlots;
		for (UINT i = 0; i < kUavSlots; i++)
			if (state.omUavs[i]) {
				firstUav = i;
				break;
			}
		UINT keepCounts[kUavSlots];
		for (auto& count : keepCounts) count = (UINT)-1;
		if (firstUav < kUavSlots)
			c->OMSetRenderTargetsAndUnorderedAccessViews(firstUav, state.rtvs, state.dsv, firstUav, kUavSlots - firstUav,
														 state.omUavs + firstUav, keepCounts);
		else
			c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, state.rtvs, state.dsv);
		c->CSSetUnorderedAccessViews(0, kUavSlots, state.csUavs, keepCounts);
	}
	c->SOSetTargets(D3D11_SO_BUFFER_SLOT_COUNT, state.soTargets, nullptr);
	c->VSSetShaderResources(0, kSrvSlots, stages[RS_VS].srvs);
	c->HSSetShaderResources(0, kSrvSlots, stages[RS_HS].srvs);
	c->DSSetShaderResources(0, kSrvSlots, stages[RS_DS].srvs);
	c->GSSetShaderResources(0, kSrvSlots, stages[RS_GS].srvs);
	c->PSSetShaderResources(0, kSrvSlots, stages[RS_PS].srvs);
	c->CSSetShaderResources(0, kSrvSlots, stages[RS_CS].srvs);
	c->VSSetSamplers(0, kSamplerSlots, stages[RS_VS].samplers);
	c->HSSetSamplers(0, kSamplerSlots, stages[RS_HS].samplers);
	c->DSSetSamplers(0, kSamplerSlots, stages[RS_DS].samplers);
	c->GSSetSamplers(0, kSamplerSlots, stages[RS_GS].samplers);
	c->PSSetSamplers(0, kSamplerSlots, stages[RS_PS].samplers);
	c->CSSetSamplers(0, kSamplerSlots, stages[RS_CS].samplers);
	c->RSSetState(state.rasterizer);
	c->RSSetViewports(state.viewportCount, state.viewports);
	c->RSSetScissorRects(state.scissorCount, state.scissors);
	c->OMSetBlendState(state.blendState, state.blendFactor, state.sampleMask);
	c->OMSetDepthStencilState(state.depthState, state.stencilRef);
	c->SetPredication(state.predicate, state.predicateValue);
}

// Playback
template <typename T>
T Read(const uint8_t* payload, size_t offset) {
	T value;
	memcpy(&value, payload + offset, sizeof(T));
	return value;
}

// Playback bindings of dynamic buffers: the game's own buffer is replaced by the snapshot of the
// generation the recording was in at that point. The game's bindings are tracked so a new
// generation can rebind the slots that use the buffer.
struct Substitute {
	ID3D11Buffer* gameBuffer;
	ID3D11Buffer* snapshot;
};
Substitute s_substitutes[256];
int32_t s_substituteCount;
struct SnapshotView {
	ID3D11ShaderResourceView* gameView;
	ID3D11Buffer* snapshot;
	ID3D11ShaderResourceView* view;
};
SnapshotView s_snapshotViews[256];
int32_t s_snapshotViewCount;
struct GameBindings {
	ID3D11Buffer* vertexBuffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT], offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	ID3D11Buffer* indexBuffer;
	DXGI_FORMAT indexFormat;
	UINT indexOffset;
	ID3D11ShaderResourceView* srvs[RS_COUNT][kSrvSlots];
};
GameBindings s_gameBindings;

ID3D11Buffer* SubstituteBuffer(ID3D11Buffer* buffer) {
	if (!buffer) return buffer;
	for (int i = 0; i < s_substituteCount; i++)
		if (s_substitutes[i].gameBuffer == buffer) return s_substitutes[i].snapshot;
	return buffer;
}
ID3D11ShaderResourceView* SubstituteView(ID3D11ShaderResourceView* view) {
	if (!view || !s_substituteCount) return view;
	ComPtr<ID3D11Resource> res;
	view->GetResource(&res);
	ID3D11Buffer* snapshot = SubstituteBuffer((ID3D11Buffer*)res.Get());
	if (snapshot == (ID3D11Buffer*)res.Get()) return view;
	for (int i = 0; i < s_snapshotViewCount; i++)
		if (s_snapshotViews[i].gameView == view && s_snapshotViews[i].snapshot == snapshot) return s_snapshotViews[i].view;
	if (s_snapshotViewCount >= 256) return view;
	D3D11_SHADER_RESOURCE_VIEW_DESC desc;
	view->GetDesc(&desc);
	ComPtr<ID3D11Device> device;
	g_gameContext->GetDevice(&device);
	ID3D11ShaderResourceView* own = nullptr;
	if (!device || FAILED(device->CreateShaderResourceView(snapshot, &desc, &own))) own = nullptr;
	if (!own) return view;
	s_snapshotViews[s_snapshotViewCount++] = {view, snapshot, own};
	return own;
}
void ReleaseSnapshotViews() {
	for (int i = 0; i < s_snapshotViewCount; i++) s_snapshotViews[i].view->Release();
	s_snapshotViewCount = 0;
}
void SetShaderResources(ID3D11DeviceContext* c, int stage, UINT start, UINT count, ID3D11ShaderResourceView* const* views) {
	switch (stage) {
		case RS_VS: c->VSSetShaderResources(start, count, views); break;
		case RS_HS: c->HSSetShaderResources(start, count, views); break;
		case RS_DS: c->DSSetShaderResources(start, count, views); break;
		case RS_GS: c->GSSetShaderResources(start, count, views); break;
		case RS_PS: c->PSSetShaderResources(start, count, views); break;
		case RS_CS: c->CSSetShaderResources(start, count, views); break;
	}
}
// A new generation of `buf`: rebind every slot that uses it.
void RebindBuffer(ID3D11DeviceContext* c, ID3D11Buffer* buffer) {
	for (UINT i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++)
		if (s_gameBindings.vertexBuffers[i] == buffer) {
			ID3D11Buffer* snapshot = SubstituteBuffer(buffer);
			c->IASetVertexBuffers(i, 1, &snapshot, &s_gameBindings.strides[i], &s_gameBindings.offsets[i]);
		}
	if (s_gameBindings.indexBuffer == buffer) c->IASetIndexBuffer(SubstituteBuffer(buffer), s_gameBindings.indexFormat, s_gameBindings.indexOffset);
	for (int stage = 0; stage < RS_COUNT; stage++)
		for (UINT i = 0; i < kSrvSlots; i++) {
			ID3D11ShaderResourceView* view = s_gameBindings.srvs[stage][i];
			if (!view) continue;
			ComPtr<ID3D11Resource> res;
			view->GetResource(&res);
			if ((ID3D11Buffer*)res.Get() != buffer) continue;
			ID3D11ShaderResourceView* substituted = SubstituteView(view);
			SetShaderResources(c, stage, i, 1, &substituted);
		}
}

// Reused shadow maps (ReuseShadows=1): passes into a square depth texture, with or without colour
// targets of the same size (NieR also writes the depth into an R32_FLOAT target), are the shadow map:
// drawn from the light, not from the eye, so the left eye's is right for the right eye too. The right
// eye's replay skips their draws and the clears of those textures; they still hold the left eye's.
struct ViewSize {
	ID3D11View* view;
	UINT width, height;
};
ViewSize s_viewSizes[64];  // this frame's
int32_t s_viewSizeCount;
ID3D11RenderTargetView* s_shadowRtvs[16];  // colour targets seen in a shadow pass (kept across frames: cleared before they're bound)
int32_t s_shadowRtvCount;
uint32_t s_skippedShadowDraws;
bool s_loggedShadowReuse;

// A view's 2D texture size (0 x 0: not a 2D texture)
ViewSize SizeOf(ID3D11View* view) {
	for (int32_t i = 0; i < s_viewSizeCount; i++)
		if (s_viewSizes[i].view == view) return s_viewSizes[i];
	ViewSize size = {view, 0, 0};
	ComPtr<ID3D11Resource> res;
	view->GetResource(&res);
	D3D11_RESOURCE_DIMENSION dim;
	if (res && (res->GetType(&dim), dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)) {
		D3D11_TEXTURE2D_DESC desc;
		((ID3D11Texture2D*)res.Get())->GetDesc(&desc);
		size.width = desc.Width;
		size.height = desc.Height;
	}
	if (s_viewSizeCount < (int32_t)std::size(s_viewSizes)) s_viewSizes[s_viewSizeCount++] = size;
	return size;
}

bool IsShadowDsv(ID3D11DepthStencilView* view) {
	if (!view) return false;
	ViewSize size = SizeOf(view);
	return size.width == size.height && size.width >= 512;
}

bool IsShadowRtv(ID3D11RenderTargetView* view) {
	for (int32_t i = 0; i < s_shadowRtvCount; i++)
		if (s_shadowRtvs[i] == view) return true;
	return false;
}

// The targets of a shadow pass: a shadow depth texture, and colour targets of its size (or none)
bool IsShadowPass(ID3D11RenderTargetView* const* rtvs, UINT count, ID3D11DepthStencilView* dsv) {
	if (!IsShadowDsv(dsv)) return false;
	ViewSize depth = SizeOf(dsv);
	for (UINT i = 0; i < count; i++) {
		if (!rtvs || !rtvs[i]) continue;
		ViewSize size = SizeOf(rtvs[i]);
		if (size.width != depth.width || size.height != depth.height) return false;
	}
	for (UINT i = 0; i < count; i++)
		if (rtvs && rtvs[i] && !IsShadowRtv(rtvs[i]) && s_shadowRtvCount < (int32_t)std::size(s_shadowRtvs)) s_shadowRtvs[s_shadowRtvCount++] = rtvs[i];
	if (!s_loggedShadowReuse) {
		s_loggedShadowReuse = true;
		Log("synchronized: the right eye reuses the shadow map (%ux%u, %u colour targets) drawn for the left eye", depth.width, depth.height,
			(unsigned)s_shadowRtvCount);
	}
	return true;
}

void PlayRecording(ID3D11DeviceContext* c) {
	float patchedConstants[kPatchScratchFloats];
	bool inShadowPass = false;  // inside a reused shadow pass
	s_viewSizeCount = 0;
	s_skippedShadowDraws = 0;
	s_substituteCount = 0;
	s_gameBindings = {};
	memcpy(s_gameBindings.vertexBuffers, s_recordStartState.vertexBuffers, sizeof(s_gameBindings.vertexBuffers));
	memcpy(s_gameBindings.strides, s_recordStartState.strides, sizeof(s_gameBindings.strides));
	memcpy(s_gameBindings.offsets, s_recordStartState.offsets, sizeof(s_gameBindings.offsets));
	s_gameBindings.indexBuffer = s_recordStartState.indexBuffer;
	s_gameBindings.indexFormat = s_recordStartState.indexFormat;
	s_gameBindings.indexOffset = s_recordStartState.indexOffset;
	for (int stage = 0; stage < RS_COUNT; stage++) memcpy(s_gameBindings.srvs[stage], s_recordStartState.stages[stage].srvs, sizeof(s_gameBindings.srvs[stage]));
	for (size_t pos = 0; pos < s_arenaUsed;) {
		CmdHeader header;
		memcpy(&header, s_arena + pos, sizeof(header));
		const uint8_t* payload = s_arena + pos + sizeof(header);
		pos += sizeof(header) + header.payloadSize;
		switch (header.op) {
			case OP_SET_CB:
			case OP_SET_SRV:
			case OP_SET_SAMPLERS: {
				SlotRange range = Read<SlotRange>(payload, 0);
				void* const* objs = (void* const*)(payload + sizeof(SlotRange));
				UINT start = range.start, count = range.count;
				if (header.op == OP_SET_CB) {
					ID3D11Buffer* const* buffers = (ID3D11Buffer* const*)objs;
					switch (header.arg) {
						case RS_VS: c->VSSetConstantBuffers(start, count, buffers); break;
						case RS_HS: c->HSSetConstantBuffers(start, count, buffers); break;
						case RS_DS: c->DSSetConstantBuffers(start, count, buffers); break;
						case RS_GS: c->GSSetConstantBuffers(start, count, buffers); break;
						case RS_PS: c->PSSetConstantBuffers(start, count, buffers); break;
						case RS_CS: c->CSSetConstantBuffers(start, count, buffers); break;
					}
				} else if (header.op == OP_SET_SRV) {
					ID3D11ShaderResourceView* const* views = (ID3D11ShaderResourceView* const*)objs;
					ID3D11ShaderResourceView* substituted[kSrvSlots];
					for (UINT i = 0; i < count && start + i < kSrvSlots && header.arg < RS_COUNT; i++) {
						s_gameBindings.srvs[header.arg][start + i] = views[i];
						substituted[i] = SubstituteView(views[i]);
					}
					if (count <= kSrvSlots) SetShaderResources(c, header.arg, start, count, substituted);
				} else {
					ID3D11SamplerState* const* samplers = (ID3D11SamplerState* const*)objs;
					switch (header.arg) {
						case RS_VS: c->VSSetSamplers(start, count, samplers); break;
						case RS_HS: c->HSSetSamplers(start, count, samplers); break;
						case RS_DS: c->DSSetSamplers(start, count, samplers); break;
						case RS_GS: c->GSSetSamplers(start, count, samplers); break;
						case RS_PS: c->PSSetSamplers(start, count, samplers); break;
						case RS_CS: c->CSSetSamplers(start, count, samplers); break;
					}
				}
				break;
			}
			case OP_SET_SHADER: {
				void* shader = Read<void*>(payload, 0);
				UINT count = (UINT)Read<uint64_t>(payload, 8);
				ID3D11ClassInstance* const* classInstances = count ? (ID3D11ClassInstance* const*)(payload + 16) : nullptr;
				switch (header.arg) {
					case RS_VS: c->VSSetShader((ID3D11VertexShader*)shader, classInstances, count); break;
					case RS_HS: c->HSSetShader((ID3D11HullShader*)shader, classInstances, count); break;
					case RS_DS: c->DSSetShader((ID3D11DomainShader*)shader, classInstances, count); break;
					case RS_GS: c->GSSetShader((ID3D11GeometryShader*)shader, classInstances, count); break;
					case RS_PS: c->PSSetShader((ID3D11PixelShader*)shader, classInstances, count); break;
					case RS_CS: c->CSSetShader((ID3D11ComputeShader*)shader, classInstances, count); break;
				}
				break;
			}
			case OP_CS_UAV: {
				SlotRange range = Read<SlotRange>(payload, 0);
				const UINT* initialCounts = (const UINT*)(payload + sizeof(SlotRange) + range.count * 8);
				c->CSSetUnorderedAccessViews(range.start, range.count, (ID3D11UnorderedAccessView* const*)(payload + sizeof(SlotRange)),
											 header.arg ? initialCounts : nullptr);
				break;
			}
			case OP_IA_LAYOUT: c->IASetInputLayout(Read<ID3D11InputLayout*>(payload, 0)); break;
			case OP_IA_VB: {
				UINT startSlot = Read<uint32_t>(payload, 0), count = Read<uint32_t>(payload, 4);
				ID3D11Buffer* const* buffers = (ID3D11Buffer* const*)(payload + 8);
				const UINT* strides = (const UINT*)(payload + 8 + count * 8);
				const UINT* offsets = (const UINT*)(payload + 8 + count * 12);
				ID3D11Buffer* substituted[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
				for (UINT i = 0; i < count && startSlot + i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++) {
					s_gameBindings.vertexBuffers[startSlot + i] = buffers[i];
					s_gameBindings.strides[startSlot + i] = strides[i];
					s_gameBindings.offsets[startSlot + i] = offsets[i];
					substituted[i] = SubstituteBuffer(buffers[i]);
				}
				c->IASetVertexBuffers(startSlot, count, substituted, strides, offsets);
				break;
			}
			case OP_IA_IB:
				s_gameBindings.indexBuffer = Read<ID3D11Buffer*>(payload, 0);
				s_gameBindings.indexFormat = (DXGI_FORMAT)Read<uint32_t>(payload, 8);
				s_gameBindings.indexOffset = Read<uint32_t>(payload, 12);
				c->IASetIndexBuffer(SubstituteBuffer(s_gameBindings.indexBuffer), s_gameBindings.indexFormat, s_gameBindings.indexOffset);
				break;
			case OP_NEW_GENERATION: {
				ID3D11Buffer* buffer = (ID3D11Buffer*)(uintptr_t)Read<uint64_t>(payload, 0);
				int genIndex = (int)Read<uint64_t>(payload, 8);
				if (genIndex < 0 || genIndex >= s_generationCount) break;
				int slot = 0;
				while (slot < s_substituteCount && s_substitutes[slot].gameBuffer != buffer) slot++;
				if (slot == s_substituteCount) {
					if (s_substituteCount >= 256) break;
					s_substituteCount++;
				}
				s_substitutes[slot] = {buffer, s_generations[genIndex].snapshot};
				RebindBuffer(c, buffer);
				break;
			}
			case OP_IA_TOPOLOGY: c->IASetPrimitiveTopology((D3D11_PRIMITIVE_TOPOLOGY)header.arg); break;
			case OP_DRAW: {
				if (inShadowPass) {
					s_skippedShadowDraws++;
					break;
				}
				GpuTimingDraw();
				const uint32_t* args = (const uint32_t*)payload;
				switch (header.arg) {
					case DRAW: c->Draw(args[0], args[1]); break;
					case DRAW_INDEXED: c->DrawIndexed(args[0], args[1], (INT)args[2]); break;
					case DRAW_INSTANCED: c->DrawInstanced(args[0], args[1], args[2], args[3]); break;
					case DRAW_INDEXED_INSTANCED: c->DrawIndexedInstanced(args[0], args[1], args[2], (INT)args[3], args[4]); break;
				}
				break;
			}
			case OP_DRAW_AUTO: c->DrawAuto(); break;
			case OP_DRAW_INDIRECT:
				if (header.arg)
					c->DrawIndexedInstancedIndirect(SubstituteBuffer(Read<ID3D11Buffer*>(payload, 0)), Read<uint32_t>(payload, 8));
				else
					c->DrawInstancedIndirect(SubstituteBuffer(Read<ID3D11Buffer*>(payload, 0)), Read<uint32_t>(payload, 8));
				break;
			case OP_DISPATCH: {
				GpuTimingCompute(c, "R ", header.arg);
				GpuTimingWork(c, GPU_WORK_DISPATCH);
				const uint32_t* groups = (const uint32_t*)payload;
				if (header.arg) ViewFixBeforeDispatch(c, header.arg);
				c->Dispatch(groups[0], groups[1], groups[2]);
				if (header.arg) ViewFixAfterDispatch(c);
				break;
			}
			case OP_DISPATCH_INDIRECT: c->DispatchIndirect(SubstituteBuffer(Read<ID3D11Buffer*>(payload, 0)), Read<uint32_t>(payload, 8)); break;
			case OP_OM_RT: {
				ID3D11RenderTargetView* const* rtvs = (ID3D11RenderTargetView* const*)(payload + 8);
				ID3D11DepthStencilView* dsv = Read<ID3D11DepthStencilView*>(payload, 0);
				inShadowPass = g_cfg.reuseShadows && IsShadowPass(rtvs, header.arg, dsv);
				FoveatedOnTargets(c, header.arg, rtvs, dsv);
				c->OMSetRenderTargets(header.arg, rtvs, dsv);
				GpuTimingTargets(c, header.arg, rtvs, dsv, inShadowPass ? "R reused " : "R ");
				break;
			}
			case OP_OM_RT_UAV: {
				UINT rtvCount = Read<uint32_t>(payload, 0), uavStart = Read<uint32_t>(payload, 4), uavCount = Read<uint32_t>(payload, 8),
					 hasCounts = Read<uint32_t>(payload, 12);
				UINT rtvsKept = rtvCount == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL ? 0 : (rtvCount > 8 ? 8 : rtvCount);
				UINT uavsKept = uavCount == D3D11_KEEP_UNORDERED_ACCESS_VIEWS ? 0 : (uavCount > 8 ? 8 : uavCount);
				if (rtvCount != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
					inShadowPass = g_cfg.reuseShadows && uavCount == 0 &&
								   IsShadowPass((ID3D11RenderTargetView* const*)(payload + 24), rtvsKept, Read<ID3D11DepthStencilView*>(payload, 16));
					GpuTimingTargets(c, rtvsKept, (ID3D11RenderTargetView* const*)(payload + 24), Read<ID3D11DepthStencilView*>(payload, 16),
									 inShadowPass ? "R reused " : "R ");
					FoveatedOnTargets(c, rtvsKept, (ID3D11RenderTargetView* const*)(payload + 24), Read<ID3D11DepthStencilView*>(payload, 16));
				}
				c->OMSetRenderTargetsAndUnorderedAccessViews(
					rtvCount, rtvsKept ? (ID3D11RenderTargetView* const*)(payload + 24) : nullptr, Read<ID3D11DepthStencilView*>(payload, 16), uavStart, uavCount,
					uavsKept ? (ID3D11UnorderedAccessView* const*)(payload + 24 + rtvsKept * 8) : nullptr,
					hasCounts && uavsKept ? (const UINT*)(payload + 24 + rtvsKept * 8 + uavsKept * 8) : nullptr);
				break;
			}
			case OP_OM_BLEND:
				c->OMSetBlendState(Read<ID3D11BlendState*>(payload, 0), header.arg ? (const FLOAT*)(payload + 8) : nullptr, Read<uint32_t>(payload, 24));
				break;
			case OP_OM_DS: c->OMSetDepthStencilState(Read<ID3D11DepthStencilState*>(payload, 0), Read<uint32_t>(payload, 8)); break;
			case OP_RS_STATE: c->RSSetState(Read<ID3D11RasterizerState*>(payload, 0)); break;
			case OP_RS_VP: c->RSSetViewports(header.arg, (const D3D11_VIEWPORT*)payload); break;
			case OP_RS_SCISSOR: c->RSSetScissorRects(header.arg, (const D3D11_RECT*)payload); break;
			case OP_SO_TARGETS: c->SOSetTargets(header.arg, (ID3D11Buffer* const*)payload, (const UINT*)(payload + header.arg * 8)); break;
			case OP_COPY_REGION: {
				const uint32_t* fields = (const uint32_t*)(payload + 16);
				c->CopySubresourceRegion(Read<ID3D11Resource*>(payload, 0), fields[0], fields[1], fields[2], fields[3],
										 (ID3D11Resource*)SubstituteBuffer((ID3D11Buffer*)Read<ID3D11Resource*>(payload, 8)), fields[4],
										 header.arg ? (const D3D11_BOX*)(payload + 36) : nullptr);
				break;
			}
			case OP_COPY:
				c->CopyResource(Read<ID3D11Resource*>(payload, 0), (ID3D11Resource*)SubstituteBuffer((ID3D11Buffer*)Read<ID3D11Resource*>(payload, 8)));
				break;
			case OP_UPDATE: {
				UpdateCommand cmd;
				memcpy(&cmd, payload, sizeof(cmd));
				const void* data = payload + sizeof(cmd);
				if (cmd.isConstantBuffer) data = PatchConstantBufferForReplay((ID3D11Resource*)cmd.res, data, cmd.bytes, patchedConstants);
				c->UpdateSubresource((ID3D11Resource*)cmd.res, cmd.sub, cmd.hasBox ? &cmd.box : nullptr, data, cmd.rowPitch, cmd.depthPitch);
				break;
			}
			case OP_COPY_COUNT:
				c->CopyStructureCount(Read<ID3D11Buffer*>(payload, 0), Read<uint32_t>(payload, 16), Read<ID3D11UnorderedAccessView*>(payload, 8));
				break;
			case OP_CLEAR_RTV:
				if (g_cfg.reuseShadows && IsShadowRtv(Read<ID3D11RenderTargetView*>(payload, 0))) break;
				c->ClearRenderTargetView(Read<ID3D11RenderTargetView*>(payload, 0), (const FLOAT*)(payload + 8));
				break;
			case OP_CLEAR_UAV_UINT: c->ClearUnorderedAccessViewUint(Read<ID3D11UnorderedAccessView*>(payload, 0), (const UINT*)(payload + 8)); break;
			case OP_CLEAR_UAV_FLOAT: c->ClearUnorderedAccessViewFloat(Read<ID3D11UnorderedAccessView*>(payload, 0), (const FLOAT*)(payload + 8)); break;
			case OP_CLEAR_DSV:
				if (g_cfg.reuseShadows && IsShadowDsv(Read<ID3D11DepthStencilView*>(payload, 0))) break;
				c->ClearDepthStencilView(Read<ID3D11DepthStencilView*>(payload, 0), Read<uint32_t>(payload, 8), Read<float>(payload, 12),
										 (UINT8)Read<uint32_t>(payload, 16));
				break;
			case OP_GENERATE_MIPS: c->GenerateMips(Read<ID3D11ShaderResourceView*>(payload, 0)); break;
			case OP_MIN_LOD: c->SetResourceMinLOD(Read<ID3D11Resource*>(payload, 0), Read<float>(payload, 8)); break;
			case OP_RESOLVE:
				c->ResolveSubresource(Read<ID3D11Resource*>(payload, 0), Read<uint32_t>(payload, 16), Read<ID3D11Resource*>(payload, 8),
									  Read<uint32_t>(payload, 20), (DXGI_FORMAT)Read<uint32_t>(payload, 24));
				break;
			case OP_MAP: {
				MapCommand cmd;
				memcpy(&cmd, payload, sizeof(cmd));
				const void* data = payload + sizeof(cmd);
				if (cmd.isConstantBuffer) data = PatchConstantBufferForReplay((ID3D11Resource*)cmd.res, data, cmd.bytes, patchedConstants);
				// A buffer written with NO_OVERWRITE is mapped with DISCARD here and gets its whole
				// recorded contents (which include everything written before): draws already
				// submitted keep reading the old copy, so nothing they use is overwritten.
				D3D11_MAP type = (D3D11_MAP)cmd.type;
				if (type == D3D11_MAP_WRITE_NO_OVERWRITE) type = D3D11_MAP_WRITE_DISCARD;
				D3D11_MAPPED_SUBRESOURCE mapped;
				if (SUCCEEDED(c->Map((ID3D11Resource*)cmd.res, cmd.sub, type, 0, &mapped))) {
					if (mapped.RowPitch == cmd.rowPitch && mapped.DepthPitch == cmd.depthPitch) {
						memcpy(mapped.pData, data, cmd.bytes);
					} else {  // a different row pitch this time: copy row by row
						Extent extent;
						if (GetExtent((ID3D11Resource*)cmd.res, cmd.sub, &extent)) {
							UINT bytesPerBlock = 1, blockDim = 1;
							FormatInfo(extent.format, &bytesPerBlock, &blockDim);
							UINT rows = (extent.height + blockDim - 1) / blockDim, rowBytes = ((extent.width + blockDim - 1) / blockDim) * bytesPerBlock;
							for (UINT z = 0; z < extent.depth; z++)
								for (UINT y = 0; y < rows; y++)
									memcpy((uint8_t*)mapped.pData + (size_t)z * mapped.DepthPitch + (size_t)y * mapped.RowPitch,
										   (const uint8_t*)data + (size_t)z * cmd.depthPitch + (size_t)y * cmd.rowPitch, rowBytes);
						}
					}
					c->Unmap((ID3D11Resource*)cmd.res, cmd.sub);
				}
				break;
			}
			case OP_PREDICATION: c->SetPredication(Read<ID3D11Predicate*>(payload, 0), header.arg ? TRUE : FALSE); break;
			case OP_CLEAR_STATE:
				c->ClearState();
				s_gameBindings = {};
				break;
			case OP_DISCARD_RES:
			case OP_DISCARD_VIEW: {
				ComPtr<ID3D11DeviceContext1> context1;
				if (SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), &context1))) {
					if (header.op == OP_DISCARD_RES)
						context1->DiscardResource(Read<ID3D11Resource*>(payload, 0));
					else
						context1->DiscardView(Read<ID3D11View*>(payload, 0));
				}
				break;
			}
			default: break;
		}
	}
}
}  // namespace

// Public API
void ReplayInstall(ID3D11DeviceContext* ctx) {
	static bool installed;
	if (installed) return;
	installed = true;
	g_gameContext = ctx;
	s_arena = (uint8_t*)VirtualAlloc(nullptr, kArenaBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	s_refs = (IUnknown**)VirtualAlloc(nullptr, kMaxRefs * sizeof(IUnknown*), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!s_arena || !s_refs) {
		Log("synchronized: could not reserve recording memory; synchronized mode unavailable");
		return;
	}
	void** vtable = *(void***)ctx;
	int hooked = 0, total = 0;
#define HOOK(index, hook, original, name) \
	total++;                              \
	hooked += HookFunction(vtable[index], (void*)&hook, (void**)&original, name)
	HOOK(10, H_Slots<0>, s_slotHooks[0].orig, s_slotHooks[0].name);
	HOOK(25, H_Slots<1>, s_slotHooks[1].orig, s_slotHooks[1].name);
	HOOK(26, H_Slots<2>, s_slotHooks[2].orig, s_slotHooks[2].name);
	HOOK(31, H_Slots<3>, s_slotHooks[3].orig, s_slotHooks[3].name);
	HOOK(32, H_Slots<4>, s_slotHooks[4].orig, s_slotHooks[4].name);
	HOOK(59, H_Slots<5>, s_slotHooks[5].orig, s_slotHooks[5].name);
	HOOK(61, H_Slots<6>, s_slotHooks[6].orig, s_slotHooks[6].name);
	HOOK(62, H_Slots<7>, s_slotHooks[7].orig, s_slotHooks[7].name);
	HOOK(63, H_Slots<8>, s_slotHooks[8].orig, s_slotHooks[8].name);
	HOOK(65, H_Slots<9>, s_slotHooks[9].orig, s_slotHooks[9].name);
	HOOK(66, H_Slots<10>, s_slotHooks[10].orig, s_slotHooks[10].name);
	HOOK(70, H_Slots<11>, s_slotHooks[11].orig, s_slotHooks[11].name);
	HOOK(23, H_Shader<0>, s_shaderHooks[0].orig, s_shaderHooks[0].name);
	HOOK(60, H_Shader<1>, s_shaderHooks[1].orig, s_shaderHooks[1].name);
	HOOK(64, H_Shader<2>, s_shaderHooks[2].orig, s_shaderHooks[2].name);
	HOOK(17, H_IASetInputLayout, o_IASetInputLayout, "IASetInputLayout");
	HOOK(18, H_IASetVertexBuffers, o_IASetVertexBuffers, "IASetVertexBuffers");
	HOOK(19, H_IASetIndexBuffer, o_IASetIndexBuffer, "IASetIndexBuffer");
	HOOK(24, H_IASetPrimitiveTopology, o_IASetPrimitiveTopology, "IASetPrimitiveTopology");
	HOOK(30, H_SetPredication, o_SetPredication, "SetPredication");
	HOOK(34, H_OMSetRTUAV, o_OMSetRTUAV, "OMSetRenderTargetsAndUnorderedAccessViews");
	HOOK(35, H_OMSetBlendState, o_OMSetBlendState, "OMSetBlendState");
	HOOK(36, H_OMSetDepthStencilState, o_OMSetDepthStencilState, "OMSetDepthStencilState");
	HOOK(37, H_SOSetTargets, o_SOSetTargets, "SOSetTargets");
	HOOK(38, H_DrawAuto, o_DrawAuto, "DrawAuto");
	HOOK(39, H_DrawIndexedInstancedIndirect, o_DrawIndexedInstancedIndirect, "DrawIndexedInstancedIndirect");
	HOOK(40, H_DrawInstancedIndirect, o_DrawInstancedIndirect, "DrawInstancedIndirect");
	HOOK(42, H_DispatchIndirect, o_DispatchIndirect, "DispatchIndirect");
	HOOK(43, H_RSSetState, o_RSSetState, "RSSetState");
	HOOK(45, H_RSSetScissorRects, o_RSSetScissorRects, "RSSetScissorRects");
	HOOK(46, H_CopySubresourceRegion, o_CopySubresourceRegion, "CopySubresourceRegion");
	HOOK(47, H_CopyResource, o_CopyResource, "CopyResource");
	HOOK(49, H_CopyStructureCount, o_CopyStructureCount, "CopyStructureCount");
	HOOK(50, H_ClearRTV, o_ClearRTV, "ClearRenderTargetView");
	HOOK(51, H_ClearUAVUint, o_ClearUAVUint, "ClearUnorderedAccessViewUint");
	HOOK(52, H_ClearUAVFloat, o_ClearUAVFloat, "ClearUnorderedAccessViewFloat");
	HOOK(53, H_ClearDSV, o_ClearDSV, "ClearDepthStencilView");
	HOOK(54, H_GenerateMips, o_GenerateMips, "GenerateMips");
	HOOK(55, H_SetResourceMinLOD, o_SetResourceMinLOD, "SetResourceMinLOD");
	HOOK(57, H_Resolve, o_Resolve, "ResolveSubresource");
	HOOK(110, H_ClearState, o_ClearState, "ClearState");
	ComPtr<ID3D11DeviceContext1> context1;
	if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), &context1)) && context1) {
		void** vtable1 = *(void***)context1.Get();
#define HOOK1(index, hook, original, name) \
	total++;                               \
	hooked += HookFunction(vtable1[index], (void*)&hook, (void**)&original, name)
		HOOK1(117, H_DiscardResource, o_DiscardResource, "DiscardResource");
		HOOK1(118, H_DiscardView, o_DiscardView, "DiscardView");
		HOOK1(115, H_Ctx1<0>, o_Ctx1[0], "CopySubresourceRegion1");
		HOOK1(116, H_Ctx1<1>, o_Ctx1[1], "UpdateSubresource1");
		HOOK1(119, H_Ctx1<2>, o_Ctx1[2], "VSSetConstantBuffers1");
		HOOK1(123, H_Ctx1<3>, o_Ctx1[3], "PSSetConstantBuffers1");
		HOOK1(124, H_Ctx1<4>, o_Ctx1[4], "CSSetConstantBuffers1");
		HOOK1(132, H_Ctx1<5>, o_Ctx1[5], "ClearView");
		HOOK1(131, H_Ctx1<6>, o_Ctx1[6], "SwapDeviceContextState");
		HOOK1(122, H_Ctx1<7>, o_Ctx1[7], "GSSetConstantBuffers1");
#undef HOOK1
	}
#undef HOOK
	s_hooksReady = hooked == total;
	Log("synchronized: %d/%d extra context hooks in place%s (Synchronized=%d, F5 switches it)", hooked, total,
		s_hooksReady ? "" : "; synchronized mode unavailable", g_cfg.synchronized);
}

bool ReplayAvailable() { return s_hooksReady; }

namespace {
// Our own drawing at Present (depth reprojection): the game's state is saved and put back.
PipelineState s_modDrawState;
}  // namespace
void StateSaveTemp(ID3D11DeviceContext* c) { CapturePipelineState(c, s_modDrawState); }
void StateRestoreTemp(ID3D11DeviceContext* c) {
	ApplyPipelineState(c, s_modDrawState);
	ReleasePipelineState(s_modDrawState);
}

// Drops every reference the recorder holds on the game's objects. The swapchain can only be
// resized when nothing holds its back buffer: a saved pipeline state still holding its render
// target view makes ResizeBuffers fail and the game freezes.
void ReplayRelease() {
	if (!s_hooksReady) return;
	ExclusiveLock guard(&s_recordLock);
	g_recording = false;
	s_recordingValid = false;
	for (auto& o : s_openMaps) o.res = nullptr;
	ReleaseHeldRefs();
	s_arenaUsed = 0;
	ReleaseSnapshotViews();
	ReleasePipelineState(s_recordStartState);
	ReleasePipelineState(s_presentState);
	ReleasePipelineState(s_finalPassState);
}

// Right after Present (StereoBeginFrame has decided whether this frame is synchronized).
void ReplayFrameStart(ID3D11DeviceContext* c, bool synchronized) {
	if (!s_hooksReady) return;
	if (!synchronized) {
		ReplayRelease();
		return;
	}
	ReleasePipelineState(s_finalPassState);
	{
		ExclusiveLock guard(&s_recordLock);
		g_recording = false;
		for (auto& o : s_openMaps) o.res = nullptr;
		ReleaseHeldRefs();
		s_arenaUsed = 0;
		s_frameCommands = 0;
		s_uploadCopyMs = 0;
		memset(s_opCounts, 0, sizeof(s_opCounts));
		s_recordingValid = synchronized;
		s_cancelReason = nullptr;
		s_stoppedAtUi = false;
		s_generationCount = 0;
		for (int i = 0; i < s_poolCount; i++) s_pool[i].inUse = false;
		ReleaseSnapshotViews();
	}
	if (!synchronized) return;
	CapturePipelineState(c, s_recordStartState);
	g_recording = true;
}

// At the first UI draw (fromUi) or at Present.
void ReplayStop(bool fromUi) {
	if (!g_recording) return;
	ExclusiveLock guard(&s_recordLock);
	if (g_recording) {
		g_recording = false;
		s_stoppedAtUi = fromUi;
		for (int i = 0; i < s_generationCount; i++) CloseGeneration(s_generations[i]);
		for (auto& o : s_openMaps)
			if (o.res) {  // still mapped: the game writes it later, can't replay this frame
				s_recordingValid = false;
				s_cancelReason = "a resource stayed mapped across the end of the recording";
			}
	}
}

// The final pass (UI target -> backbuffer) as the game drew it, for the second eye's scene image.
void ReplayCaptureFinalPassState(ID3D11DeviceContext* c) { CapturePipelineState(c, s_finalPassState); }
void ReplayApplyFinalPassState(ID3D11DeviceContext* c) { ApplyPipelineState(c, s_finalPassState); }

// At Present, after the left eye's frame was captured: draw the frame again for the right eye.
// Returns true when the right eye's image is ready (in the UI split's scene texture, or in the
// backbuffer when the frame had no UI).
bool ReplaySecondEye(ID3D11DeviceContext* c, bool* inBackbuffer) {
	*inBackbuffer = false;
	if (!s_hooksReady) return false;
	ReplayStop(false);
	bool canReplay = s_recordingValid && s_arenaUsed > 0;
	s_statFrames++;
	if (!canReplay) {
		if (s_cancelReason) {
			s_statCancelled++;
			snprintf(s_lastCancelReason, sizeof(s_lastCancelReason), "%s", s_cancelReason);
		}
		return false;
	}
	double startMs = NowMs();
	g_replaying = true;
	CapturePipelineState(c, s_presentState);
	StereoBeginSecondEye();
	GpuTimingMark(c, "replay start");
	// //use deferred context instead of global context
	// ApplyPipelineState(g_replayCtx, s_recordStartState);
	// PlayRecording(g_replayCtx);
	// ID3D11CommandList* list = nullptr;
	// g_replayCtx->FinishCommandList(FALSE, &list);
	// c->ExecuteCommandList(list, TRUE);
	// list->Release();
	ApplyPipelineState(c, s_recordStartState);
	FoveatedSceneAgain();
	PlayRecording(c);
	FoveatedSceneEnd(c);
	GpuTimingMark(c, "replay end");
	bool patched = StereoSecondEyePatched(), ready = patched;
	if (ready && s_stoppedAtUi) {
		GpuTimingMark(c, "R final pass");
		ready = UiSecondEyeScene(c);
		if (!ready) snprintf(s_lastCancelReason, sizeof(s_lastCancelReason), "%s", "the final pass wasn't seen this frame");
	} else {
		*inBackbuffer = ready;
	}
	ApplyPipelineState(c, s_presentState);
	ReleasePipelineState(s_presentState);
	ReleasePipelineState(s_recordStartState);
	ReleasePipelineState(s_finalPassState);
	g_replaying = false;
	double replayMs = NowMs() - startMs;
	if (ready) s_statReplayed++;
	else if (!patched) s_statNoCamera++;
	else s_statCancelled++;
	s_statReplayMs += replayMs;
	if (replayMs > s_statReplayMaxMs) s_statReplayMaxMs = replayMs;
	s_statCopyMs += s_uploadCopyMs;
	s_statCommands += s_frameCommands;
	s_statBytes += (double)s_arenaUsed;
	s_statShadowDrawsSkipped += s_skippedShadowDraws;
	if (!s_loggedFirstFrame) {
		s_loggedFirstFrame = true;
		char opList[900];
		size_t used = 0;
		for (int i = 1; i < OP_COUNT && used + 48 < sizeof(opList); i++)
			if (s_opCounts[i]) used += snprintf(opList + used, sizeof(opList) - used, " %s %u,", kOpNames[i], s_opCounts[i]);
		Log("synchronized: first frame drawn twice: %u calls recorded (%u KB), %s;%s", s_frameCommands, (unsigned)(s_arenaUsed >> 10),
			s_stoppedAtUi ? "stopped at the first UI draw" : "frame without UI, recorded to Present", opList);
	}
	return ready;
}

// From the culling report (every 30 s, game thread).
void ReplayReport() {
	DWORD now = GetTickCount();
	if (now - s_lastReportTick < 30000) return;
	s_lastReportTick = now;
	if (!s_statFrames) return;
	uint32_t drawnTwice = s_statReplayed + s_statNoCamera;
	Log("synchronized: %u frames, %u drawn twice (%u without the camera found), %u cancelled%s%s; second eye %.1f ms "
		"of CPU time on average (max %.1f), recording copies %.2f ms, %.0f calls and %.0f KB per frame, %.0f shadow "
		"draws reused per frame",
		s_statFrames, s_statReplayed, s_statNoCamera, s_statCancelled, s_statCancelled ? ", last because: " : "",
		s_statCancelled ? s_lastCancelReason : "", drawnTwice ? s_statReplayMs / drawnTwice : 0.0, s_statReplayMaxMs,
		drawnTwice ? s_statCopyMs / drawnTwice : 0.0, drawnTwice ? s_statCommands / drawnTwice : 0.0, drawnTwice ? s_statBytes / drawnTwice / 1024 : 0.0, drawnTwice ? s_statShadowDrawsSkipped / drawnTwice : 0.0);
	{
		char menuLine[160];
		snprintf(menuLine, sizeof(menuLine), "Synchronized: %u of %u frames drawn twice, right eye %.1f ms CPU", s_statReplayed,
				 s_statFrames, drawnTwice ? s_statReplayMs / drawnTwice : 0.0);
		MenuSetInfo(INFO_SYNCHRONIZED, menuLine);
	}
	s_statFrames = s_statReplayed = s_statCancelled = s_statNoCamera = 0;
	s_statReplayMs = s_statReplayMaxMs = s_statCopyMs = s_statCommands = s_statBytes = s_statShadowDrawsSkipped = 0;
}
