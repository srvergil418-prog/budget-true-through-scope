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
	extern float g_maskRadiusScale; // auto 模式下对自动半径的缩放
	extern bool  g_maskAuto;      // true = 用镜片方向自动算遮罩（B 方案）
	extern float g_fovOverride;   // >0 = ADS 时强制这个 FOV，<=0 = 用游戏默认
	// B 方案：透镜 nif 相对 Weapon 节点的偏移
	extern float g_lensOffX;
	extern float g_lensOffY;
	extern float g_lensOffZ;
	extern bool  g_lensEnabled;   // 是否挂载透镜
	// 调试剔除开关（ImGui 持久勾选）
	extern bool  g_sup144;        // 不渲染 144 索引（ScopeFade）
	extern bool  g_sup384A;       // 不渲染 384 候选 A（低 voff）
	extern bool  g_sup384B;       // 不渲染 384 候选 B（高 voff）
	extern bool  g_supNif;        // 不渲染自挂 nif（96/stride24）
	extern bool  g_supLense;      // 不渲染 Lense 的两个 384 draw（A+B 一起）
	extern bool  g_diagSplit;     // 红/绿诊断：把 Lense 拆成两片分别染色
	extern bool  g_diagBOnly;     // 只画 B 组（index 0..191）染红
	extern bool  g_diagCross;     // 十字诊断：B 组几何 + 屏幕中心十字
	extern bool  g_screenOverlay; // 旧的屏幕层准星（红圈+十字）
	extern bool  g_skipDepthWrite; // r166: Lense 深度预 pass 不写深度（验证深度因果）
	extern int   g_crossTarget;   // 0=both, 1=only voffA(7670272), 2=only voffB(12181248), 3=none
	extern float g_crossOffX;     // 诊断用十字偏移 X
	extern float g_crossOffY;     // 诊断用十字偏移 Y
	extern bool  g_diagAOnly;     // 只画 A 组（index 192..383）染绿
	extern bool  g_forceLense;    // 强制 Lense:0 可见（覆盖 STS 剔除）
	// 逐节点强制显示 ScopeNormal 下的子节点（默认被 STS 剔除）
	extern bool  g_forcePoly;     // polySurface22
	extern bool  g_forceMount1;
	extern bool  g_forceMount2;
	extern bool  g_wantShot;      // 请求截一张图（菜单按钮）

	// ---- r169 两阶段：目镜 mask ----
	extern bool  g_maskEnabled;   // 在 Lense 颜色 pass 里写 R8 mask
	extern bool  g_maskUse;       // overlay 采样 mask 裁剪准星（M3）
	extern bool  g_maskDebug;     // M1：双色调试（mask 红 + 准星绿）
	extern bool  g_redCross;      // 红色十字（overlay 层，受 mask 裁切）
	extern bool  g_strictDW;     // r180: SKIP DEPTH 严格身份匹配（诊断用，默认关）
	extern bool  g_bttsEnabled;   // r186: 一键启用 BTTS（连带设置所有必要开关）
	extern char  g_scopeProfileName[32];   // r201: 当前识别到的瞄具 profile 名（菜单显示用）
	extern bool  g_showScopeReticle;      // r202: 显示 STS 原准星（贴在瞄具上），默认 false
	extern bool  g_scopeValid;           // r205: 当前瞄具是否被 BTTS 识别（false = 机械瞄具/未知，不画准星）
	extern ID3D11ShaderResourceView* g_maskSRV;   // overlay 读取用（main.cpp 创建）

	// ---- overlay（overlay.cpp 实现）----
	void OverlayDraw(IDXGISwapChain* a_swapChain);
	void OverlayShutdown();
	bool LoadDdsSrv(ID3D11Device* a_dev, const wchar_t* a_path, ID3D11ShaderResourceView** a_outSrv);

	// ---- imgui 菜单（imgui_ui.cpp 实现）----
	bool ImGui_Init(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, HWND a_hwnd);
	void ImGui_Render(IDXGISwapChain* a_sc);
	void ImGui_Toggle();
	bool ImGui_menuOpen();
}
