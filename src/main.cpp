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
	static RE::NiAVObject* FindNode(RE::PlayerCharacter* a_pc, const char* a_sub, int* a_pCount = nullptr)
	{
		RE::NiAVObject* found = nullptr;
		int count = 0;
		if (!a_pc || !a_pc->Get3D() || !a_sub) return nullptr;

		std::function<void(RE::NiAVObject*, int)> walk = [&](RE::NiAVObject* obj, int depth) {
			if (!obj || depth > 14 || found) return;
			count++;
			const char* nm = obj->name.c_str();
			if (nm && *nm && strstr(nm, a_sub)) { found = obj; return; }
			if (auto* node = obj->IsNode()) {
				for (auto& child : node->children) {
					if (child) walk(child.get(), depth + 1);
				}
			}
		};
		walk(a_pc->Get3D(), 0);
		if (a_pCount) *a_pCount = count;
		return found;
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
				// 每帧算遮罩（镜片随武器晃）
				UpdateMask(a_pc);

				// 隐藏原准星：ADS 进入时 + 每 15 帧重跑（切枪会重建节点）
				if (!s_lastADS || (s_frame % 15) == 0) {
					if (auto* ret = FindNode(a_pc, "Reticle")) {
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
					Log("[ADS enter] mask=(%.0f,%.0f,r=%.1f) fov=%.1f",
						g_maskCX, g_maskCY, g_maskR, RE::PlayerCamera::GetSingleton() ? RE::PlayerCamera::GetSingleton()->firstPersonFOV : -1.0f);
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

F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
	F4SE::PluginVersionData data{};
	data.AuthorName("ReticleCenter");
	data.PluginName("ReticleCenter");
	data.PluginVersion(0x00010000);
	data.UsesAddressLibrary(true);
	data.IsLayoutDependent(true);
	data.UsesSigScanning(false);
	data.HasNoStructUse(false);
	data.CompatibleVersions({ F4SE::RUNTIME_1_10_163 });
	return data;
}();

F4SE_EXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
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
