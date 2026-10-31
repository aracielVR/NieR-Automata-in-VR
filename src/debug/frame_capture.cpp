// frame_capture.cpp - [Debug] F11: records the next frame's D3D11 activity to NieRVR_capture_N.bin (tools/analyze_capture.py).
#include "debug/frame_capture.h"
#include "core/com.h"
#include "core/locks.h"
#include "core/config.h"
#include "core/log.h"
#include "core/main.h"
#include "core/pure.h"
#include "render/texture_tags.h"
#include <atomic>
#include <stdio.h>
#include <string.h>

namespace {
enum RecordType : uint16_t {
	R_FRAME_BEGIN = 1,  // u32 frame, u32 0, u32 0
	R_SET_CB = 2,       // u8 stage, u8 pad[3], u32 start, u32 count, u64 bufs[count]
	R_CB_DATA = 3,      // u64 buf, u32 byteWidth, u8 via(0 map,1 update,2 readback), u8 pad[3], data
	R_SET_SHADER = 4,   // u8 stage, pad[7], u64 shader
	R_DRAW = 5,         // u8 kind(0 Draw,1 Indexed,2 Inst,3 IndexedInst), pad[3], u32 a, u32 b, u32 c
	R_SET_RT = 6,       // u32 numRTV, u32 w, u32 h, u32 fmt, u64 dsv, u64 rtv0
	R_VIEWPORT = 7,     // u32 n, then n * 6 floats
	R_SHADER_BLOB = 8,  // u8 stage, pad[7], u64 shader, u64 hash, bytecode
	R_DISPATCH = 9,     // u32 x, y, z
	R_EXEC_CMDLIST = 10,
	R_FRAME_END = 12,
	R_SET_SRV = 13,  // u8 stage, pad[3], u32 start, u32 count, count * {u64 res, u32 w, u32 h, u32 fmt, u32 dim}
	R_SET_UAV = 14,  // same layout as R_SET_SRV (stage = CS)
	R_BUF_DATA = 16,  // u64 buf, u32 offset, u32 len, data: part of a shader-resource buffer written by UpdateSubresource
	R_TEX_HASH = 15,  // u64 res, u32 hash (3Dmigoto's texture hash, render/texture_tags.cpp), u32 0: after R_SET_SRV
};
static_assert(sizeof(D3D11_VIEWPORT) == 24, "R_VIEWPORT layout");

struct RecordHeader {
	uint16_t type;
	uint16_t pad;
	uint32_t size;
};

ID3D11Device* s_device;
ID3D11DeviceContext* s_context;
std::atomic<bool> s_capturing;
bool s_armed;
std::atomic<bool> s_requested;  // CaptureRequest
std::atomic<int32_t> s_requestCount;
std::atomic<DWORD> s_lastRequestTick;
bool s_keyWasDown;
uint32_t s_captureIndex;
SRWLOCK s_recordLock = SRWLOCK_INIT;
uint8_t* s_records;
size_t s_recordsUsed, s_recordsCap;
bool s_overflow;

void EmitRecord(uint16_t type, const void* head, uint32_t headLen, const void* body = nullptr, uint32_t bodyLen = 0) {
	ExclusiveLock guard(&s_recordLock);
	size_t total = sizeof(RecordHeader) + headLen + bodyLen;
	if (!s_records || s_recordsUsed + total > s_recordsCap) {
		s_overflow = true;
	} else {
		RecordHeader header = {type, 0, headLen + bodyLen};
		memcpy(s_records + s_recordsUsed, &header, sizeof(header));
		memcpy(s_records + s_recordsUsed + sizeof(header), head, headLen);
		if (bodyLen) memcpy(s_records + s_recordsUsed + sizeof(header) + headLen, body, bodyLen);
		s_recordsUsed += total;
	}
}

// Constant buffers seen this frame: kept alive (AddRef) until the frame is written.
struct SeenBuffer {
	ID3D11Buffer* buf;
	bool hasData;
};
constexpr int32_t kMaxSeen = 8192;
SeenBuffer s_seen[kMaxSeen];
int32_t s_seenCount;

SeenBuffer* FindSeen(ID3D11Buffer* buffer) {
	for (int i = 0; i < s_seenCount; i++)
		if (s_seen[i].buf == buffer) return &s_seen[i];
	return nullptr;
}
void AddSeen(ID3D11Buffer* buffer, bool withData) {
	if (!buffer) return;
	ExclusiveLock guard(&s_recordLock);
	SeenBuffer* seen = FindSeen(buffer);
	if (!seen && s_seenCount < kMaxSeen) {
		seen = &s_seen[s_seenCount++];
		seen->buf = buffer;
		seen->hasData = false;
		buffer->AddRef();
	}
	if (seen && withData) seen->hasData = true;
}

// Buffers currently mapped (Map .. Unmap) during the capture.
struct MappedBuffer {
	ID3D11Resource* res;
	void* data;
	UINT size;
};
MappedBuffer s_mapped[128];

void EmitCbData(ID3D11Buffer* buf, UINT byteWidth, uint8_t via, const void* data, UINT len) {
	struct {
		uint64_t buf;
		uint32_t byteWidth;
		uint8_t via, pad[3];
	} header = {(uint64_t)(uintptr_t)buf, byteWidth, via, {0, 0, 0}};
	static_assert(sizeof(header) == 16, "R_CB_DATA layout");
	EmitRecord(R_CB_DATA, &header, sizeof(header), data, len);
	AddSeen(buf, true);
}

// Shader-resource / UAV bindings: which textures and buffers each pass reads and writes, so data
// flow between passes (e.g. compute passes) can be followed offline.
struct ViewRecord {
	uint64_t res;
	uint32_t w, h, fmt, dim;
};
static_assert(sizeof(ViewRecord) == 24, "ViewRecord layout");
template <typename View>
void DescribeView(View* view, ViewRecord* record) {
	*record = {};
	if (!view) return;
	ComPtr<ID3D11Resource> res;
	view->GetResource(&res);
	if (!res) return;
	record->res = (uint64_t)(uintptr_t)res.Get();
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	record->dim = (uint32_t)dim;
	if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		D3D11_TEXTURE2D_DESC desc;
		((ID3D11Texture2D*)res.Get())->GetDesc(&desc);
		record->w = desc.Width;
		record->h = desc.Height;
		record->fmt = desc.Format;
	} else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
		D3D11_BUFFER_DESC desc;
		((ID3D11Buffer*)res.Get())->GetDesc(&desc);
		record->w = desc.ByteWidth;
		record->h = desc.StructureByteStride;
	}
}
// NieR binds pixel shader resources many slots at a time (more than 16), so long calls go out in
// chunks of 16, without the trailing empty slots.
template <typename View>
void EmitViews(uint16_t type, uint8_t stage, UINT start, UINT count, View* const* views) {
	if (!views) return;
	if (count > 128) count = 128;
	while (count > 0 && !views[count - 1]) count--;
	for (UINT base = 0; base < count; base += 16) {
		UINT chunk = count - base < 16 ? count - base : 16;
		struct {
			uint8_t stage, pad[3];
			uint32_t start, count;
		} header = {stage, {0, 0, 0}, start + base, chunk};
		static_assert(sizeof(header) == 12, "R_SET_SRV layout");
		ViewRecord records[16];
		for (UINT i = 0; i < chunk; i++) DescribeView(views[base + i], &records[i]);
		EmitRecord(type, &header, sizeof(header), records, chunk * sizeof(ViewRecord));
		for (UINT i = 0; i < chunk; i++)  // buffers: their contents are read back at the end of the frame
			if (type == R_SET_SRV && records[i].dim == D3D11_RESOURCE_DIMENSION_BUFFER)
				AddSeen((ID3D11Buffer*)(uintptr_t)records[i].res, false);
		// which texture each one is, so an effect's draws can be told apart by their textures (lens flare)
		for (UINT i = 0; i < chunk; i++) {
			struct {
				uint64_t res;
				uint32_t hash, pad;
			} tag = {records[i].res, 0, 0};
			if (records[i].dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && TextureTagsHash((ID3D11Resource*)(uintptr_t)records[i].res, &tag.hash))
				EmitRecord(R_TEX_HASH, &tag, sizeof(tag), nullptr, 0);
		}
	}
}

// --- frame end: readback, shader blobs, file, summary ---------------------------------------
void ReadbackUnseenBuffers(int* readbacks) {
	for (int i = 0; i < s_seenCount; i++) {
		SeenBuffer& seen = s_seen[i];
		if (seen.hasData) continue;
		D3D11_BUFFER_DESC desc;
		seen.buf->GetDesc(&desc);
		bool constant = (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
		if (desc.ByteWidth > (constant ? 65536u : 16u << 20)) continue;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		ComPtr<ID3D11Buffer> staging;
		if (FAILED(s_device->CreateBuffer(&desc, nullptr, &staging))) continue;
		s_context->CopyResource(staging.Get(), seen.buf);
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (SUCCEEDED(s_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
			EmitCbData(seen.buf, desc.ByteWidth, 2, mapped.pData, desc.ByteWidth);
			s_context->Unmap(staging.Get(), 0);
			(*readbacks)++;
		}
	}
}

void EmitShaderBlob(ShaderStage stage, void* shader, uint64_t hash, const uint8_t* blob, uint32_t len) {
	struct {
		uint8_t stage, pad[7];
		uint64_t sh, hash;
	} header = {stage, {0}, (uint64_t)(uintptr_t)shader, hash};
	static_assert(sizeof(header) == 24, "R_SHADER_BLOB layout");
	EmitRecord(R_SHADER_BLOB, &header, sizeof(header), blob, len);
}

// Logs the projection matrices found in this frame's constant buffers.
void Summarise() {
	struct Candidate {
		uint64_t buf;
		uint32_t offset;
		pure::ProjInfo p;
		uint32_t draws;
	} candidates[24];
	int candidateCount = 0;
	uint32_t draws = 0, cbData = 0, targetChanges = 0, dispatches = 0;
	for (size_t pos = 0; pos + sizeof(RecordHeader) <= s_recordsUsed;) {
		RecordHeader header;
		memcpy(&header, s_records + pos, sizeof(header));
		const uint8_t* p = s_records + pos + sizeof(header);
		if (header.type == R_DRAW) draws++;
		if (header.type == R_SET_RT) targetChanges++;
		if (header.type == R_DISPATCH) dispatches++;
		if (header.type == R_CB_DATA && header.size >= 16) {
			cbData++;
			uint64_t buf;
			memcpy(&buf, p, 8);
			const uint8_t* data = p + 16;
			uint32_t len = header.size - 16;
			for (uint32_t offset = 0; offset + 64 <= len && candidateCount < 24; offset += 16) {
				float matrix[16];
				memcpy(matrix, data + offset, 64);
				pure::ProjInfo proj;
				if (!pure::classify_projection(matrix, &proj)) continue;
				bool dup = false;
				for (int i = 0; i < candidateCount; i++)
					if (candidates[i].buf == buf && candidates[i].offset == offset) dup = true;
				if (!dup) candidates[candidateCount++] = {buf, offset, proj, 0};
			}
		}
		pos += sizeof(header) + header.size;
	}
	// how many draws had each candidate buffer bound (any stage)
	uint64_t bound[4][14] = {};
	for (size_t pos = 0; pos + sizeof(RecordHeader) <= s_recordsUsed;) {
		RecordHeader header;
		memcpy(&header, s_records + pos, sizeof(header));
		const uint8_t* p = s_records + pos + sizeof(header);
		if (header.type == R_SET_CB) {
			uint8_t stage = p[0];
			uint32_t start, count;
			memcpy(&start, p + 4, 4);
			memcpy(&count, p + 8, 4);
			for (uint32_t i = 0; i < count && stage < 4 && start + i < 14; i++) memcpy(&bound[stage][start + i], p + 12 + i * 8, 8);
		} else if (header.type == R_DRAW) {
			for (int i = 0; i < candidateCount; i++) {
				bool used = false;
				for (int s = 0; s < 4 && !used; s++)
					for (int k = 0; k < 14 && !used; k++) used = bound[s][k] == candidates[i].buf;
				if (used) candidates[i].draws++;
			}
		}
		pos += sizeof(header) + header.size;
	}
	Log("capture: frame had %u draws, %u dispatches, %u render-target changes, %u constant-buffer uploads%s", draws,
		dispatches, targetChanges, cbData, s_overflow ? " (CAPTURE TRUNCATED: buffer full)" : "");
	if (!candidateCount) Log("capture: no perspective projection matrix found in constant buffers this frame");
	for (int i = 0; i < candidateCount; i++) {
		const pure::ProjInfo& p = candidates[i].p;
		Log("capture: projection in cbuffer %p +%u: %s-major, vfov %.2f deg, aspect %.4f, off-centre (%.4f, %.4f), "
			"z = %.6f + %.6f/z, w sign %.0f, used by %u draws",
			(void*)(uintptr_t)candidates[i].buf, candidates[i].offset, p.rowMajor ? "row" : "column", (double)p.vfovDeg,
			(double)p.aspect, (double)p.cx, (double)p.cy, (double)p.A, (double)p.B, (double)p.wSign, candidates[i].draws);
	}
}

void WriteCapture() {
	wchar_t path[MAX_PATH];
	lstrcpynW(path, g_modDir, MAX_PATH - 40);
	char name[48];
	snprintf(name, sizeof(name), "NieRVR_capture_%u.bin", s_captureIndex);
	int length = lstrlenW(path);
	for (int i = 0; name[i] && length < MAX_PATH - 1; i++) path[length++] = (wchar_t)name[i];
	path[length] = 0;
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		Log("ERROR: capture: cannot write %s (error %u)", name, (unsigned)GetLastError());
		return;
	}
	const char magic[8] = {'N', 'V', 'R', 'C', 'A', 'P', '0', '1'};
	DWORD written;
	WriteFile(file, magic, 8, &written, nullptr);
	for (size_t pos = 0; pos < s_recordsUsed;) {
		DWORD chunk = (DWORD)(s_recordsUsed - pos > (16u << 20) ? (16u << 20) : s_recordsUsed - pos);
		if (!WriteFile(file, s_records + pos, chunk, &written, nullptr)) break;
		pos += chunk;
	}
	CloseHandle(file);
	Log("capture: wrote %s (%u KB)", name, (unsigned)((s_recordsUsed + 8) >> 10));
}

void FinishCapture() {
	s_capturing = false;
	int readbacks = 0;
	ReadbackUnseenBuffers(&readbacks);
	int blobs = ShaderTableTakeUsed(EmitShaderBlob);
	uint32_t frameEnd[3] = {s_captureIndex, 0, 0};
	EmitRecord(R_FRAME_END, frameEnd, 12);
	Summarise();
	Log("capture: %d constant buffers read back, %d shaders included, %u shaders known", readbacks, blobs,
		ShaderTableCount());
	WriteCapture();
	AcquireSRWLockExclusive(&s_recordLock);  // a late hook call on another thread must see s_buf == null
	uint8_t* records = s_records;
	s_records = nullptr;
	s_recordsUsed = s_recordsCap = 0;
	for (int i = 0; i < s_seenCount; i++) s_seen[i].buf->Release();
	s_seenCount = 0;
	for (auto& e : s_mapped) e.res = nullptr;
	ReleaseSRWLockExclusive(&s_recordLock);
	VirtualFree(records, 0, MEM_RELEASE);
}
}  // namespace

void CaptureSetDevice(ID3D11Device* device, ID3D11DeviceContext* context) {
	s_device = device;
	s_context = context;
}

bool CaptureRecording(ID3D11DeviceContext* c) { return s_capturing && c == s_context; }

void CaptureBeforePresent() {
	if (!s_context) return;
	if (s_capturing) FinishCapture();
	// hotkey (edge-triggered, only while the game window has focus)
	int vk = g_cfg.probe ? g_cfg.keyCapture : 0;
	if (vk > 0 && vk < 256) {
		bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		bool modified = (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_SHIFT) & 0x8000);  // Ctrl/Shift+key are other hotkeys
		if (down && !s_keyWasDown && !modified) {
			HWND window = GetForegroundWindow();
			DWORD pid = 0;
			if (window) GetWindowThreadProcessId(window, &pid);
			if (pid == GetCurrentProcessId()) s_armed = true;
		}
		s_keyWasDown = down;
	}
}

void CaptureRequest(const char* why) {
	DWORD now = GetTickCount();
	if (!g_cfg.probe || s_requestCount >= 3 || (s_lastRequestTick && now - s_lastRequestTick < 10000)) return;
	s_lastRequestTick = now;
	++s_requestCount;
	Log("capture: taking one by itself (%s)", why);
	s_requested = true;
}

void CaptureAfterPresent() {
	if (s_requested.exchange(false)) s_armed = true;
	if (!s_armed || s_capturing) return;
	s_armed = false;
	s_recordsCap = 256u << 20;  // reserved+committed lazily by the OS as it is touched
	s_records = (uint8_t*)VirtualAlloc(nullptr, s_recordsCap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!s_records) {
		Log("ERROR: capture: could not allocate capture memory");
		s_recordsCap = 0;
		return;
	}
	s_recordsUsed = 0;
	s_overflow = false;
	s_captureIndex++;
	uint32_t frameBegin[3] = {s_captureIndex, 0, 0};
	EmitRecord(R_FRAME_BEGIN, frameBegin, 12);
	Log("capture: capturing frame (capture %u)", s_captureIndex);
	s_capturing = true;
}

void CaptureSetConstantBuffers(ShaderStage stage, UINT start, UINT count, ID3D11Buffer* const* buffers) {
	if (!buffers || count > 14) return;
	struct {
		uint8_t stage, pad[3];
		uint32_t start, count;
	} header = {(uint8_t)stage, {0, 0, 0}, start, count};
	static_assert(sizeof(header) == 12, "R_SET_CB layout");
	uint64_t ptrs[14];
	for (UINT i = 0; i < count; i++) {
		ptrs[i] = (uint64_t)(uintptr_t)buffers[i];
		AddSeen(buffers[i], false);
	}
	EmitRecord(R_SET_CB, &header, sizeof(header), ptrs, count * 8);
}

void CaptureSetShader(ShaderStage stage, void* shader) {
	struct {
		uint8_t stage, pad[7];
		uint64_t sh;
	} header = {(uint8_t)stage, {0}, (uint64_t)(uintptr_t)shader};
	static_assert(sizeof(header) == 16, "R_SET_SHADER layout");
	EmitRecord(R_SET_SHADER, &header, sizeof(header));
	ShaderTableMarkUsed(shader);
}

void CaptureDraw(uint8_t kind, UINT arg0, UINT arg1, UINT arg2) {
	struct {
		uint8_t kind, pad[3];
		uint32_t arg0, arg1, arg2;
	} header = {kind, {0, 0, 0}, arg0, arg1, arg2};
	static_assert(sizeof(header) == 16, "R_DRAW layout");
	EmitRecord(R_DRAW, &header, sizeof(header));
}

void CaptureMap(ID3D11Resource* buffer, void* data, UINT size) {
	ExclusiveLock guard(&s_recordLock);
	for (auto& e : s_mapped)
		if (!e.res) {
			e = {buffer, data, size};
			break;
		}
}

void CaptureUnmap(ID3D11Resource* buffer) {
	MappedBuffer found = {};
	{
		ExclusiveLock guard(&s_recordLock);
		for (auto& e : s_mapped)
			if (e.res == buffer) {
				found = e;
				e.res = nullptr;
				break;
			}
	}
	if (found.res && found.data) EmitCbData((ID3D11Buffer*)buffer, found.size, 0, found.data, found.size);
}

void CaptureUpdate(ID3D11Resource* buffer, const void* data, UINT size) {
	EmitCbData((ID3D11Buffer*)buffer, size, 1, data, size);
}

void CaptureBufferUpdate(ID3D11Resource* buffer, UINT offset, const void* data, UINT size) {
	struct {
		uint64_t buf;
		uint32_t offset, len;
	} header = {(uint64_t)(uintptr_t)buffer, offset, size};
	static_assert(sizeof(header) == 16, "R_BUF_DATA layout");
	EmitRecord(R_BUF_DATA, &header, sizeof(header), data, size);
	AddSeen((ID3D11Buffer*)buffer, true);
}

void CaptureSetRenderTargets(UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
	struct {
		uint32_t count, w, h, fmt;
		uint64_t dsv, rtv0;
	} record = {count, 0, 0, 0, (uint64_t)(uintptr_t)dsv, 0};
	static_assert(sizeof(record) == 32, "R_SET_RT layout");
	ID3D11View* view = (count && rtvs && rtvs[0]) ? (ID3D11View*)rtvs[0] : (ID3D11View*)dsv;
	if (count && rtvs) record.rtv0 = (uint64_t)(uintptr_t)rtvs[0];
	if (view) {
		ComPtr<ID3D11Resource> res;
		view->GetResource(&res);
		D3D11_RESOURCE_DIMENSION dim;
		if (res) {
			res->GetType(&dim);
			if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
				D3D11_TEXTURE2D_DESC desc;
				((ID3D11Texture2D*)res.Get())->GetDesc(&desc);
				record.w = desc.Width;
				record.h = desc.Height;
				record.fmt = desc.Format;
			}
			// record resources (not views), so render targets can be matched with later SRV reads
			if (count && rtvs && rtvs[0]) record.rtv0 = (uint64_t)(uintptr_t)res.Get();
		}
	}
	if (dsv) {
		ComPtr<ID3D11Resource> depthRes;
		dsv->GetResource(&depthRes);
		if (depthRes) {
			record.dsv = (uint64_t)(uintptr_t)depthRes.Get();
		}
	}
	EmitRecord(R_SET_RT, &record, sizeof(record));
}

void CaptureViewports(UINT count, const D3D11_VIEWPORT* viewports) {
	if (!viewports || count > 16) return;
	uint32_t count32 = count;
	EmitRecord(R_VIEWPORT, &count32, 4, viewports, count * sizeof(D3D11_VIEWPORT));
}

void CaptureExecuteCommandList() {
	uint32_t zero = 0;
	EmitRecord(R_EXEC_CMDLIST, &zero, 4);
}

void CaptureShaderResources(ShaderStage stage, UINT start, UINT count, ID3D11ShaderResourceView* const* views) {
	EmitViews(R_SET_SRV, stage, start, count, views);
}

void CaptureUnorderedAccessViews(UINT start, UINT count, ID3D11UnorderedAccessView* const* views) {
	EmitViews(R_SET_UAV, ST_CS, start, count, views);
}

void CaptureDispatch(UINT x, UINT y, UINT z) {
	uint32_t groups[3] = {x, y, z};
	EmitRecord(R_DISPATCH, groups, 12);
}
