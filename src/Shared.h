#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <cstdint>
#include <atomic>

namespace RC
{
	// ---- 配置 ----
	constexpr const char* kLogPath = "E:\\Custom Fallout4\\reticlecenter.txt";
	constexpr const char* kBoxLogPath = "E:\\Custom Fallout4\\reticle-box.txt";

	// 准星贴图（512x512 BC3）
	constexpr const char* kReticleDds = "Data\\Textures\\TTS\\Reticle\\CSResRetGerman1.dds";

	// 镜片包围球半径的校准系数（仅用于日志诊断）
	constexpr float kMaskRadiusScale = 1.0f;

	// ---- 日志 ----
	void Log(const char* fmt, ...);
	void BoxLog(const char* fmt, ...);

	// ---- 共享状态：主线程写，渲染线程读 ----
	extern std::atomic<bool> g_ads;
	extern float g_maskCX;   // 镜片投影（暂未使用）
	extern float g_maskCY;
	extern float g_maskR;

	// ---- 可调参数（ImGui 菜单里改，overlay 里用）----
	extern float g_reticleOffX;   // 准星相对屏幕中心的偏移
	extern float g_reticleOffY;
	extern float g_reticleScale;  // 准星缩放
	extern float g_reticleSize;   // 准星基准边长（像素），与遮罩半径无关
	extern float g_maskOffX;      // 遮罩圆心相对屏幕中心的偏移
	extern float g_maskOffY;
	extern float g_maskRadius;    // 遮罩半径（像素）
	extern bool  g_maskAuto;      // true = 用镜片方向自动算遮罩（B 方案）

	// ---- overlay（overlay.cpp 实现）----
	void OverlayDraw(IDXGISwapChain* a_swapChain);
	void OverlayShutdown();

	// ---- imgui 菜单（imgui_ui.cpp 实现）----
	bool ImGui_Init(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, HWND a_hwnd);
	void ImGui_Render(IDXGISwapChain* a_sc);
	void ImGui_Toggle();
	bool ImGui_menuOpen();
}
