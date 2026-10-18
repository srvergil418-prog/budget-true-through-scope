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
	// B 方案：用「准星方向」当屏幕中心，算镜片相对它的屏幕偏移。
	// 准星（Reticle:0）本来就对准弹道，所以它的方向 = 屏幕中心方向；
	// 镜片（Lense:0）和它在同一坐标系，两者夹角就是遮罩该有的偏移。
	// 偏移和半径都经过 FOV 换算 -> FOV 变化、换枪、换镜都自动适配。
	static void UpdateMask(RE::PlayerCharacter* a_pc)
	{
		g_maskR = 0.0f;

		RE::NiAVObject* lense = FindNode(a_pc, "Lense");
		RE::NiAVObject* ret = FindNode(a_pc, "Reticle");
		if (!lense || !ret) {
			static bool s_warned = false;
			if (!s_warned) { s_warned = true; Log("[MASK] lense/reticle missing"); }
			return;
		}

		const RE::NiPoint3 r = ret->world.translate;
		const RE::NiPoint3 l = lense->world.translate;
		const float rLen = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
		const float lLen = std::sqrt(l.x * l.x + l.y * l.y + l.z * l.z);
		if (!(rLen > 1.0f) || !(lLen > 1.0f)) return;

		const RE::NiPoint3 vR{ r.x / rLen, r.y / rLen, r.z / rLen };
		const RE::NiPoint3 vL{ l.x / lLen, l.y / lLen, l.z / lLen };

		float dot = vR.x * vL.x + vR.y * vL.y + vR.z * vL.z;
		if (dot > 0.99999f) dot = 0.99999f;
		if (dot < -0.99999f) dot = -0.99999f;

		// 垂直于准星方向的残余分量 = 屏幕平面上的偏移方向
		const RE::NiPoint3 perp{ vL.x - vR.x * dot, vL.y - vR.y * dot, vL.z - vR.z * dot };

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

		// perp 的横向分量 / 深度 -> NDC 偏移（x=右, y=上, z=前）
		const float ndcX = (perp.x / dot) / (tanHalfV * (W / H));
		const float ndcY = (perp.y / dot) / tanHalfV;
		g_maskCX = (ndcX * 0.5f) * W;
		g_maskCY = (-ndcY * 0.5f) * H;

		float rWorld = lense->worldBound.fRadius;
		if (!(rWorld > 0.01f)) return;
		float rPix = (rWorld / lLen) * (H * 0.5f) / tanHalfV;
		if (!(rPix > 1.0f) || rPix > H) return;
		g_maskR = rPix;
	}

	// ======================= B: 圆形透镜（贴在武器上的 nif） =======================
	// 从 Meshes\ReticleCenter\LensCircle.nif 加载一个圆形平面，挂到 Weapon 下。
	// 它是个 3D 物体：决定"准星能出现在哪些像素"——圆外根本没有几何，所以画不出来。
	// 位置参数 (0,0,10) 沿用 TTS 的默认值，之后可在菜单里偏移。
	static RE::NiAVObject* s_lensNode = nullptr;

	static void CacheLensIndexCount(RE::NiAVObject* a_node);

	static void SpawnLens()
	{
		if (!g_lensEnabled) return;
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc || !pc->Get3D()) return;
		auto* weapon = pc->Get3D()->GetObjectByName("Weapon");
		if (!weapon) return;
		auto* weaponNode = weapon->IsNode();
		if (!weaponNode) return;

		// 目标位置：挂到 ScopeFade 的包围球中心 —— 那是圆环的圆心，也就是
		// "洞"的中心，这个位置才是镜片该在的地方。
		RE::NiPoint3 target{ g_lensOffX, g_lensOffY, g_lensOffZ };
		if (auto* fade = FindNode(pc, "ScopeFade")) {
			const RE::NiPoint3 c = fade->worldBound.center;
			const auto& pW = weaponNode->world;
			const float pScale = (std::fabs(pW.scale) > 1e-6f) ? pW.scale : 1.0f;
			const RE::NiPoint3 d = c - pW.translate;

			RE::NiPoint3 local;
			local.x = pW.rotate.entry[0].pt[0] * d.x + pW.rotate.entry[1].pt[0] * d.y + pW.rotate.entry[2].pt[0] * d.z;
			local.y = pW.rotate.entry[0].pt[1] * d.x + pW.rotate.entry[1].pt[1] * d.y + pW.rotate.entry[2].pt[1] * d.z;
			local.z = pW.rotate.entry[0].pt[2] * d.x + pW.rotate.entry[1].pt[2] * d.y + pW.rotate.entry[2].pt[2] * d.z;
			local = local / pScale;

			target.x = local.x + g_lensOffX;
			target.y = local.y + g_lensOffY;
			target.z = local.z + g_lensOffZ;

			static int s_alignLog = 0;
			if (s_alignLog < 3) {
				s_alignLog++;
				Log("[LENS] align to fade center=(%.2f,%.2f,%.2f) -> local=(%.2f,%.2f,%.2f)",
					c.x, c.y, c.z, local.x, local.y, local.z);
			}
		}

		// 已经挂着且还在武器树下：只更新位置（菜单改完立刻生效），不重新加载
		if (s_lensNode && s_lensNode->parent == weaponNode) {
			s_lensNode->local.translate = target;
			RE::NiUpdateData ud{};
			s_lensNode->Update(ud);
			return;
		}

		RE::NiPointer<RE::NiNode> model;
		RE::BSModelDB::DBTraits::ArgsType args{};
		args.prepareAfterLoad = 1;
		args.loadTextures = 1;
		args.performProcess = 1;
		RE::BSModelDB::Demand("Meshes\\ReticleCenter\\LensCircle.nif", &model, args);

		auto* node = model.get();
		if (!node) {
			static bool s_warned = false;
			if (!s_warned) { s_warned = true; Log("[LENS] nif load FAILED"); }
			return;
		}
		node->IncRefCount();
		node->name = "RCLens";
		weaponNode->AttachChild(node, false);
		node->local.translate = target;
		node->local.rotate.MakeIdentity();

		RE::NiUpdateData ud{};
		weaponNode->Update(ud);
		node->Update(ud);

		s_lensNode = node;
		Log("[LENS] spawned (%s) children=%zu", node->name.c_str(), node->children.size());
		CacheLensIndexCount(node);
	}

	// 强制镜片（Lense）保持可见：STS 在瞄准时会把 ScopeNormal（含 Lense）剔除，
	// 导致我们换的 shader 根本没机会执行。这里每帧把 Lense 连同它的父链一起
	// 取消剔除，覆盖 STS 的隐藏。
	static void ForceLenseVisible(RE::PlayerCharacter* a_pc)
	{
		auto* lense = FindNode(a_pc, "Lense");
		if (!lense) return;
		if (lense->GetAppCulled()) lense->SetAppCulled(false);
		auto* p = lense->parent;
		int guard = 0;
		while (p && guard++ < 8) {
			if (p->GetAppCulled()) p->SetAppCulled(false);
			p = p->parent;
		}
	}

	// 打印一个子树下所有 BSTriShape 的三角形/顶点数和可见状态。
	// 用途：找出 ADS 时真正在渲染的镜片（ScopeNormal 下的 Lense 会被 STS 剔除）。
	static void DumpShapes(RE::NiAVObject* a_obj, int a_depth)
	{
		if (!a_obj || a_depth > 4) return;
		if (auto* tri = a_obj->IsTriShape()) {
			Log("[SHAPE]%d %-22s tri=%u vert=%u cull=%d",
				a_depth, a_obj->name.c_str(),
				tri->numTriangles, (unsigned)tri->numVertices,
				a_obj->GetAppCulled() ? 1 : 0);
			return;
		}
		if (auto* node = a_obj->IsNode()) {
			for (auto& c : node->children) {
				if (c) DumpShapes(c.get(), a_depth + 1);
			}
		}
	}

	// ======================= 透镜绘制识别 =======================
	// 回退：用 Lense（ScopeNormal 下那一块），不再用 ScopeFade。
	static UINT s_lensIndexCount = 0;
	static UINT s_diskIndexCount = 0;

	static void CacheLenseIndexCount(RE::PlayerCharacter* a_pc)
	{
		if (s_lensIndexCount > 0) return;
		if (!a_pc) return;
		auto* target = FindNode(a_pc, "Lense");
		if (!target) return;
		if (auto* tri = target->IsTriShape()) {
			s_lensIndexCount = tri->numTriangles * 3u;
			Log("[LENS] using '%s' indexCount=%u (tri=%u vert=%u)",
				target->name.c_str(), s_lensIndexCount,
				tri->numTriangles, (unsigned)tri->numVertices);
		}
	}

	static void CacheLensIndexCount(RE::NiAVObject* a_node)
	{
		if (!a_node) return;
		if (auto* tri = a_node->IsTriShape()) {
			s_diskIndexCount = tri->numTriangles * 3u;
			Log("[LENS] disk indexCount=%u (tri=%u vert=%u)",
				s_diskIndexCount, tri->numTriangles, (unsigned)tri->numVertices);
			return;
		}
		if (auto* node = a_node->IsNode()) {
			for (auto& c : node->children) {
				if (c) CacheLensIndexCount(c.get());
			}
		}
	}

	using DrawIndexed_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
	static DrawIndexed_t s_origDrawIndexed = nullptr;

	// ======================= 透镜绘制：屏幕 UV 准星 =======================
	// 命中透镜的 DrawIndexed 时，换上这个像素着色器：用屏幕坐标采样准星贴图，
	// 准星之外输出全透明。于是准星"钉在屏幕上"，但只有透镜几何体覆盖的像素
	// 才会被画出来 —— 也就是"只有透过透镜才看得见"。
	namespace LensDraw
	{
		static ID3D11PixelShader* s_ps = nullptr;
		static ID3D11Buffer* s_cb = nullptr;
		static ID3D11ShaderResourceView* s_srv = nullptr;
		static ID3D11SamplerState* s_samp = nullptr;
		static ID3D11DepthStencilState* s_ds = nullptr;

		struct Cb
		{
			float screenW, screenH, centerX, centerY, size, alpha, pad0, pad1;
		};

		static void Init(ID3D11Device* a_dev)
		{
			if (s_ps || !a_dev) return;

			static const char* kPs = R"(
cbuffer Cb : register(b0) {
    float screenW, screenH, centerX, centerY, size, alpha, pad0, pad1;
};
Texture2D    gTex  : register(t0);
SamplerState gSamp : register(s0);
float4 main(float4 pos : SV_POSITION) : SV_Target {
    // 调试模式：圆盘内、准星范围外 -> 半透明绿（定位用）；
    // 准星范围内 -> 真实准星贴图。
    float2 uv = (pos.xy - float2(centerX, centerY)) / size + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return float4(0.0, 1.0, 0.0, 0.45);
    }
    float4 c = gTex.Sample(gSamp, uv);
    return float4(c.rgb, c.a * alpha);
}
)";

			ID3DBlob* blob = nullptr;
			ID3DBlob* err = nullptr;
			const HRESULT hr = D3DCompile(kPs, strlen(kPs), nullptr, nullptr, nullptr, "main", "ps_5_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
			if (FAILED(hr) || !blob) {
				Log("[LENSDRAW] ps compile FAILED 0x%08X", (unsigned)hr);
				if (err) err->Release();
				return;
			}
			a_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_ps);
			blob->Release();
			if (err) err->Release();

			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = sizeof(Cb);
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			a_dev->CreateBuffer(&bd, nullptr, &s_cb);

			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			a_dev->CreateSamplerState(&sd, &s_samp);

			// Lense 是多层筒状几何（同心圆），不限制的话每层都会画一遍准星，
			// 叠起来就是万花筒。开启深度测试，只让最近的一层通过。
			D3D11_DEPTH_STENCIL_DESC dsDesc{};
			dsDesc.DepthEnable = TRUE;
			dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			dsDesc.StencilEnable = FALSE;
			a_dev->CreateDepthStencilState(&dsDesc, &s_ds);

			wchar_t wpath[MAX_PATH] = {};
			MultiByteToWideChar(CP_ACP, 0, kReticleDds, -1, wpath, MAX_PATH);
			LoadDdsSrv(a_dev, wpath, &s_srv);

			Log("[LENSDRAW] init ok ps=%d srv=%d", s_ps ? 1 : 0, s_srv ? 1 : 0);
		}

		static void Draw(ID3D11DeviceContext* a_ctx, UINT a_count, UINT a_start, INT a_base)
		{
			if (!s_ps || !s_cb) return;

			const auto& gs = RE::BSGraphics::State::GetSingleton();
			Cb cb{};
			cb.screenW = (float)gs.backBufferWidth;
			cb.screenH = (float)gs.backBufferHeight;
			cb.centerX = cb.screenW * 0.5f + g_reticleOffX;
			cb.centerY = cb.screenH * 0.5f + g_reticleOffY;
			cb.size = g_reticleSize * g_reticleScale;
			cb.alpha = 1.0f;

			D3D11_MAPPED_SUBRESOURCE ms{};
			if (SUCCEEDED(a_ctx->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
				std::memcpy(ms.pData, &cb, sizeof(cb));
				a_ctx->Unmap(s_cb, 0);
			}

			ID3D11PixelShader* oldPS = nullptr;
			a_ctx->PSGetShader(&oldPS, nullptr, nullptr);

			ID3D11DepthStencilState* oldDS = nullptr;
			UINT oldStencilRef = 0;
			a_ctx->OMGetDepthStencilState(&oldDS, &oldStencilRef);

			a_ctx->PSSetShader(s_ps, nullptr, 0);
			a_ctx->PSSetConstantBuffers(0, 1, &s_cb);
			a_ctx->PSSetShaderResources(0, 1, &s_srv);
			a_ctx->PSSetSamplers(0, 1, &s_samp);
			if (s_ds) a_ctx->OMSetDepthStencilState(s_ds, 0);

			s_origDrawIndexed(a_ctx, a_count, a_start, a_base);

			// 恢复：必须把原来的 PS 还回去，否则绿色会泄漏到天空等后续绘制
			if (oldPS) {
				a_ctx->PSSetShader(oldPS, nullptr, 0);
				oldPS->Release();
			} else {
				static int s_nullLog = 0;
				if (s_nullLog < 3) { s_nullLog++; Log("[LENSDRAW] oldPS was NULL"); }
			}
			if (oldDS) { a_ctx->OMSetDepthStencilState(oldDS, oldStencilRef); oldDS->Release(); }
		}
	}

	static void STDMETHODCALLTYPE hkDrawIndexed(ID3D11DeviceContext* a_ctx, UINT a_count, UINT a_start, INT a_base)
	{
		// 照 TTS 的 IsTargetDrawCall：用「索引数 + 顶点 stride」双重匹配。
		// 光比索引数会误伤（96 有两个几何体、384 和天空撞车），
		// stride 才是我们圆盘特有的指纹（TTS 的 TARGET_STRIDE = 28）。
		if (s_diskIndexCount > 0 && a_count == s_diskIndexCount && g_ads.load()) {
			ID3D11Buffer* vb = nullptr;
			UINT stride = 0, off = 0;
			a_ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &off);
			if (vb) vb->Release();

			if (stride == 28) {
				static int s_hitLog = 0;
				if (s_hitLog < 5) {
					s_hitLog++;
					Log("[DRAW] disk MATCH count=%u stride=%u", a_count, stride);
				}
				LensDraw::Draw(a_ctx, a_count, a_start, a_base);
				return;
			}
		}
		s_origDrawIndexed(a_ctx, a_count, a_start, a_base);
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
			static bool s_fovSaved = false;
			static float s_origFov = 0.0f;

			const bool ads = IsInADS(a_pc);
			g_ads.store(ads);
			if (ads) s_frame++;

			if (ads) {
				// 每帧算镜片投影（诊断用）
				UpdateMask(a_pc);

				// 回退到 r73 的行为：强制 Lense 可见（覆盖 STS 剔除）
				ForceLenseVisible(a_pc);

				// 透镜只在开镜时可见
				if (s_lensNode && s_lensNode->GetAppCulled()) {
					s_lensNode->SetAppCulled(false);
				}

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

				// FOV 覆盖：>0 时强制用这个值（菜单里调）；进入 ADS 时记下原值
				if (g_fovOverride > 1.0f) {
					if (auto* cam = RE::PlayerCamera::GetSingleton()) {
						if (!s_fovSaved) {
							s_origFov = cam->firstPersonFOV;
							s_fovSaved = true;
						}
						cam->firstPersonFOV = g_fovOverride;
					}
				}

				if (!s_lastADS) {
					// 进入开镜：读遮罩索引数 + 挂上覆盖"洞"的圆盘透镜
					s_lensIndexCount = 0;
					CacheLenseIndexCount(a_pc);
					SpawnLens();

					// 列出 ScopeAiming 下所有几何体：ADS 时真正渲染的是它们，
					// ScopeNormal 下的 Lense 已被 STS 剔除。
					{
						static int s_dumpShapes = 0;
						if (s_dumpShapes < 2) {
							s_dumpShapes++;
							if (auto* root3d = a_pc->Get3D()) {
								if (auto* aiming = root3d->GetObjectByName("ScopeAiming")) {
									DumpShapes(aiming, 0);
								} else {
									Log("[SHAPE] ScopeAiming not found");
								}
							}
						}
					}

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
				// 收镜：透镜隐藏，避免不开镜也能看见准星
				if (s_lensNode) s_lensNode->SetAppCulled(true);
				// 退出开镜：把相机 FOV 还给游戏（掏枪/收镜后就是镜子自己的默认值）
				if (s_fovSaved) {
					if (auto* cam = RE::PlayerCamera::GetSingleton()) {
						cam->firstPersonFOV = s_origFov;
					}
					s_fovSaved = false;
				}
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

		// 回到屏幕层方案（r37 状态）：准星固定在屏幕中心，圆形遮罩可调。
		// 3D 接管那套因为"索引数无法唯一标识几何体"而误伤天空，已停用。
		OverlayDraw(a_sc);
		return s_origPresent(a_sc, a_sync, a_flags);
	}

	// ======================= ClipCursor =======================
	// 游戏会把鼠标限制在一个小矩形里（瞄准时尤其明显）。菜单打开时必须
	// 放行 ClipCursor，否则鼠标只能在那块小区域里动 —— 这就是"F7 后鼠标
	// 只能在中间一小块动"的原因。
	using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
	static ClipCursor_t s_origClipCursor = nullptr;

	static BOOL WINAPI hkClipCursor(const RECT* a_rect)
	{
		if (ImGui_menuOpen()) {
			return s_origClipCursor(nullptr);
		}
		return s_origClipCursor(a_rect);
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

		// 菜单打开时放行鼠标裁剪
		if (MH_CreateHook((LPVOID)&ClipCursor, (LPVOID)&hkClipCursor, (LPVOID*)&s_origClipCursor) == MH_OK) {
			MH_EnableHook((LPVOID)&ClipCursor);
			Log("[INIT] ClipCursor hooked");
		} else {
			Log("[INIT] ClipCursor hook FAILED");
		}

		// 透镜绘制识别：hook ID3D11DeviceContext::DrawIndexed (vtable[12])
		{
			auto* rdGi = RE::BSGraphics::RendererData::GetSingleton();
			if (rdGi && rdGi->context) {
				auto* ctx = (ID3D11DeviceContext*)static_cast<void*>(rdGi->context);
				void** vtCtx = *(void***)ctx;
				if (MH_CreateHook(vtCtx[12], (LPVOID)&hkDrawIndexed, (LPVOID*)&s_origDrawIndexed) == MH_OK) {
					MH_EnableHook(vtCtx[12]);
					Log("[INIT] DrawIndexed hooked @ %p", vtCtx[12]);
					LensDraw::Init((ID3D11Device*)static_cast<void*>(rdGi->device));
				} else {
					Log("[INIT] DrawIndexed hook FAILED");
				}
			} else {
				Log("[INIT] context NULL (no DrawIndexed hook)");
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
