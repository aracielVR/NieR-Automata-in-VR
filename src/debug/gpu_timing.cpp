// gpu_timing.cpp - every 5 s, one frame's GPU time pass by pass (timestamp queries), for the log and the Info page.
#include "debug/gpu_timing.h"
#include "core/com.h"
#include "core/config.h"
#include "core/log.h"
#include "ui/menu.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

namespace {
constexpr int32_t kMaxMarks = 320;
constexpr int32_t kPassesLogged = 16, kPassesShown = 5;
struct Mark {
	char label[56];
	uint32_t draws;
	uint32_t work[GPU_WORK_COUNT];
	double cpuMs;  // NowMs() at the mark
};

// "413 draws, 2 copies, 1 clear" (nothing: "no work counted")
void DescribeWork(const Mark& mark, char* out, size_t cap) {
	static const char* const kNames[GPU_WORK_COUNT] = {"copies", "clears", "compute", "mips/resolves"};
	size_t used = 0;
	out[0] = 0;
	if (mark.draws) used += (size_t)snprintf(out, cap, "%u draws", mark.draws);
	for (int32_t kind = 0; kind < GPU_WORK_COUNT && used < cap; kind++)
		if (mark.work[kind]) used += (size_t)snprintf(out + used, cap - used, "%s%u %s", used ? ", " : "", mark.work[kind], kNames[kind]);
	if (!used) snprintf(out, cap, "no work counted");
}
struct Frame {
	ID3D11Query* disjoint;
	ID3D11Query* timestamps[kMaxMarks];
	Mark marks[kMaxMarks];
	int32_t markCount;
	bool pending;
};
Frame s_frame;
bool s_active, s_initDone, s_failed;
DWORD s_lastStartTick;
ID3D11DeviceContext* s_context;
char s_lastLabel[56];
uint32_t s_depthKeys[4];

const char* DepthFormatName(DXGI_FORMAT format) {
	switch (format) {
		case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
		case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
		case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
		case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
		case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
		case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
		case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
		default: return "other";
	}
}

bool CreateQueries(ID3D11DeviceContext* c) {
	if (s_initDone) return !s_failed;
	s_initDone = true;
	ComPtr<ID3D11Device> device;
	c->GetDevice(&device);
	if (!device) return !(s_failed = true);
	D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
	HRESULT hr = device->CreateQuery(&desc, &s_frame.disjoint);
	desc.Query = D3D11_QUERY_TIMESTAMP;
	for (int i = 0; i < kMaxMarks && SUCCEEDED(hr); i++) hr = device->CreateQuery(&desc, &s_frame.timestamps[i]);
	if (FAILED(hr)) {
		s_failed = true;
		Log("gpu timing: queries unavailable (0x%08x)", (unsigned)hr);
		return false;
	}
	s_context = c;
	return true;
}

void DescribeTarget(ID3D11View* view, char* out, size_t cap, ID3D11DepthStencilView* dsv) {
	out[0] = 0;
	if (!view) return;
	ComPtr<ID3D11Resource> res;
	view->GetResource(&res);
	if (!res) return;
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		D3D11_TEXTURE2D_DESC desc;
		((ID3D11Texture2D*)res.Get())->GetDesc(&desc);
		snprintf(out, cap, "%ux%u f%d", desc.Width, desc.Height, (int)desc.Format);
		// depth buffer details, once per size (for the depth-based eye correction)
		if (dsv && (ID3D11View*)dsv == view && desc.Width >= 640) {
			uint32_t key = desc.Width * 7919u + desc.Height;
			int slot = -1;
			for (int i = 0; i < 4 && slot < 0; i++)
				if (s_depthKeys[i] == key) slot = -2;
			for (int i = 0; i < 4 && slot == -1; i++)
				if (!s_depthKeys[i]) slot = i;
			if (slot >= 0) {
				s_depthKeys[slot] = key;
				D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc;
				dsv->GetDesc(&viewDesc);
				Log("gpu timing: depth buffer %ux%u, format %d (%s), view format %d, %u sample(s), bind 0x%x%s",
					desc.Width, desc.Height, (int)desc.Format, DepthFormatName(desc.Format), (int)viewDesc.Format, desc.SampleDesc.Count,
					desc.BindFlags, (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ? " (shader-readable)" : " (not shader-readable)");
			}
		}
	}
}
}  // namespace

// At the start of the game's frame (right after Present).
void GpuTimingFrameStart(ID3D11DeviceContext* c) {
	if (s_active || s_frame.pending || GetTickCount() - s_lastStartTick < 5000) return;
	if (!CreateQueries(c)) return;
	s_lastStartTick = GetTickCount();
	s_active = true;
	s_frame.markCount = 0;
	s_lastLabel[0] = 0;
	c->Begin(s_frame.disjoint);
	GpuTimingMark(c, "frame start");
}

void GpuTimingMark(ID3D11DeviceContext* c, const char* label) {
	if (!s_active || c != s_context || s_frame.markCount >= kMaxMarks) return;
	Mark& mark = s_frame.marks[s_frame.markCount];
	snprintf(mark.label, sizeof(mark.label), "%s", label);
	mark.draws = 0;
	memset(mark.work, 0, sizeof(mark.work));
	mark.cpuMs = NowMs();
	c->End(s_frame.timestamps[s_frame.markCount]);
	s_frame.markCount++;
	snprintf(s_lastLabel, sizeof(s_lastLabel), "%s", label);
}

// A render-target change of the game (or of the replay, prefix "R ").
void GpuTimingTargets(ID3D11DeviceContext* c, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
					  const char* prefix) {
	if (!s_active || c != s_context) return;
	char targetText[40] = "", depthText[40] = "", label[56];
	ID3D11RenderTargetView* rtv0 = (count && rtvs) ? rtvs[0] : nullptr;
	DescribeTarget(rtv0, targetText, sizeof(targetText), nullptr);
	DescribeTarget(dsv, depthText, sizeof(depthText), dsv);
	if (rtv0)
		snprintf(label, sizeof(label), "%s%s%s%s", prefix, targetText, count > 1 ? " +MRT" : "", dsv ? " +depth" : "");
	else if (dsv)
		snprintf(label, sizeof(label), "%sdepth only %s", prefix, depthText);
	else
		snprintf(label, sizeof(label), "%sno target", prefix);
	GpuTimingMark(c, label);
}
// kind: 1 light lists, 2 GI volumes (probe selection), else another compute pass. Each kind gets its
// own mark so its time is measured on its own.
void GpuTimingCompute(ID3D11DeviceContext* c, const char* prefix, int kind) {
	if (!s_active || c != s_context) return;
	char label[56];
	snprintf(label, sizeof(label), "%scompute: %s", prefix, kind == 1 ? "light lists" : (kind == 2 ? "GI volumes" : "other"));
	if (strcmp(label, s_lastLabel) != 0) GpuTimingMark(c, label);
}
void GpuTimingDraw() {
	if (s_active && s_frame.markCount > 0) s_frame.marks[s_frame.markCount - 1].draws++;
}
void GpuTimingWork(ID3D11DeviceContext* c, GPU_WORK kind) {
	if (s_active && c == s_context && s_frame.markCount > 0) s_frame.marks[s_frame.markCount - 1].work[kind]++;
}

// At Present, after everything of this frame (including the replay) was submitted.
void GpuTimingFrameEnd(ID3D11DeviceContext* c) {
	if (!s_active || c != s_context) return;
	GpuTimingMark(c, "end");
	c->End(s_frame.disjoint);
	s_active = false;
	s_frame.pending = true;
}

// Each frame: once the GPU has the results, log them.
void GpuTimingPoll(ID3D11DeviceContext* c) {
	if (!s_frame.pending || c != s_context) return;
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
	if (c->GetData(s_frame.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
	uint64_t timestamps[kMaxMarks];
	for (int i = 0; i < s_frame.markCount; i++)
		if (c->GetData(s_frame.timestamps[i], &timestamps[i], sizeof(uint64_t), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
	s_frame.pending = false;
	if (disjoint.Disjoint || !disjoint.Frequency || s_frame.markCount < 2) return;
	double toMs = 1000.0 / (double)disjoint.Frequency;
	double giMs = 0;
	int giCount = 0;
	// segments: mark i lasts until mark i+1
	struct Seg {
		int32_t mark;
		double ms;
	} segs[kMaxMarks];
	int segCount = 0;
	double total = (double)(timestamps[s_frame.markCount - 1] - timestamps[0]) * toMs, left = 0, ui = 0, right = 0, handoff = 0, shadowR = 0;
	int phase = 0;  // 0 left scene, 1 UI and final, 2 the right eye (replay and its final pass); handoff: to the headset
	for (int i = 0; i + 1 < s_frame.markCount; i++) {
		const char* label = s_frame.marks[i].label;
		if (!strcmp(label, "UI start") || !strcmp(label, "present")) phase = 1;
		if (!strcmp(label, "replay start")) phase = 2;
		double ms = (double)(timestamps[i + 1] - timestamps[i]) * toMs;
		if (ms < 0) ms = 0;
		if (strstr(label, "to headset") || strstr(label, "upscale") || !strcmp(label, "depth reprojection")) handoff += ms;
		else if (phase == 0) left += ms;
		else if (phase == 1) ui += ms;
		else right += ms;
		if (phase == 2 && strstr(label, "reused")) shadowR += ms;
		if (strstr(label, "compute: GI volumes")) giMs += ms, giCount++;
		segs[segCount++] = {i, ms};
	}
	// slowest 10
	for (int x = 0; x < segCount; x++)
		for (int y = x + 1; y < segCount; y++)
			if (segs[y].ms > segs[x].ms) {
				Seg swap = segs[x];
				segs[x] = segs[y];
				segs[y] = swap;
			}
	double cpuTotal = s_frame.marks[s_frame.markCount - 1].cpuMs - s_frame.marks[0].cpuMs;
	Log("gpu timing: frame %.2f ms on the GPU (%.2f ms of CPU time between the first and last mark): left eye scene "
		"%.2f, UI + final %.2f, right eye %.2f (reused shadow passes in it %.2f), to the headset %.2f; GI volumes pass "
		"%.2f ms (%d x, GIVolumes=%d); %d passes",
		total, cpuTotal, left, ui, right, shadowR, handoff, giMs, giCount, g_cfg.giVolumes, s_frame.markCount - 1);
	char info[160];
	snprintf(info, sizeof(info), "GPU per frame %.1f ms: left scene %.1f, UI %.1f, right eye %.1f, to headset %.1f; GI pass %.2f ms",
			 total, left, ui, right, handoff, giMs);
	MenuSetInfo(INFO_GPU_TOTAL, info);
	for (int k = 0; k < segCount && k < kPassesLogged; k++) {
		const Mark& mark = s_frame.marks[segs[k].mark];
		// CPU time to the next mark: when it is as long as the GPU time, the GPU was waiting for the game
		double cpu = s_frame.marks[segs[k].mark + 1].cpuMs - mark.cpuMs;
		char work[96];
		DescribeWork(mark, work, sizeof(work));
		Log("gpu timing:   %6.2f ms  %-44s %s  (CPU %.2f ms)", segs[k].ms, mark.label, work, cpu);
		if (k < kPassesShown) {
			snprintf(info, sizeof(info), "  %.2f ms  %s (%s)", segs[k].ms, mark.label, work);
			MenuSetInfo(INFO_GPU_PASSES + k, info);
		}
	}
}
