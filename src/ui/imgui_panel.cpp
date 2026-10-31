// imgui_panel.cpp - Dear ImGui panels (the menu, the stats overlay) drawn on the VR thread's device.
#include "ui/imgui_panel.h"
#include "core/log.h"
#include <stdio.h>
#include <d3dcompiler.h>
#include "backends/imgui_impl_dx11.h"
#include "imgui.h"

// ImGui's DX11 backend calls D3DCompile for its two small shaders. Defined here so the DLL doesn't
// import d3dcompiler_47.dll at load time: it's loaded on first use, like xr_device.cpp does.
HRESULT WINAPI D3DCompile(const void* data, SIZE_T size, const char* name, const D3D_SHADER_MACRO* defines,
						  ID3DInclude* include, const char* entry, const char* target, UINT flags1, UINT flags2,
						  ID3DBlob** code, ID3DBlob** errors) {
	static pD3DCompile real;
	if (!real) {
		HMODULE compilerDll = LoadLibraryW(L"d3dcompiler_47.dll");
		real = compilerDll ? (pD3DCompile)(void*)GetProcAddress(compilerDll, "D3DCompile") : nullptr;
		if (!real) {
			Log("ui: d3dcompiler_47.dll not available (error %u)", (unsigned)GetLastError());
			return E_FAIL;
		}
	}
	return real(data, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}

namespace {
ImVec4 ToImVec4(const float* color) { return ImVec4(color[0], color[1], color[2], color[3]); }
ImVec4 ToImVec4(const float* color, float alpha) { return ImVec4(color[0], color[1], color[2], alpha); }

void LoadFont(ImGuiIO& io, float size) {
	char path[MAX_PATH];
	UINT length = GetWindowsDirectoryA(path, MAX_PATH - 32);
	if (length && length < MAX_PATH - 32) {
		snprintf(path + length, MAX_PATH - length, "\\Fonts\\segoeui.ttf");
		if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && io.Fonts->AddFontFromFileTTF(path, size)) return;
	}
	io.Fonts->AddFontDefault();
}

void ApplyStyle(float fontSize) {
	using ui::Colors;
	ImGuiStyle& style = ImGui::GetStyle();
	style.FontSizeBase = fontSize;
	style.WindowRounding = 6;
	style.WindowBorderSize = 0;
	style.FrameRounding = 4;
	style.GrabRounding = 4;
	style.GrabMinSize = fontSize * 0.2f;
	style.FramePadding = ImVec2(fontSize * 0.4f, fontSize * 0.2f);
	style.ItemSpacing = ImVec2(fontSize * 0.4f, fontSize * 0.3f);
	style.CellPadding = ImVec2(fontSize * 0.3f, fontSize * 0.25f);
	style.ScrollbarSize = fontSize * 0.5f;
	ImVec4* colors = style.Colors;
	colors[ImGuiCol_Text] = ToImVec4(Colors::text);
	colors[ImGuiCol_TextDisabled] = ToImVec4(Colors::dim);
	colors[ImGuiCol_WindowBg] = ToImVec4(Colors::bg);
	colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
	colors[ImGuiCol_Separator] = ToImVec4(Colors::bar);
	colors[ImGuiCol_FrameBg] = ToImVec4(Colors::bar);
	colors[ImGuiCol_FrameBgHovered] = ToImVec4(Colors::bar);
	colors[ImGuiCol_FrameBgActive] = ToImVec4(Colors::accent, 0.35f);
	colors[ImGuiCol_SliderGrab] = ToImVec4(Colors::accent, 0.55f);
	colors[ImGuiCol_SliderGrabActive] = ToImVec4(Colors::accent, 0.9f);  // not white: the value is written over it
	colors[ImGuiCol_CheckMark] = ToImVec4(Colors::accent);
	colors[ImGuiCol_CheckboxSelectedBg] = ToImVec4(Colors::bar);
	colors[ImGuiCol_Button] = ToImVec4(Colors::bar);
	colors[ImGuiCol_ButtonHovered] = ToImVec4(Colors::bar);
	colors[ImGuiCol_ButtonActive] = ToImVec4(Colors::accent, 0.5f);
	colors[ImGuiCol_Header] = ToImVec4(Colors::bar);
	colors[ImGuiCol_HeaderHovered] = ToImVec4(Colors::bar);
	colors[ImGuiCol_HeaderActive] = ToImVec4(Colors::bar);
	colors[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
	colors[ImGuiCol_TabHovered] = ToImVec4(Colors::bar);
	colors[ImGuiCol_TabSelected] = ToImVec4(Colors::accent, 0.85f);
	colors[ImGuiCol_TabSelectedOverline] = ToImVec4(Colors::accent);
	colors[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
	colors[ImGuiCol_TabDimmedSelected] = ToImVec4(Colors::accent, 0.85f);
	colors[ImGuiCol_PlotLines] = ToImVec4(Colors::accent);
	colors[ImGuiCol_NavCursor] = ToImVec4(Colors::accent);
	colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
	colors[ImGuiCol_ScrollbarGrab] = ToImVec4(Colors::bar);
	colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
	colors[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0);
}

bool CreatePanel(UiPanel& panel, ID3D11Device* device, ID3D11DeviceContext* context) {
	panel.ctx = ImGui::CreateContext();
	ImGui::SetCurrentContext(panel.ctx);
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	if (panel.navigation) {
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	}
	LoadFont(io, panel.fontSize);
	ApplyStyle(panel.fontSize);
	if (!ImGui_ImplDX11_Init(device, context)) {
		Log("ui: %s: ImGui DX11 backend could not start", panel.name);
		ImGui::DestroyContext(panel.ctx);
		panel.ctx = nullptr;
		return false;
	}
	panel.lastMs = NowMs();
	Log("ui: %s: Dear ImGui %s", panel.name, IMGUI_VERSION);
	return true;
}
}  // namespace

bool UiPrepare(UiPanel& panel, ID3D11Device* device, ID3D11DeviceContext* context, int width, int height) {
	if (panel.failed) return false;
	if (!panel.ctx && !CreatePanel(panel, device, context)) {
		panel.failed = true;
		return false;
	}
	ImGui::SetCurrentContext(panel.ctx);
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)width, (float)height);
	double now = NowMs();
	float dt = (float)((now - panel.lastMs) / 1000);
	io.DeltaTime = dt > 0 && dt < 1 ? dt : 1.0f / 60;
	panel.lastMs = now;
	return true;
}

void UiNewFrame() {
	ImGui_ImplDX11_NewFrame();
	ImGui::NewFrame();
}

// Ends the frame and draws it into rtv, cleared to fully see-through first.
void UiRender(ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv) {
	ImGui::Render();
	const float clear[4] = {0, 0, 0, 0};
	context->ClearRenderTargetView(rtv, clear);
	context->OMSetRenderTargets(1, &rtv, nullptr);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	context->OMSetRenderTargets(0, nullptr, nullptr);
}
