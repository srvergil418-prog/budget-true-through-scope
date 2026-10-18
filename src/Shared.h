#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <cstdint>

namespace RC
{
	// ---- 配置（先用常量，跑通后再做 ini）----
	constexpr const char* kLogPath = "E:\\Custom Fallout4\\reticlecenter.txt";
	constexpr const char* kBoxLogPath = "E:\\Custom Fallout4\\reticle-box.txt";

	// 准星贴图：优先用 TTS mod 里已经放好的那张（512x512 BC3）
	constexpr const char* kReticleDds = "Data\\Textures\\TTS\\Reticle\\CSResRetGerman1.dds";

	// 遮罩半径的安全系数：世界半径 * 系数（校准用，1.0 = 直接用包围球）
	constexpr float kMaskRadiusScale = 1.0f;

	// ---- 日志 ----
	void Log(const char* fmt, ...);
	void BoxLog(const char* fmt, ...);

	// ---- 共享状态：主线程写，渲染线程读 ----
	extern std::atomic<bool> g_ads;
	extern float g_maskCX;   // 遮罩圆心（屏幕像素）
	extern float g_maskCY;
	extern float g_maskR;    // 0 = 无效，此时不画准星

	// ---- overlay（overlay.cpp 实现）----
	void OverlayDraw(IDXGISwapChain* a_swapChain);
	void OverlayShutdown();
}
