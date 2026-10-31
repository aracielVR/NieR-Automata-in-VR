#include "ui/stats_overlay.h"
#include <stdio.h>
#include "core/log.h"
#include "ui/imgui_panel.h"
#include "imgui.h"

namespace {
UiPanel s_panel = {"stats overlay", 24.0f, false};

OverlayNumbers s_numbers;
double s_lastFrameMs;  // NowMs() of the last headset frame (for its frame time)
double s_drawnMs, s_lastCheckMs;

// Frames counted since startMs; every half second they become the frame rates in s_numbers.
struct FrameCounts {
	uint32_t headset, gameLeft, gameRight, gameFlat;
	double startMs;
};
FrameCounts s_counts;

// The frame rates since counting started, into s_numbers; then counting starts again.
void TakeFrameRates(double now) {
	double secs = (now - s_counts.startMs) / 1000;
	s_numbers.headsetFps = (float)(s_counts.headset / secs);
	s_numbers.gameFpsLeft = (float)(s_counts.gameLeft / secs);
	s_numbers.gameFpsRight = (float)(s_counts.gameRight / secs);
	s_numbers.gameFpsFlat = (float)(s_counts.gameFlat / secs);
	s_counts = {};
	s_counts.startMs = now;
}

// "L  1.2 m/s  340 deg/s    accel 25 m/s2  4100 deg/s2"; a "-" for each number not known
void HandLine(const char* name, const OverlayHand& hand, const ImVec4& dim) {
	if (hand.speed < 0) {
		ImGui::TextColored(dim, "%s  not tracked", name);
		return;
	}
	char spin[16], accel[16], spinAccel[16];
	if (hand.spin >= 0) snprintf(spin, sizeof(spin), "%.0f", (double)hand.spin);
	else snprintf(spin, sizeof(spin), "-");
	if (hand.accel >= 0) snprintf(accel, sizeof(accel), "%.0f", (double)hand.accel);
	else snprintf(accel, sizeof(accel), "-");
	if (hand.spinAccel >= 0) snprintf(spinAccel, sizeof(spinAccel), "%.0f", (double)hand.spinAccel);
	else snprintf(spinAccel, sizeof(spinAccel), "-");
	ImGui::Text("%s  %.1f m/s  %s deg/s    accel %s m/s2  %s deg/s2", name, (double)hand.speed, spin, accel, spinAccel);
}

void Contents() {
	const OverlayNumbers& n = s_numbers;
	const ImVec4 title(ui::Colors::accent[0], ui::Colors::accent[1], ui::Colors::accent[2], 1);
	const ImVec4 dim(ui::Colors::dim[0], ui::Colors::dim[1], ui::Colors::dim[2], 1);
	ImGui::TextColored(title, "NieRVR " NIERVR_VERSION);
	ImGui::SameLine();
	ImGui::TextColored(dim, "  %s", n.mode);
	ImGui::Text("Headset %.0f fps    game L %.0f  R %.0f  flat %.0f", n.headsetFps, n.gameFpsLeft, n.gameFpsRight, n.gameFpsFlat);
	if (n.sceneW && (n.sceneW != n.gameW || n.sceneH != n.gameH))
		ImGui::Text("Game image %ux%u, upscaled to %ux%u", n.gameW, n.gameH, n.sceneW, n.sceneH);
	else
		ImGui::Text("Game image %ux%u", n.gameW, n.gameH);
	if (n.upscaler[0]) ImGui::TextColored(dim, "%s", n.upscaler);
	if (n.sharpX > 0)
		ImGui::Text("Sharpness %.0f%% across, %.0f%% up-down", n.sharpX, n.sharpY);
	else
		ImGui::TextColored(dim, "Sharpness: in stereo");
	if (n.poseDelayMs >= 0)
		ImGui::Text("Pose delay %.0f ms, predicting %.0f ms", n.poseDelayMs, n.predictMs);
	else
		ImGui::TextColored(dim, "Pose delay: in stereo");
	ImGui::TextColored(dim, "Hands, fastest in the last half second:");
	HandLine("L", n.leftHand, dim);
	HandLine("R", n.rightHand, dim);
	const FrameTimes& times = n.frameTimes;
	float sum = 0, worst = 0;
	for (float ms : times.ms) {
		sum += ms;
		if (ms > worst) worst = ms;
	}
	char label[64];
	snprintf(label, sizeof(label), "headset frame %.1f ms avg, %.1f worst", (double)(sum / FrameTimes::kCount), (double)worst);
	ImGui::PlotLines("##frames", times.ms, FrameTimes::kCount, times.next, label, 0.0f, 33.3f,
					 ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 3.0f));
}
}  // namespace

void OverlayCountHeadsetFrame() {
	s_counts.headset++;
	double now = NowMs();
	if (s_lastFrameMs) {
		FrameTimes& times = s_numbers.frameTimes;
		times.ms[times.next] = (float)(now - s_lastFrameMs);
		times.next = (times.next + 1) % FrameTimes::kCount;
	}
	s_lastFrameMs = now;
}

void OverlayCountGameImage(int kind) {
	if (kind == 0) s_counts.gameLeft++;
	else if (kind == 1) s_counts.gameRight++;
	else s_counts.gameFlat++;
}

bool OverlayRedrawDue(bool shown, void (*gather)(OverlayNumbers& numbers)) {
	double now = NowMs();
	if (now - s_lastCheckMs > 1000) {  // just switched on (or the headset wasn't rendering): count from now
		s_counts = {};
		s_counts.startMs = now;
		s_drawnMs = 0;
	}
	s_lastCheckMs = now;
	if (now - s_counts.startMs >= 500) {
		gather(s_numbers);
		TakeFrameRates(now);
	}
	if (shown && now - s_drawnMs < 100) return false;
	s_drawnMs = now;
	return true;
}

bool OverlayRender(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, int width, int height) {
	if (!UiPrepare(s_panel, device, context, width, height)) return false;
	UiNewFrame();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.075f, 0.07f, 0.70f));  // see-through
	ImGui::Begin("stats", nullptr,
				 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
					 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
	Contents();
	ImGui::End();
	ImGui::PopStyleColor();
	UiRender(context, rtv);
	return true;
}
