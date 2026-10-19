#include "PCH.h"
#include "Shared.h"

#ifdef RC_HAS_IMGUI
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace RC
{
	// ---- 可调参数（overlay 里用）----
	float g_reticleOffX = 0.0f;
	float g_reticleOffY = 0.0f;
	float g_reticleScale = 1.0f;
	float g_reticleSize = 500.0f;   // 准星基准边长（像素），与遮罩半径无关
	float g_maskOffX = 0.0f;
	float g_maskOffY = 0.0f;
	float g_maskRadius = 324.0f;
	float g_maskRadiusScale = 1.0f;
	bool  g_maskAuto = false;       // B 方案：用镜片方向自动算遮罩
	float g_fovOverride = 0.0f;     // >0 = ADS 时强制 FOV
	float g_lensOffX = 0.0f;
	float g_lensOffY = 0.0f;
	float g_lensOffZ = 10.0f;
	bool  g_lensEnabled = true;
	bool  g_sup144 = false;
	bool  g_sup384A = false;
	bool  g_sup384B = false;
	bool  g_supNif = false;
	bool  g_supLense = false;
	bool  g_diagSplit = false;
	bool  g_diagBOnly = false;
	bool  g_diagCross = false;
	bool  g_screenOverlay = true;
	bool  g_skipDepthWrite = false;
	bool  g_maskEnabled = false;
	bool  g_maskUse = false;
	int   g_crossTarget = 0;
	float g_crossOffX = 0.0f;
	float g_crossOffY = 0.0f;
	bool  g_diagAOnly = false;
	bool  g_forceLense = false;
	bool  g_forcePoly = false;
	bool  g_forceMount1 = false;
	bool  g_forceMount2 = false;
	bool  g_wantShot = false;

	static bool s_init = false;
	static bool s_menu = false;
	static WNDPROC s_origWndProc = nullptr;

	static LRESULT CALLBACK hkWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		if (s_menu) {
			if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
		}
		return CallWindowProc(s_origWndProc, h, m, w, l);
	}

	bool ImGui_Init(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, HWND a_hwnd)
	{
		if (s_init) return true;
		if (!a_dev || !a_ctx || !a_hwnd) return false;

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;   // 不写 imgui.ini
		ImGui::StyleColorsDark();

		if (!ImGui_ImplWin32_Init(a_hwnd)) return false;
		if (!ImGui_ImplDX11_Init(a_dev, a_ctx)) return false;

		s_origWndProc = (WNDPROC)SetWindowLongPtr(a_hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
		s_init = true;
		Log("[IMGUI] init ok, hwnd=%p", (void*)a_hwnd);
		return true;
	}

	// 菜单开关：除切换显示外，还要处理"游戏抢鼠标"和"光标可见性"。
	// ShowCursor 是【计数器】而不是开关（TRUE 加 1 / FALSE 减 1，计数 >= 0 才可见），
	// 所以两边都要用循环压到位，不能只调一次。
	static void SetCursorVisible(bool a_visible)
	{
		int count = ShowCursor(a_visible ? TRUE : FALSE);
		for (int i = 0; i < 50 && (a_visible ? (count < 0) : (count >= 0)); i++) {
			count = ShowCursor(a_visible ? TRUE : FALSE);
		}
	}

	void ImGui_Toggle()
	{
		s_menu = !s_menu;

		SetCursorVisible(s_menu);

		// 菜单打开时让游戏不再吃掉鼠标（否则镜头还在跟着动）
		if (auto* cm = RE::ControlMap::GetSingleton()) {
			cm->ignoreKeyboardMouse = s_menu;
		} else {
			Log("[IMGUI] ControlMap NULL");
		}

		Log("[IMGUI] menu=%d", s_menu ? 1 : 0);
	}
	bool ImGui_menuOpen() { return s_menu; }

	static float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

	void ImGui_Render(IDXGISwapChain* a_sc)
	{
		if (!s_init) return;

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// 一行控件：[滑条] [数值输入] [reset]
		auto Row = [](const char* label, float* v, float def, float step, float vmin, float vmax) {
			ImGui::PushID(label);
			ImGui::TextUnformatted(label);
			ImGui::SameLine(150.0f);
			ImGui::SetNextItemWidth(170.0f);
			ImGui::SliderFloat("##s", v, vmin, vmax, "%.2f");
			*v = ClampF(*v, vmin, vmax);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(95.0f);
			ImGui::InputFloat("##i", v, 0.0f, 0.0f, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button("reset")) *v = def;
			ImGui::PopID();
			(void)step;
		};

		if (s_menu) {
			ImGui::SetNextWindowSize(ImVec2(430, 400), ImGuiCond_FirstUseEver);
			ImGui::Begin("ReticleCenter (F7)", &s_menu);

			ImGui::Text("Reticle (screen space)");
			Row("offset X", &g_reticleOffX, 0.0f, 1.0f, -2000.0f, 2000.0f);
			Row("offset Y", &g_reticleOffY, 0.0f, 1.0f, -2000.0f, 2000.0f);
			Row("size", &g_reticleSize, 500.0f, 10.0f, 50.0f, 3000.0f);
			Row("scale", &g_reticleScale, 1.0f, 0.05f, 0.05f, 5.0f);

			ImGui::Separator();
			ImGui::Text("Mask circle");
			ImGui::Checkbox("auto (lens direction)", &g_maskAuto);
			Row("mask X", &g_maskOffX, 0.0f, 1.0f, -2000.0f, 2000.0f);
			Row("mask Y", &g_maskOffY, 0.0f, 1.0f, -2000.0f, 2000.0f);
			Row("radius", &g_maskRadius, 324.0f, 5.0f, 10.0f, 2000.0f);
			Row("radius x (auto)", &g_maskRadiusScale, 1.0f, 0.05f, 0.1f, 5.0f);

			ImGui::Separator();
			ImGui::Text("Debug: skip draws (persistent)");
			// 四个 FORCE 开关互斥：勾一个自动取消其他三个
			if (ImGui::Checkbox("FORCE Lense:0 visible", &g_forceLense) && g_forceLense) {
				g_forcePoly = g_forceMount1 = g_forceMount2 = false;
			}
			if (ImGui::Checkbox("FORCE polySurface22 visible", &g_forcePoly) && g_forcePoly) {
				g_forceLense = g_forceMount1 = g_forceMount2 = false;
			}
			if (ImGui::Checkbox("FORCE Mount1 visible", &g_forceMount1) && g_forceMount1) {
				g_forceLense = g_forcePoly = g_forceMount2 = false;
			}
			if (ImGui::Checkbox("FORCE Mount2 visible", &g_forceMount2) && g_forceMount2) {
				g_forceLense = g_forcePoly = g_forceMount1 = false;
			}
			ImGui::Checkbox("skip LENSE (both 384)", &g_supLense);
			ImGui::Checkbox("DIAG red/green split (B=red A=green)", &g_diagSplit);
			if (ImGui::Checkbox("DIAG B-only (red)", &g_diagBOnly) && g_diagBOnly) g_diagAOnly = false;
			if (ImGui::Checkbox("DIAG A-only (green)", &g_diagAOnly) && g_diagAOnly) g_diagBOnly = false;
			ImGui::Checkbox("DIAG CROSS (B-geo + screen center)", &g_diagCross);
			ImGui::Checkbox("screen overlay reticle (old)", &g_screenOverlay);
			ImGui::Checkbox("SKIP DEPTH WRITE (r166 test)", &g_skipDepthWrite);
			ImGui::Checkbox("MASK write (r169 two-stage)", &g_maskEnabled);
			ImGui::Checkbox("MASK clip reticle", &g_maskUse);
			ImGui::Combo("cross target", &g_crossTarget, "both\0only voffA\0only voffB\0none\0A-cross+B-skip\0B-cross+A-skip\0");
			ImGui::SliderFloat("cross offX", &g_crossOffX, -400.0f, 400.0f, "%.0f");
			ImGui::SliderFloat("cross offY", &g_crossOffY, -400.0f, 400.0f, "%.0f");
			ImGui::Checkbox("skip 144 (ScopeFade)", &g_sup144);
			ImGui::Checkbox("skip 384 A (low voff)", &g_sup384A);
			ImGui::Checkbox("skip 384 B (high voff)", &g_sup384B);
			ImGui::Checkbox("skip NIF (96/stride24)", &g_supNif);
			if (ImGui::Button("save screenshot")) g_wantShot = true;

			ImGui::Separator();
			ImGui::Text("Camera FOV");
			Row("fov (0 = game default)", &g_fovOverride, 0.0f, 1.0f, 0.0f, 150.0f);

			ImGui::Separator();
			ImGui::Text("Lens nif (B)");
			ImGui::Checkbox("spawn lens", &g_lensEnabled);
			Row("lens X", &g_lensOffX, 0.0f, 0.5f, -100.0f, 100.0f);
			Row("lens Y", &g_lensOffY, 0.0f, 0.5f, -100.0f, 100.0f);
			Row("lens Z", &g_lensOffZ, 10.0f, 0.5f, -100.0f, 100.0f);

			ImGui::Separator();
			ImGui::Text("auto raw: (%.0f, %.0f) r=%.0f", g_maskCX, g_maskCY, g_maskR);
			ImGui::TextDisabled("red ring = mask border");
			ImGui::End();
		}

		ImGui::Render();
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}
}
#else
namespace RC
{
	float g_reticleOffX = 0.0f;
	float g_reticleOffY = 0.0f;
	float g_reticleScale = 1.0f;
	float g_maskOffX = 0.0f;
	float g_maskOffY = 0.0f;
	float g_maskRadius = 324.0f;

	bool ImGui_Init(ID3D11Device*, ID3D11DeviceContext*, HWND) { return false; }
	void ImGui_Render(IDXGISwapChain*) {}
	void ImGui_Toggle() {}
	bool ImGui_menuOpen() { return false; }
}
#endif
