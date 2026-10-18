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
	float g_maskOffX = 0.0f;
	float g_maskOffY = 0.0f;
	float g_maskRadius = 324.0f;   // 默认 15% 屏高（2160*0.15）

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

	void ImGui_Toggle() { s_menu = !s_menu; }
	bool ImGui_menuOpen() { return s_menu; }

	void ImGui_Render(IDXGISwapChain* a_sc)
	{
		if (!s_init) return;

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		if (s_menu) {
			ImGui::SetNextWindowSize(ImVec2(380, 320), ImGuiCond_FirstUseEver);
			ImGui::Begin("ReticleCenter (F7)", &s_menu);
			ImGui::Text("Reticle (screen space)");
			ImGui::SliderFloat("offset X", &g_reticleOffX, -1500.0f, 1500.0f, "%.0f");
			ImGui::SliderFloat("offset Y", &g_reticleOffY, -1500.0f, 1500.0f, "%.0f");
			ImGui::SliderFloat("scale", &g_reticleScale, 0.2f, 3.0f, "%.2f");
			ImGui::Separator();
			ImGui::Text("Mask circle");
			ImGui::SliderFloat("mask X", &g_maskOffX, -1500.0f, 1500.0f, "%.0f");
			ImGui::SliderFloat("mask Y", &g_maskOffY, -1500.0f, 1500.0f, "%.0f");
			ImGui::SliderFloat("radius", &g_maskRadius, 20.0f, 1200.0f, "%.0f");
			ImGui::Separator();
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
