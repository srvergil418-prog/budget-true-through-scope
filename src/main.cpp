#include "PCH.h"
#include "Shared.h"

#include <MinHook.h>

namespace RC
{
	// ======================= 日志 =======================
	static void WriteTo(const char* a_path, const char* fmt, va_list args)
	{
		char buf[1024];
		vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
		strcat_s(buf, "\r\n");

		HANDLE h = ::CreateFileA(a_path, FILE_APPEND_DATA, FILE_SHARE_READ,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			DWORD written = 0;
			::WriteFile(h, buf, (DWORD)strlen(buf), &written, nullptr);
			::CloseHandle(h);
		}
	}

	void Log(const char* fmt, ...)
	{
		va_list a;
		va_start(a, fmt);
		WriteTo(kLogPath, fmt, a);
		va_end(a);
	}

	void BoxLog(const char* fmt, ...)
	{
		va_list a;
		va_start(a, fmt);
		WriteTo(kBoxLogPath, fmt, a);
		va_end(a);
	}

	// ======================= 共享状态 =======================
	std::atomic<bool> g_ads{ false };
	float g_maskCX = 0.0f;
	float g_maskCY = 0.0f;
	float g_maskR = 0.0f;

	// ======================= ADS 判定 =======================
	static bool IsInADS(RE::PlayerCharacter* a_pc)
	{
		if (!a_pc) return false;
		const auto st = a_pc->gunState;
		return st == RE::GUN_STATE::kSighted || st == RE::GUN_STATE::kFireSighted;
	}

	// ======================= 节点查找 =======================
	// 从 "Weapon" 节点开始遍历：Reticle/Lense 都挂在
	//   Weapon -> WPN_xxx -> P-Scope -> ScopeAiming -> ScopeViewParts -> Reticle:0
	// 从 Get3D() 根算深度会超限找不到（上一版就是这么失败的）。
	// Weapon 找不到时退回整棵树，并把节点数写进日志便于诊断。
	static RE::NiAVObject* FindNode(RE::PlayerCharacter* a_pc, const char* a_sub, int* a_pCount = nullptr)
	{
		RE::NiAVObject* found = nullptr;
		int count = 0;
		if (!a_pc || !a_pc->Get3D() || !a_sub) return nullptr;

		RE::NiAVObject* start = nullptr;
		if (auto* weapon = a_pc->Get3D()->GetObjectByName("Weapon")) {
			start = weapon;
		} else {
			start = a_pc->Get3D();
		}

		std::function<void(RE::NiAVObject*, int)> walk = [&](RE::NiAVObject* obj, int depth) {
			if (!obj || depth > 16 || found) return;
			count++;
			const char* nm = obj->name.c_str();
			if (nm && *nm && strstr(nm, a_sub)) { found = obj; return; }
			if (auto* node = obj->IsNode()) {
				for (auto& child : node->children) {
					if (child) walk(child.get(), depth + 1);
				}
			}
		};
		walk(start, 0);
		if (a_pCount) *a_pCount = count;
		return found;
	}

	// 把候选节点的世界坐标用游戏的 viewProjMat 投影，打印 NDC。
	// 这是 stencil 方案的前置实验：如果 NDC 落在中心附近，说明这个矩阵
	// 就是渲染这棵武器树的那一个，之后就能直接用它把目镜画进 stencil。
	static void ProbeViewProj(RE::PlayerCharacter* a_pc)
	{
		auto* rd = RE::BSGraphics::RendererData::GetSingleton();
		if (!rd) { Log("[VP] RendererData NULL"); return; }

		// cameraState 在 BSGraphics::State 上，不在 RendererData
		const auto& gs = RE::BSGraphics::State::GetSingleton();
		const auto& vp = gs.cameraState.camViewData.viewProjMat;
		float m[4][4];
		for (int i = 0; i < 4; i++) {
			_mm_storeu_ps(m[i], vp[i]);
		}
		Log("[VP] r0=(%.3f,%.3f,%.3f,%.3f)", m[0][0], m[0][1], m[0][2], m[0][3]);
		Log("[VP] r1=(%.3f,%.3f,%.3f,%.3f)", m[1][0], m[1][1], m[1][2], m[1][3]);
		Log("[VP] r2=(%.3f,%.3f,%.3f,%.3f)", m[2][0], m[2][1], m[2][2], m[2][3]);
		Log("[VP] r3=(%.3f,%.3f,%.3f,%.3f)", m[3][0], m[3][1], m[3][2], m[3][3]);

		for (const char* nm : { "Reticle", "Lense", "Glass" }) {
			auto* obj = FindNode(a_pc, nm);
			if (!obj) { Log("[VP] %s: not found", nm); continue; }
			const auto& w = obj->world.translate;
			const float x = w.x, y = w.y, z = w.z, ww = 1.0f;
			const float px = x * m[0][0] + y * m[0][1] + z * m[0][2] + ww * m[0][3];
			const float py = x * m[1][0] + y * m[1][1] + z * m[1][2] + ww * m[1][3];
			const float pz = x * m[2][0] + y * m[2][1] + z * m[2][2] + ww * m[2][3];
			const float pw = x * m[3][0] + y * m[3][1] + z * m[3][2] + ww * m[3][3];
			if (fabsf(pw) > 1e-6f) {
				Log("[VP] %-8s world=(%.2f,%.2f,%.2f) ndc=(%.3f,%.3f) w=%.3f depth=%.3f",
					nm, w.x, w.y, w.z, px / pw, py / pw, pw, pz / pw);
			} else {
				Log("[VP] %-8s world=(%.2f,%.2f,%.2f) w=0 (投影在无穷远)", nm, w.x, w.y, w.z);
			}
		}
	}

	// 把准星的 world (x,y) 对齐到镜片的 (x,y)，深度(z)保持不变。
	// 两者在同一棵武器树、同一坐标系，所以完全不需要知道相机/眼睛在哪——
	// 这正是之前用相机世界坐标失败的原因（两个空间差了几万单位）。
	// 对准之后：镜片在屏幕中心 -> 准星也在屏幕中心，且准星在镜片后方，
	// 天然透过镜片才看得见、被镜筒挡住，不需要任何遮罩/投影。
	static bool AlignReticleToLense(RE::NiAVObject* a_reticle, RE::NiAVObject* a_lense)
	{
		if (!a_reticle || !a_lense || !a_reticle->parent) return false;

		const RE::NiPoint3 lw = a_lense->world.translate;
		const RE::NiPoint3 rw = a_reticle->world.translate;
		const RE::NiPoint3 target{ lw.x, lw.y, rw.z };

		const auto& pW = a_reticle->parent->world;
		const float pScale = (std::fabs(pW.scale) > 1e-6f) ? pW.scale : 1.0f;

		// 手工做 rotate^T * v（dev 分支的 NiMatrix3 没有 Transpose()，
		// NiPoint4 的成员是 .v.x / .pt[]，没有直接 .x）
		const RE::NiPoint3 d = target - pW.translate;
		const auto& r0 = pW.rotate.entry[0];
		const auto& r1 = pW.rotate.entry[1];
		const auto& r2 = pW.rotate.entry[2];
		RE::NiPoint3 local;
		local.x = r0.pt[0] * d.x + r1.pt[0] * d.y + r2.pt[0] * d.z;
		local.y = r0.pt[1] * d.x + r1.pt[1] * d.y + r2.pt[1] * d.z;
		local.z = r0.pt[2] * d.x + r1.pt[2] * d.y + r2.pt[2] * d.z;
		local = local / pScale;

		a_reticle->local.translate = local;
		return true;
	}

	// 打印子树的可见性与包围球（诊断 ADS 时哪些几何体是活的）
	static void DumpSubtree(RE::NiAVObject* a_obj, int a_depth)
	{
		static int s_total = 0;
		if (a_depth == 0) s_total = 0;
		if (!a_obj || a_depth > 4 || s_total > 60) return;
		s_total++;
		Log("[TREE]%d %-26s cull=%d world=(%.1f,%.1f,%.1f) bound=(%.1f,%.1f,%.1f) r=%.2f scale=%.2f",
			a_depth, a_obj->name.c_str(), a_obj->GetAppCulled() ? 1 : 0,
			a_obj->world.translate.x, a_obj->world.translate.y, a_obj->world.translate.z,
			a_obj->worldBound.center.x, a_obj->worldBound.center.y, a_obj->worldBound.center.z,
			a_obj->worldBound.fRadius, a_obj->world.scale);
		if (auto* node = a_obj->IsNode()) {
			for (auto& child : node->children) {
				if (child) DumpSubtree(child.get(), a_depth + 1);
			}
		}
	}

	// ======================= 遮罩投影 =======================
	// 第一人称武器树在"眼睛空间"渲染：原点≈眼睛，+Z 为前向
	// （实测 Reticle 的 world.z 恒 ~119，x/y 随武器晃动摆动）
	static void UpdateMask(RE::PlayerCharacter* a_pc)
	{
		g_maskR = 0.0f;

		RE::NiAVObject* lense = FindNode(a_pc, "Lense");
		if (!lense) {
			static bool s_warned = false;
			if (!s_warned) { s_warned = true; Log("[LENSE] not found"); }
			return;
		}

		const RE::NiPoint3 p = lense->world.translate;
		const float z = p.z;
		if (!(z > 1.0f)) return;

		auto* cam = RE::PlayerCamera::GetSingleton();
		if (!cam) return;

		float fovDeg = cam->firstPersonFOV;
		if (!(fovDeg > 5.0f) || fovDeg > 175.0f) fovDeg = 70.0f;
		const float tanHalfV = std::tan(fovDeg * 3.14159265f / 180.0f * 0.5f);
		if (tanHalfV < 1e-4f) return;

		const auto& gs = RE::BSGraphics::State::GetSingleton();
		const float W = static_cast<float>(gs.backBufferWidth);
		const float H = static_cast<float>(gs.backBufferHeight);
		if (W < 1.0f || H < 1.0f) return;
		const float tanHalfH = tanHalfV * (W / H);

		const float ndcX = (p.x / z) / tanHalfH;
		const float ndcY = (p.y / z) / tanHalfV;
		g_maskCX = (ndcX * 0.5f + 0.5f) * W;
		g_maskCY = (0.5f - ndcY * 0.5f) * H;

		// 半径：用包围球；为 0 时退回 0（不画），避免出现没有遮罩的大准星
		float rWorld = lense->worldBound.fRadius * kMaskRadiusScale;
		if (!(rWorld > 0.01f)) {
			static bool s_warned2 = false;
			if (!s_warned2) { s_warned2 = true; Log("[LENSE] worldBound.fRadius=%.3f (invalid)", rWorld); }
			return;
		}
		float rPix = (rWorld / z) * (H * 0.5f) / tanHalfV;
		if (!(rPix > 1.0f) || rPix > H) return;
		g_maskR = rPix;
	}

	// ======================= 主线程每帧 =======================
	using PCUpdateMainThread_t = void(__fastcall*)(RE::PlayerCharacter*);
	static PCUpdateMainThread_t s_origPCUpdate = nullptr;

	static void __fastcall hkPCUpdateMainThread(RE::PlayerCharacter* a_pc)
	{
		if (a_pc) {
			static bool s_lastADS = false;
			static int s_frame = 0;
			static int s_hideLog = 0;

			const bool ads = IsInADS(a_pc);
			g_ads.store(ads);
			if (ads) s_frame++;

			if (ads) {
				// 每帧算镜片投影（屏幕层遮罩用）
				UpdateMask(a_pc);

				// 隐藏原 STS 准星（它贴在瞄具上会跟着晃）
				{
					auto* ret = FindNode(a_pc, "Reticle");
					if (ret) {
						if (!ret->GetAppCulled()) {
							ret->SetAppCulled(true);
							if (s_hideLog < 20) {
								s_hideLog++;
								Log("[CULL] hid %s", ret->name.c_str());
							}
						}
					} else if (!s_lastADS) {
						Log("[CULL] no reticle node");
					}
				}

				if (!s_lastADS) {
					int cnt = 0;
					auto* ret = FindNode(a_pc, "Reticle", &cnt);
					auto* len = FindNode(a_pc, "Lense");
					auto* root = a_pc->Get3D();
					auto* wpn = root ? root->GetObjectByName("Weapon") : nullptr;
					Log("[ADS enter] mask=(%.0f,%.0f,r=%.1f) fov=%.1f nodes=%d weapon=%d reticle=%s lense=%s",
						g_maskCX, g_maskCY, g_maskR,
						RE::PlayerCamera::GetSingleton() ? RE::PlayerCamera::GetSingleton()->firstPersonFOV : -1.0f,
						cnt, wpn ? 1 : 0,
						ret ? ret->name.c_str() : "(null)",
						len ? len->name.c_str() : "(null)");

					// 前几次开镜时，把 ADS 状态下的节点可见性和包围球全打出来：
					// 需要判断 ScopeNormal 是否被隐藏、ScopeAiming 下哪个几何体是活的"洞"
					static int s_dumpCount = 0;
					if (s_dumpCount < 3) {
						s_dumpCount++;
						ProbeViewProj(a_pc);
						if (len) {
							Log("[LENSE] %s cull=%d world=(%.2f,%.2f,%.2f) bound=(%.2f,%.2f,%.2f) r=%.2f",
								len->name.c_str(), len->GetAppCulled() ? 1 : 0,
								len->world.translate.x, len->world.translate.y, len->world.translate.z,
								len->worldBound.center.x, len->worldBound.center.y, len->worldBound.center.z,
								len->worldBound.fRadius);
						}
						if (root) {
							if (auto* aiming = root->GetObjectByName("ScopeAiming")) {
								DumpSubtree(aiming, 0);
							} else {
								Log("[TREE] ScopeAiming not found by exact name");
							}
						}
					}
				}
			} else if (s_lastADS) {
				g_maskR = 0.0f;
				Log("[ADS exit]");
			}

			s_lastADS = ads;
		}

		s_origPCUpdate(a_pc);
	}

	// ======================= Present =======================
	using Present_t = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
	static Present_t s_origPresent = nullptr;

	static HRESULT WINAPI hkPresent(IDXGISwapChain* a_sc, UINT a_sync, UINT a_flags)
	{
		// F7 开关调参菜单
		static bool s_lastF7 = false;
		const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
		if (f7 && !s_lastF7) ImGui_Toggle();
		s_lastF7 = f7;

		ImGui_Render(a_sc);

		// 屏幕层准星：固定在屏幕中心，不跟瞄具晃
		OverlayDraw(a_sc);
		return s_origPresent(a_sc, a_sync, a_flags);
	}

	// ======================= 安装 =======================
	static void InstallHooks()
	{
		const auto mh = MH_Initialize();
		Log("[INIT] MH_Initialize = %d", (int)mh);

		{
			static REL::Relocation<std::uintptr_t> addr{ REL::ID(1134912) };
			if (MH_CreateHook((LPVOID)addr.address(), (LPVOID)&hkPCUpdateMainThread, (LPVOID*)&s_origPCUpdate) == MH_OK) {
				MH_EnableHook((LPVOID)addr.address());
				Log("[INIT] PCUpdateMainThread hooked @ %p", (void*)addr.address());
			} else {
				Log("[INIT] PCUpdateMainThread hook FAILED");
			}
		}

		auto* rd = RE::BSGraphics::RendererData::GetSingleton();
		if (rd && rd->renderWindow && rd->renderWindow->swapChain) {
			auto* sc = (IDXGISwapChain*)rd->renderWindow->swapChain;
			void** vt = *(void***)sc;
			if (MH_CreateHook(vt[8], (LPVOID)&hkPresent, (LPVOID*)&s_origPresent) == MH_OK) {
				MH_EnableHook(vt[8]);
				Log("[INIT] Present hooked @ %p", vt[8]);
			} else {
				Log("[INIT] Present hook FAILED");
			}

			// 初始化调参菜单（F7）——需要 device / context / 窗口句柄
			DXGI_SWAP_CHAIN_DESC sd{};
			if (SUCCEEDED(sc->GetDesc(&sd))) {
				auto* dev = (ID3D11Device*)static_cast<void*>(rd->device);
				auto* ctx = (ID3D11DeviceContext*)static_cast<void*>(rd->context);
				ImGui_Init(dev, ctx, sd.OutputWindow);
			} else {
				Log("[INIT] swapchain GetDesc failed (menu disabled)");
			}
		} else {
			Log("[INIT] RendererData/swapChain NULL");
		}
	}

	static void OnMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) return;
		if (a_msg->type == F4SE::MessagingInterface::kGameDataReady) {
			Log("=== ReticleCenter 独立插件 启动 ===");
			InstallHooks();
		} else if (a_msg->type == F4SE::MessagingInterface::kPostLoadGame) {
			Log("[STATE] 存档载入，重置");
			g_maskR = 0.0f;
			g_ads.store(false);
		}
	}
}

// 用 F4SE 老式入口（Query + Load）：
//   新式的 F4SEPlugin_Version / PluginVersionData 只有 master 分支有，而 master 的
//   REL::ID 对 1.10.163 无效（加载即崩）。dev-1.10.163 分支 ID 全对但没有
//   PluginVersionData，所以走老式接口 —— F4SE 0.6.23 仍然支持它。
extern "C" __declspec(dllexport) bool F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = "ReticleCenter";
	a_info->version = 1;
	return true;
}

extern "C" __declspec(dllexport) bool F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);
	RC::Log("=== ReticleCenter loaded (build %s %s) ===", __DATE__, __TIME__);

	auto* msg = F4SE::GetMessagingInterface();
	if (msg) {
		msg->RegisterListener(RC::OnMessage);
	} else {
		RC::Log("[INIT] GetMessagingInterface FAILED");
	}
	return true;
}
