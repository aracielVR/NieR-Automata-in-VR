// imgui_panel.h - a Dear ImGui panel drawn into a render target (imgui_panel.cpp).
#pragma once
#include <d3d11.h>

struct ImGuiContext;

namespace ui {
struct Colors {
	static constexpr float bg[4] = {0.118f, 0.114f, 0.102f, 1};       // dark warm grey
	static constexpr float text[4] = {0.855f, 0.831f, 0.733f, 1};     // beige
	static constexpr float dim[4] = {0.588f, 0.569f, 0.502f, 1};
	static constexpr float accent[4] = {0.769f, 0.659f, 0.416f, 1};   // gold
	static constexpr float bar[4] = {0.275f, 0.263f, 0.235f, 1};
	static constexpr float warn[4] = {0.839f, 0.471f, 0.353f, 1};     // "(restart)"
};
}  // namespace ui

struct UiPanel {
	const char* name;
	float fontSize;
	bool navigation;
	ImGuiContext* ctx;
	bool failed;
	double lastMs;  // NowMs() of the last frame (ImGui wants the time between frames)
};

bool UiPrepare(UiPanel& p, ID3D11Device* dev, ID3D11DeviceContext* dc, int w, int h);
void UiNewFrame();
void UiRender(ID3D11DeviceContext* dc, ID3D11RenderTargetView* rtv);
