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

		// 诊断：记录 Weapon 指针与已挂载实例的关系
		Log("[LENS] spawn check: weaponNode=%p | s_lensNode=%p | parent=%p | same=%d",
			(void*)weaponNode, (void*)s_lensNode,
			s_lensNode ? (void*)s_lensNode->parent : nullptr,
			(s_lensNode && s_lensNode->parent == weaponNode) ? 1 : 0);

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

	// ======================= ScopeFade 只读诊断 =======================
	// 一次性捕获 ScopeFade 候选 draw 的 VB/IB 身份与原始数据。
	// 遵守约束：不 Map 游戏缓冲（用 staging + CopyResource）、只捕获一次、
	// 不逐帧、不改变任何渲染状态、释放所有 COM 引用。
	// ======================= Gate A：同帧 Draw 时间线诊断 =======================
	// F10 启动约 60 帧窗口，记录 nif(96) / ScopeFade(144) / Lense(384) 三个候选的
	// 绘制顺序、pass 归属、DSV 完整信息。只读，不改任何绘制。
	static std::atomic<bool> s_timelineActive{ false };
	static std::atomic<int>  s_timelineFrames{ 0 };
	static int s_timelineSeq = 0;
	static int s_dsProbeCount = 0;
	static int s_candLog = 0;
	static int s_candFrame = 0;
	static bool s_frameStats = true;
	static int s_frameDraws = 0;
	static int s_frameStencilOn = 0;
	static int s_stencilOnLog = 0;

	// ======================= 红/绿诊断（确定哪片是目镜） =======================
	// 把 Lense 的 384 索引拆成两次绘制：B(index 0..191) 染红，A(index 192..383) 染绿。
	// 保留原 VS / 输入布局 / 变换，只替换 PS。
	static ID3D11PixelShader* s_psRed = nullptr;
	static ID3D11PixelShader* s_psCross = nullptr;   // 屏幕中心十字（用 SV_Position）
	static ID3D11Buffer* s_cbCross = nullptr;
	static ID3D11DepthStencilState* s_dsNoStencil = nullptr;

	struct CrossCb { float centerX, centerY, halfW, halfH; };
	static ID3D11PixelShader* s_psGreen = nullptr;

	static void InitDiagShaders(ID3D11Device* a_dev)
	{
		if (s_psRed || !a_dev) return;
		const char* kRed = "float4 main(float4 p : SV_POSITION) : SV_Target { return float4(1,0,0,1); }";
		const char* kGrn = "float4 main(float4 p : SV_POSITION) : SV_Target { return float4(0,1,0,1); }";
		ID3DBlob* b = nullptr; ID3DBlob* e = nullptr;
		if (SUCCEEDED(D3DCompile(kRed, strlen(kRed), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, &e)) && b) {
			a_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s_psRed);
			b->Release();
		}
		if (e) { e->Release(); e = nullptr; }
		b = nullptr;
		if (SUCCEEDED(D3DCompile(kGrn, strlen(kGrn), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, &e)) && b) {
			a_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s_psGreen);
			b->Release();
		}
		if (e) e->Release();
		Log("[DIAG] shaders red=%d green=%d", s_psRed ? 1 : 0, s_psGreen ? 1 : 0);

		// 十字准星 PS：用 SV_Position 算屏幕中心，非准星像素 discard
		const char* kCross = R"(
cbuffer Cb : register(b0) {
    float centerX, centerY, halfW, halfH;
};
float4 main(float4 pos : SV_POSITION) : SV_Target {
    float dx = pos.x - centerX;
    float dy = pos.y - centerY;
    // 十字：水平线（|dy|<=halfH）或 垂直线（|dx|<=halfW）
    bool onH = (abs(dy) <= halfH) && (abs(dx) <= 200.0);
    bool onV = (abs(dx) <= halfW) && (abs(dy) <= 200.0);
    if (!onH && !onV) discard;
    return float4(1.0, 0.0, 0.0, 1.0);
}
)";
		b = nullptr; e = nullptr;
		if (SUCCEEDED(D3DCompile(kCross, strlen(kCross), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, &e)) && b) {
			a_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s_psCross);
			b->Release();
		}
		if (e) e->Release();

		// 十字常量缓冲
		D3D11_BUFFER_DESC cbd{};
		cbd.ByteWidth = 16;   // 4 floats
		cbd.Usage = D3D11_USAGE_DYNAMIC;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		a_dev->CreateBuffer(&cbd, nullptr, &s_cbCross);

		// 禁用 stencil 的深度状态（保留深度测试、不写深度）
		D3D11_DEPTH_STENCIL_DESC dsC{};
		dsC.DepthEnable = TRUE;
		dsC.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dsC.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		dsC.StencilEnable = FALSE;
		a_dev->CreateDepthStencilState(&dsC, &s_dsNoStencil);

		Log("[DIAG] cross ps=%d cb=%d dsNoStencil=%d", s_psCross ? 1 : 0, s_cbCross ? 1 : 0, s_dsNoStencil ? 1 : 0);
	}

	// ======================= 受控抑制实验（身份验证） =======================
	// 按 F8 启动一个约 60 帧的抑制窗口：只跳过「144 索引 + PS 非空 + stride 20」
	// 的颜色 pass draw。用途是 A/B 对比确认该 draw 是否就是 ScopeFade。
	// 规则：默认关闭、有明确帧数上限、到期自动恢复、不改任何渲染状态。
	static std::atomic<bool> s_suppressActive{ false };
	static std::atomic<int>  s_suppressFrames{ 0 };
	static std::atomic<int>  s_suppressSkipped{ 0 };
	static std::atomic<int>  s_suppressWhich{ 0 };   // 0=144模式 1=384低voff 2=384高voff

	static bool s_fadeCaptured = false;
	static bool s_lenseCaptured = false;   // 384（Lense:0）的一次性捕获

	// ======================= r163: P1/P2 每条 384 draw 的身份槽位 =======================
	// 声明提前：draw hook 在本段之前就要用到它们，实现放在文件后半部分。
	struct LenseDrawSlot
	{
		ID3D11Buffer* vb = nullptr;
		UINT voff = 0;
		bool used = false;
		bool captured = false;
	};
	static LenseDrawSlot s_lenseSlots[4];
	static std::vector<uint32_t> s_slotIdx[4];
	static std::vector<float>    s_slotPos[4];   // x,y,z triples
	static int s_lenseSlotHit = 0;

	// r165: 每个 slot 的 depth pass / color pass 状态各取证一次，并排对比
	static bool s_passStateLogged[4][2] = {};    // [slot][0=depth, 1=color]

	static int FindOrAddLenseSlot(ID3D11Buffer* a_vb, UINT a_voff);
	static void ExamineLenseDraw(ID3D11DeviceContext* a_ctx, UINT a_count, UINT a_start, INT a_base, int a_slot);
	static void ExaminePassState(ID3D11DeviceContext* a_ctx, int a_slot, bool a_isColor, UINT a_voff);

	static void DumpIlFor(void* a_ptr);

	static void CaptureScopeFade(ID3D11DeviceContext* a_ctx, UINT a_count, UINT a_start, INT a_base)
	{
		ID3D11Device* dev = nullptr;
		a_ctx->GetDevice(&dev);
		if (!dev) { Log("[CAP] GetDevice FAILED"); return; }

		// --- 身份 ---
		ID3D11Buffer* vb = nullptr;
		UINT stride = 0, voff = 0;
		a_ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);

		ID3D11Buffer* ib = nullptr;
		DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
		UINT ibOff = 0;
		a_ctx->IAGetIndexBuffer(&ib, &ibFmt, &ibOff);

		D3D11_BUFFER_DESC vbd{}, ibd{};
		if (vb) vb->GetDesc(&vbd);
		if (ib) ib->GetDesc(&ibd);

		Log("[CAP] draw: count=%u start=%u base=%d", a_count, a_start, a_base);
		Log("[CAP] VB ptr=%p bytes=%u usage=%u cpu=0x%X bind=0x%X stride=%u voff=%u",
			(void*)vb, vbd.ByteWidth, vbd.Usage, vbd.CPUAccessFlags, vbd.BindFlags, stride, voff);
		Log("[CAP] IB ptr=%p bytes=%u usage=%u cpu=0x%X bind=0x%X fmt=%d offset=%u",
			(void*)ib, ibd.ByteWidth, ibd.Usage, ibd.CPUAccessFlags, ibd.BindFlags, (int)ibFmt, ibOff);
		Log("[CAP] vbOff=%u ibOff=%u diff=%lld", voff, ibOff, (long long)ibOff - (long long)voff);

		// 当前绑定的 shader / input layout —— 用于身份交叉验证
		ID3D11VertexShader* vs = nullptr;
		ID3D11PixelShader* ps = nullptr;
		ID3D11InputLayout* il = nullptr;
		a_ctx->VSGetShader(&vs, nullptr, nullptr);
		a_ctx->PSGetShader(&ps, nullptr, nullptr);
		a_ctx->IAGetInputLayout(&il);
		Log("[CAP] VS=%p PS=%p IL=%p", (void*)vs, (void*)ps, (void*)il);
		if (il) DumpIlFor(il);
		if (vs) vs->Release();
		if (ps) ps->Release();
		if (il) il->Release();

		// 光栅/混合/深度状态标识
		ID3D11RasterizerState* rs = nullptr;
		ID3D11BlendState* bs = nullptr;
		ID3D11DepthStencilState* ds = nullptr;
		a_ctx->RSGetState(&rs);
		FLOAT blendFactor[4] = {};
		UINT sampleMask = 0;
		a_ctx->OMGetBlendState(&bs, blendFactor, &sampleMask);
		UINT stencilRef = 0;
		a_ctx->OMGetDepthStencilState(&ds, &stencilRef);
		Log("[CAP] RS=%p BS=%p DS=%p stencilRef=%u blend=(%.2f,%.2f,%.2f,%.2f)",
			(void*)rs, (void*)bs, (void*)ds, stencilRef,
			blendFactor[0], blendFactor[1], blendFactor[2], blendFactor[3]);

		// --- 实验 A：渲染阶段分类（RTV/DSV + 各状态描述 + viewport）---
		ID3D11RenderTargetView* rtvs[4] = {};
		ID3D11DepthStencilView* dsv = nullptr;
		a_ctx->OMGetRenderTargets(4, rtvs, &dsv);

		int rtvCount = 0;
		for (int i = 0; i < 4; ++i) if (rtvs[i]) rtvCount++;
		Log("[CAP] RTV count=%d", rtvCount);
		for (int i = 0; i < 4; ++i) {
			if (!rtvs[i]) continue;
			ID3D11Resource* res = nullptr;
			rtvs[i]->GetResource(&res);
			if (res) {
				D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
				res->GetType(&dim);
				if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
					D3D11_TEXTURE2D_DESC td{};
					((ID3D11Texture2D*)res)->GetDesc(&td);
					Log("[CAP] RTV[%d] %ux%u fmt=%d bind=0x%X mips=%u samples=%u",
						i, td.Width, td.Height, (int)td.Format, td.BindFlags, td.MipLevels, td.SampleDesc.Count);
				} else {
					Log("[CAP] RTV[%d] non-2D (dim=%d)", i, (int)dim);
				}
				res->Release();
			}
			rtvs[i]->Release();
		}
		if (dsv) {
			// 分两项：DSV view 格式（typed）vs 底层资源格式（可能是 typeless）
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
			dsv->GetDesc(&dsvd);
			Log("[CAP] DSV view: fmt=%d dim=%d", (int)dsvd.Format, (int)dsvd.ViewDimension);

			ID3D11Resource* res = nullptr;
			dsv->GetResource(&res);
			if (res) {
				D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
				res->GetType(&dim);
				if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
					D3D11_TEXTURE2D_DESC td{};
					((ID3D11Texture2D*)res)->GetDesc(&td);
					Log("[CAP] DSV resource: %ux%u fmt=%d bind=0x%X samples=%u",
						td.Width, td.Height, (int)td.Format, td.BindFlags, td.SampleDesc.Count);
				}
				res->Release();
			}
			dsv->Release();
		} else {
			Log("[CAP] DSV null");
		}

		// BlendState 完整描述（RT0-RT3 各自）
		if (bs) {
			D3D11_BLEND_DESC bd{};
			bs->GetDesc(&bd);
			Log("[CAP] Blend IndependentBlend=%d AlphaToCoverage=%d", bd.IndependentBlendEnable, bd.AlphaToCoverageEnable);
			for (int i = 0; i < 4; ++i) {
				const auto& rt = bd.RenderTarget[i];
				Log("[CAP] BlendRT[%d] en=%d src=%d dst=%d op=%d aSrc=%d aDst=%d aOp=%d mask=0x%X",
					i, rt.BlendEnable, (int)rt.SrcBlend, (int)rt.DestBlend, (int)rt.BlendOp,
					(int)rt.SrcBlendAlpha, (int)rt.DestBlendAlpha, (int)rt.BlendOpAlpha,
					rt.RenderTargetWriteMask);
			}
		}

		// DepthStencilState 完整描述（含双面 stencil ops 和 ref）
		if (ds) {
			D3D11_DEPTH_STENCIL_DESC dsd{};
			ds->GetDesc(&dsd);
			Log("[CAP] Depth: enable=%d writeMask=%d func=%d | stencilEnable=%d readMask=0x%X writeMask=0x%X ref=%u",
				dsd.DepthEnable, (int)dsd.DepthWriteMask, (int)dsd.DepthFunc,
				dsd.StencilEnable, dsd.StencilReadMask, dsd.StencilWriteMask, stencilRef);
			Log("[CAP] Stencil Front: func=%d fail=%d dfail=%d pass=%d",
				(int)dsd.FrontFace.StencilFunc, (int)dsd.FrontFace.StencilFailOp,
				(int)dsd.FrontFace.StencilDepthFailOp, (int)dsd.FrontFace.StencilPassOp);
			Log("[CAP] Stencil Back:  func=%d fail=%d dfail=%d pass=%d",
				(int)dsd.BackFace.StencilFunc, (int)dsd.BackFace.StencilFailOp,
				(int)dsd.BackFace.StencilDepthFailOp, (int)dsd.BackFace.StencilPassOp);
		}

		// RasterizerState 描述
		if (rs) {
			D3D11_RASTERIZER_DESC rd{};
			rs->GetDesc(&rd);
			Log("[CAP] Rast: fill=%d cull=%d frontCCW=%d depthClip=%d scissor=%d",
				(int)rd.FillMode, (int)rd.CullMode, rd.FrontCounterClockwise,
				rd.DepthClipEnable, rd.ScissorEnable);
		}

		// Viewport
		D3D11_VIEWPORT vp{};
		UINT vpCount = 1;
		a_ctx->RSGetViewports(&vpCount, &vp);
		if (vpCount > 0) {
			Log("[CAP] Viewport: %.1f,%.1f %.1fx%.1f minZ=%.3f maxZ=%.3f",
				vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth);
		}

		if (rs) rs->Release();
		if (bs) bs->Release();
		if (ds) ds->Release();

		// --- 只读：staging 拷贝索引缓冲 ---
		const UINT ibBytesPerIndex = (ibFmt == DXGI_FORMAT_R32_UINT) ? 4u : 2u;
		const UINT ibReadBytes = a_count * ibBytesPerIndex;
		const UINT ibReadOff = ibOff + a_start * ibBytesPerIndex;

		if (ib && ibReadBytes > 0 && ibReadBytes < 65536 && ibReadOff + ibReadBytes <= ibd.ByteWidth) {
			D3D11_BUFFER_DESC sd{};
			sd.ByteWidth = ibReadBytes;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.BindFlags = 0;
			ID3D11Buffer* staging = nullptr;
			if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &staging)) && staging) {
				D3D11_BOX box{};
				box.left = ibReadOff;
				box.right = ibReadOff + ibReadBytes;
				box.top = 0; box.bottom = 1;
				box.front = 0; box.back = 1;
				a_ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, ib, 0, &box);

				D3D11_MAPPED_SUBRESOURCE ms{};
				if (SUCCEEDED(a_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms))) {
					const auto* idx = (const uint8_t*)ms.pData;
					// 完整读取全部索引（上一版只读了 96 个 / 日志被 400 字节缓冲截断）
					UINT mn = 0xFFFF, mx = 0;
					bool seen[65536] = {};
					UINT uniq = 0;
					// 分多行输出，每行 32 个，避免缓冲区截断
					const UINT perLine = 32;
					for (UINT base = 0; base < a_count; base += perLine) {
						char line[320] = {};
						int w = 0;
						for (UINT i = base; i < base + perLine && i < a_count; ++i) {
							UINT v = (ibBytesPerIndex == 4)
								? *((const uint32_t*)(idx + i * 4))
								: *((const uint16_t*)(idx + i * 2));
							if (v < mn) mn = v;
							if (v > mx) mx = v;
							if (!seen[v]) { seen[v] = true; ++uniq; }
							w += sprintf_s(line + w, sizeof(line) - w, "%u ", v);
						}
						Log("[CAP] IB[%u..%u] %s", base, (base + perLine > a_count ? a_count : base + perLine), line);
					}
					Log("[CAP] IB stats: min=%u max=%u unique=%u", mn, mx, uniq);
					a_ctx->Unmap(staging, 0);
				} else {
					Log("[CAP] Map(IB staging) FAILED");
				}
				staging->Release();
			} else {
				Log("[CAP] CreateBuffer(IB staging) FAILED");
			}
		}

		// --- 只读：staging 拷贝顶点缓冲（48 × 20 = 960 字节，完整导出）---
		// 按 stride 读足够多的顶点（Lense 有 82 个顶点，之前固定 960 只够 48 个）
		const UINT vbWant = stride * 200;
		const UINT vbReadBytes = (vbd.ByteWidth < vbWant) ? vbd.ByteWidth : vbWant;
		if (vb && vbReadBytes > 0 && stride > 0) {
			D3D11_BUFFER_DESC sd{};
			sd.ByteWidth = vbReadBytes;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.BindFlags = 0;
			ID3D11Buffer* staging = nullptr;
			if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &staging)) && staging) {
				D3D11_BOX box{};
				box.left = voff;
				box.right = voff + vbReadBytes;
				box.top = 0; box.bottom = 1;
				box.front = 0; box.back = 1;
				a_ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, vb, 0, &box);

				D3D11_MAPPED_SUBRESOURCE ms{};
				if (SUCCEEDED(a_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms))) {
					const auto* raw = (const uint8_t*)ms.pData;
					const UINT nVerts = vbReadBytes / stride;
					Log("[CAP] VB %u vertices (stride=%u), raw hex:", nVerts, stride);
					for (UINT v = 0; v < nVerts; ++v) {
						char hex[400];
						int w = 0;
						for (UINT b = 0; b < stride && w < 380; ++b) {
							w += sprintf_s(hex + w, sizeof(hex) - w, "%02X ", raw[v * stride + b]);
						}
						Log("[CAP] v%u: %s", v, hex);
					}
					// 每个 4 字节区段的候选统计（不命名 POSITION，只给数据）
					Log("[CAP] per-4byte-slot stats (candidate only):");
					for (UINT f = 0; f + 4 <= stride; f += 4) {
						float mnF = 1e30f, mxF = -1e30f;
						bool anyFinite = false;
						for (UINT v = 0; v < nVerts; ++v) {
							float fv = 0.0f;
							memcpy(&fv, raw + v * stride + f, 4);
							if (fv > -1e37f && fv < 1e37f) {
								anyFinite = true;
								if (fv < mnF) mnF = fv;
								if (fv > mxF) mxF = fv;
							}
						}
						if (anyFinite) {
							Log("[CAP] slot[%u]: min=%.5f max=%.5f", f, mnF, mxF);
						} else {
							Log("[CAP] slot[%u]: no finite float range (likely packed/normal data)", f);
						}
					}
					a_ctx->Unmap(staging, 0);
				} else {
					Log("[CAP] Map(VB staging) FAILED");
				}
				staging->Release();
			} else {
				Log("[CAP] CreateBuffer(VB staging) FAILED");
			}
		}

		if (vb) vb->Release();
		if (ib) ib->Release();
		dev->Release();
		Log("[CAP] done");
	}

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
    // 圆盘内采样准星贴图，圆外丢弃。center/size 是屏幕像素坐标。
    float2 uv = (pos.xy - float2(centerX, centerY)) / size + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        discard;
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

			static int s_drawLog = 0;
			if (s_drawLog < 5) {
				s_drawLog++;
				const auto& gs0 = RE::BSGraphics::State::GetSingleton();
				Log("[LENSDRAW] Draw #%d count=%u start=%u base=%d screen=%ux%u size=%.1f center=(%.1f,%.1f)",
					s_drawLog, a_count, a_start, a_base,
					gs0.backBufferWidth, gs0.backBufferHeight,
					g_reticleSize * g_reticleScale,
					g_reticleSize * g_reticleScale * 0 + (float)gs0.backBufferWidth * 0.5f + g_reticleOffX,
					(float)gs0.backBufferHeight * 0.5f + g_reticleOffY);
			}

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

			// 保存 SRV / Sampler / CB —— 只恢复 PS 不够，任何一项泄漏都会污染后续绘制
			ID3D11ShaderResourceView* oldSRV = nullptr;
			a_ctx->PSGetShaderResources(0, 1, &oldSRV);
			ID3D11SamplerState* oldSamp = nullptr;
			a_ctx->PSGetSamplers(0, 1, &oldSamp);
			ID3D11Buffer* oldCB = nullptr;
			a_ctx->PSGetConstantBuffers(0, 1, &oldCB);

			a_ctx->PSSetShader(s_ps, nullptr, 0);
			a_ctx->PSSetConstantBuffers(0, 1, &s_cb);
			a_ctx->PSSetShaderResources(0, 1, &s_srv);
			a_ctx->PSSetSamplers(0, 1, &s_samp);
			if (s_ds) a_ctx->OMSetDepthStencilState(s_ds, 0);

			s_origDrawIndexed(a_ctx, a_count, a_start, a_base);

			// 恢复：PS + SRV + Sampler + CB 全部还原，否则会污染后续绘制（天空变绿就是这么来的）
			if (oldPS) {
				a_ctx->PSSetShader(oldPS, nullptr, 0);
				oldPS->Release();
			} else {
				// 关键：原 PS 本来就为空（深度预 pass）时，必须显式设回 null，
				// 否则我们的 PS 会一直留在上下文里，污染后续所有绘制（天空变红就是这么来的）
				a_ctx->PSSetShader(nullptr, nullptr, 0);
				static int s_nullLog = 0;
				if (s_nullLog < 3) { s_nullLog++; Log("[LENSDRAW] oldPS was NULL (restored to null)"); }
			}
			// SRV 恢复：oldSRV 可能为 null，此时必须显式设 null 解绑
			a_ctx->PSSetShaderResources(0, 1, &oldSRV);
			if (oldSRV) oldSRV->Release();
			a_ctx->PSSetSamplers(0, 1, &oldSamp);
			if (oldSamp) oldSamp->Release();
			// CB 恢复：slot 0 被换成我们的 Cb，不还回去后续 PS 会拿到错误参数
			a_ctx->PSSetConstantBuffers(0, 1, &oldCB);
			if (oldCB) oldCB->Release();

			if (oldDS) { a_ctx->OMSetDepthStencilState(oldDS, oldStencilRef); oldDS->Release(); }
		}
	}

	static void STDMETHODCALLTYPE hkDrawIndexed(ID3D11DeviceContext* a_ctx, UINT a_count, UINT a_start, INT a_base)
	{
		// 菜单持久剔除（ImGui 勾选，非一次性窗口）
		if (g_ads.load()) {
			ID3D11Buffer* vbP = nullptr;
			UINT strideP = 0, voffP = 0;
			a_ctx->IAGetVertexBuffers(0, 1, &vbP, &strideP, &voffP);
			if (vbP) vbP->Release();
			ID3D11PixelShader* psP = nullptr;
			a_ctx->PSGetShader(&psP, nullptr, nullptr);
			const bool colorPass = (psP != nullptr);
			if (psP) psP->Release();

			if (g_supLense && a_count == 384 && strideP == 20 && colorPass) return;
			if (g_sup144 && a_count == 144 && colorPass) return;
			if (g_sup384A && a_count == 384 && strideP == 20 && voffP < 10000000u && colorPass) return;
			if (g_sup384B && a_count == 384 && strideP == 20 && voffP >= 10000000u && colorPass) return;
			if (g_supNif && a_count == 96 && strideP == 24) return;
		}

		// Gate A 时间线：F10 窗口内记录 nif(96)/ScopeFade(144)/Lense(384) 的绘制顺序
		if (s_timelineActive.load() && (a_count == 96 || a_count == 144 || a_count == 384)) {
			ID3D11PixelShader* psTl = nullptr;
			ID3D11VertexShader* vsTl = nullptr;
			ID3D11InputLayout* ilTl = nullptr;
			a_ctx->PSGetShader(&psTl, nullptr, nullptr);
			a_ctx->VSGetShader(&vsTl, nullptr, nullptr);
			a_ctx->IAGetInputLayout(&ilTl);

			ID3D11Buffer* vbTl = nullptr;
			UINT strideTl = 0, voffTl = 0;
			a_ctx->IAGetVertexBuffers(0, 1, &vbTl, &strideTl, &voffTl);
			ID3D11Buffer* ibTl = nullptr;
			DXGI_FORMAT fmtTl = DXGI_FORMAT_UNKNOWN;
			UINT ioffTl = 0;
			a_ctx->IAGetIndexBuffer(&ibTl, &fmtTl, &ioffTl);

			ID3D11DepthStencilView* dsvTl = nullptr;
			a_ctx->OMGetRenderTargets(1, nullptr, &dsvTl);
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
			int dsvViewFmt = -1;
			int dsvResFmt = -1;
			if (dsvTl) {
				dsvTl->GetDesc(&dsvd);
				dsvViewFmt = (int)dsvd.Format;
				ID3D11Resource* res = nullptr;
				dsvTl->GetResource(&res);
				if (res) {
					D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
					res->GetType(&dim);
					if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
						D3D11_TEXTURE2D_DESC td{};
						((ID3D11Texture2D*)res)->GetDesc(&td);
						dsvResFmt = (int)td.Format;
					}
					res->Release();
				}
				dsvTl->Release();
			}

			ID3D11DepthStencilState* dsTl = nullptr;
			UINT stencilRefTl = 0;
			a_ctx->OMGetDepthStencilState(&dsTl, &stencilRefTl);
			// GPT 指正：stencilRef 只是引用值，不等于 StencilEnable。这里完整输出描述。
			int stEnable = -1, stReadMask = -1, stWriteMask = -1;
			int stFrontFunc = -1, stFrontPassOp = -1, stBackFunc = -1, stBackPassOp = -1;
			int dEnable = -1, dWriteMask = -1, dFunc = -1;
			if (dsTl) {
				D3D11_DEPTH_STENCIL_DESC dsd{};
				dsTl->GetDesc(&dsd);
				stEnable = dsd.StencilEnable ? 1 : 0;
				stReadMask = dsd.StencilReadMask;
				stWriteMask = dsd.StencilWriteMask;
				stFrontFunc = (int)dsd.FrontFace.StencilFunc;
				stFrontPassOp = (int)dsd.FrontFace.StencilPassOp;
				stBackFunc = (int)dsd.BackFace.StencilFunc;
				stBackPassOp = (int)dsd.BackFace.StencilPassOp;
				dEnable = dsd.DepthEnable ? 1 : 0;
				dWriteMask = (int)dsd.DepthWriteMask;
				dFunc = (int)dsd.DepthFunc;
				dsTl->Release();
			}

			Log("[TL] #%d count=%u stride=%u voff=%u ioff=%u ps=%p vs=%p il=%p | dsViewFmt=%d dsResFmt=%d ref=%u",
				s_timelineSeq++, a_count, strideTl, voffTl, ioffTl,
				(void*)psTl, (void*)vsTl, (void*)ilTl,
				dsvViewFmt, dsvResFmt, stencilRefTl);
			Log("[TL]    stencilEnable=%d readMask=0x%X writeMask=0x%X frontFunc=%d frontPass=%d backFunc=%d backPass=%d | depthEnable=%d depthWrite=%d depthFunc=%d",
				stEnable, stReadMask, stWriteMask, stFrontFunc, stFrontPassOp, stBackFunc, stBackPassOp,
				dEnable, dWriteMask, dFunc);

			if (psTl) psTl->Release();
			if (vsTl) vsTl->Release();
			if (ilTl) ilTl->Release();
			if (vbTl) vbTl->Release();
			if (ibTl) ibTl->Release();
		}

		// 隔离诊断：B-only / A-only，每次只画一片，验证各自独立可见性
		if ((g_diagBOnly || g_diagAOnly) && a_count == 384 && g_ads.load()) {
			ID3D11PixelShader* psIso = nullptr;
			a_ctx->PSGetShader(&psIso, nullptr, nullptr);
			const bool colorIso = (psIso != nullptr);
			if (psIso) psIso->Release();
			if (colorIso && s_psRed && s_psGreen) {
				static int s_isoLog = 0;
				if (s_isoLog < 6) {
					s_isoLog++;
					Log("[DIAG] isolate %s start=%u base=%d", g_diagBOnly ? "B(red)" : "A(green)", a_start, a_base);
				}
				if (g_diagBOnly) {
					a_ctx->PSSetShader(s_psRed, nullptr, 0);
					s_origDrawIndexed(a_ctx, 192, a_start, a_base);
				} else {
					a_ctx->PSSetShader(s_psGreen, nullptr, 0);
					s_origDrawIndexed(a_ctx, 192, a_start + 192, a_base);
				}
				a_ctx->PSSetShader(nullptr, nullptr, 0);
				return;
			}
		}

		// 状态对照：抓普通几何（非 384）的 DepthStencil 状态，判断 REPLACE/ref=1 是否通用
		// 候选 Draw 记录：无条件记录所有 384/start=0 的调用（含是否被十字接管）
		if (g_ads.load() && a_count == 384 && a_start == 0) {
			if (s_candLog < 20) {
				s_candLog++;
				ID3D11Buffer* vbC = nullptr;
				UINT strideC = 0, voffC = 0;
				a_ctx->IAGetVertexBuffers(0, 1, &vbC, &strideC, &voffC);
				if (vbC) vbC->Release();
				ID3D11Buffer* ibC = nullptr;
				DXGI_FORMAT fmtC = DXGI_FORMAT_UNKNOWN;
				UINT ioffC = 0;
				a_ctx->IAGetIndexBuffer(&ibC, &fmtC, &ioffC);
				if (ibC) ibC->Release();
				ID3D11PixelShader* psC = nullptr;
				a_ctx->PSGetShader(&psC, nullptr, nullptr);
				const bool crossWillTake = g_diagCross && s_psCross && s_cbCross && g_ads.load() && (psC != nullptr) && (strideC == 20);
				Log("[DRAW-CAND] f%d#%d count=%u start=%u base=%d | stride=%u voff=%u | ioff=%u | ps=%s | cross=%s",
					s_candFrame, s_candLog, a_count, a_start, a_base, strideC, voffC, ioffC,
					psC ? "PS" : "null", crossWillTake ? "YES" : "NO");
				if (psC) psC->Release();
			}
		}

		// ======================= r163: 每条 384 颜色 draw 各取证一次 =======================
		// 按 (VB 指针, voff) 去重，最多 4 条；深度预 pass（PS=null）不取。
		// 必须在十字分支之前，因为十字会 return。
		if (g_ads.load() && a_count == 384 && a_start == 0) {
			ID3D11PixelShader* psP = nullptr;
			a_ctx->PSGetShader(&psP, nullptr, nullptr);
			const bool colorP = (psP != nullptr);
			if (psP) psP->Release();
			ID3D11Buffer* vbP = nullptr;
			UINT strideP = 0, voffP = 0;
			a_ctx->IAGetVertexBuffers(0, 1, &vbP, &strideP, &voffP);
			// r165: depth pass 与 color pass 都走这里，各记录一次状态（并排对比）
			if (strideP == 20) {
				const int slot = FindOrAddLenseSlot(vbP, voffP);
				if (slot >= 0) {
					ExaminePassState(a_ctx, slot, colorP, voffP);
					if (colorP && !s_lenseSlots[slot].captured) {
						s_lenseSlots[slot].captured = true;
						++s_lenseSlotHit;
						Log("[P0] 384 color draw #%d new identity -> slot %d (voff=%u)", s_lenseSlotHit, slot, voffP);
						ExamineLenseDraw(a_ctx, a_count, a_start, a_base, slot);
					}
				}
			}
			if (vbP) vbP->Release();
		}

		// 全帧统计：每帧所有 draw 的 stencil 状态
		if (g_ads.load() && s_frameStats) {
			s_frameDraws++;
			ID3D11DepthStencilState* dsS = nullptr;
			UINT refS = 0;
			a_ctx->OMGetDepthStencilState(&dsS, &refS);
			if (dsS) {
				D3D11_DEPTH_STENCIL_DESC dS{};
				dsS->GetDesc(&dS);
				if (dS.StencilEnable) {
					s_frameStencilOn++;
					// 详细记录所有 stencil 启用的 draw（限量）
					if (s_stencilOnLog < 30) {
						s_stencilOnLog++;
						Log("[STE] #%d count=%u | func=%d pass=%d wmask=0x%X ref=%u | depthEn=%d dwrite=%d",
							s_stencilOnLog, a_count,
							(int)dS.FrontFace.StencilFunc, (int)dS.FrontFace.StencilPassOp,
							dS.StencilWriteMask, refS, dS.DepthEnable, (int)dS.DepthWriteMask);
					}
				}
				dsS->Release();
			}
		}

		// 十字诊断：用 B 组几何的投影覆盖范围限制准星（不写 stencil）
		// 先取 voff 判断是否为目标 draw（g_crossTarget 控制接管哪条）
		UINT voffTgt = 0;
		if (g_diagCross && a_count == 384 && a_start == 0 && g_ads.load() && s_psCross && s_cbCross) {
			ID3D11Buffer* vbT = nullptr;
			UINT strideT = 0;
			a_ctx->IAGetVertexBuffers(0, 1, &vbT, &strideT, &voffTgt);
			if (vbT) vbT->Release();
			// r163：不再硬编码具体 voff（缓冲区池重新分配会让数值漂移）。
			// 同一 VB 内两条 384 draw 的偏移差一个量级（实测低 ~7.6e6 / 高 ~1.2e7），
			// 用「低偏移 = A / 高偏移 = B」判定，并把实际值打出来供核对。
			const bool isA = (voffTgt < 10000000u);
			const bool isB = (voffTgt >= 10000000u);
			static UINT s_seenVoffA = 0, s_seenVoffB = 0;
			if (isA && voffTgt != s_seenVoffA) { s_seenVoffA = voffTgt; Log("[CROSS] A voff=%u (low)", voffTgt); }
			if (isB && voffTgt != s_seenVoffB) { s_seenVoffB = voffTgt; Log("[CROSS] B voff=%u (high)", voffTgt); }
			const int tgt = g_crossTarget;
			// 0=both  1=onlyA  2=onlyB  3=none
			// 4=A画十字+B跳过  5=B画十字+A跳过
			// 先判断是不是颜色 pass（深度预 pass 的 ps 为空，必须放行）
			ID3D11PixelShader* psPre = nullptr;
			a_ctx->PSGetShader(&psPre, nullptr, nullptr);
			const bool isColorPass = (psPre != nullptr);
			if (psPre) psPre->Release();

			if (!isColorPass) {
				// 深度预 pass：一律原样转发，不接管也不跳过
				s_origDrawIndexed(a_ctx, a_count, a_start, a_base);
				return;
			}

			const bool wantCross = (tgt == 0) || (tgt == 1 && isA) || (tgt == 2 && isB)
				|| (tgt == 4 && isA) || (tgt == 5 && isB);
			const bool skipMe = (tgt == 4 && isB) || (tgt == 5 && isA);

			if (skipMe) {
				// 只跳过颜色 pass（不画原始黑材质）；深度已在上方放行
				static int s_skipLog = 0;
				if (s_skipLog < 6) { s_skipLog++; Log("[CROSS] skipped color voff=%u (mode %d)", voffTgt, tgt); }
				return;
			}
			if (!wantCross) {
				// P3：none/单侧模式下确认「确实原样转发、没有接管」
				static int s_fwdLog = 0;
				if (s_fwdLog < 6) { s_fwdLog++; Log("[CROSS] forward color voff=%u (mode %d) - untouched", voffTgt, tgt); }
				s_origDrawIndexed(a_ctx, a_count, a_start, a_base);
				return;
			}
		}

		if (g_diagCross && a_count == 384 && a_start == 0 && g_ads.load() && s_psCross && s_cbCross) {
			ID3D11PixelShader* psX = nullptr;
			a_ctx->PSGetShader(&psX, nullptr, nullptr);
			const bool colorX = (psX != nullptr);
			if (psX) psX->Release();

			// 守卫：确认是 Lense 的几何（stride=20，颜色 pass）
			ID3D11Buffer* vbX = nullptr;
			UINT strideX = 0, offX = 0;
			a_ctx->IAGetVertexBuffers(0, 1, &vbX, &strideX, &offX);
			if (vbX) vbX->Release();

			if (colorX && strideX == 20) {
				// 取当前 viewport 中心（必须校验有效性）
				D3D11_VIEWPORT vp{};
				UINT vpN = 1;
				a_ctx->RSGetViewports(&vpN, &vp);
				if (vpN == 0 || !(vp.Width > 0.0f) || !(vp.Height > 0.0f)) {
					static int s_badVp = 0;
					if (s_badVp < 3) { s_badVp++; Log("[CROSS] invalid viewport, skip"); }
					s_origDrawIndexed(a_ctx, a_count, a_start, a_base);   // 回退原始
					return;
				}
				const float cx = vp.TopLeftX + vp.Width * 0.5f;
				const float cy = vp.TopLeftY + vp.Height * 0.5f;

				static int s_crossLog = 0;
				if (s_crossLog < 6) {
					s_crossLog++;
					// 记录 voff / ioff / base，区分命中的是哪次 384
					ID3D11Buffer* vbL = nullptr;
					UINT strideL = 0, voffL = 0;
					a_ctx->IAGetVertexBuffers(0, 1, &vbL, &strideL, &voffL);
					if (vbL) vbL->Release();
					ID3D11Buffer* ibL = nullptr;
					DXGI_FORMAT fmtL = DXGI_FORMAT_UNKNOWN;
					UINT ioffL = 0;
					a_ctx->IAGetIndexBuffer(&ibL, &fmtL, &ioffL);
					if (ibL) ibL->Release();
					Log("[CROSS] vp=(%.0f,%.0f %.0fx%.0f) center=(%.1f,%.1f) | voff=%u ioff=%u stride=%u base=%d",
						vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, cx, cy, voffL, ioffL, strideL, a_base);
				}

				// 写 CB（失败则回退原始绘制，不用过期常量）
				D3D11_MAPPED_SUBRESOURCE ms{};
				HRESULT hrMap = a_ctx->Map(s_cbCross, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
				if (FAILED(hrMap) || !ms.pData) {
					static int s_mapFail = 0;
					if (s_mapFail < 3) { s_mapFail++; Log("[CROSS] Map FAILED 0x%08X, fallback", (unsigned)hrMap); }
					if (SUCCEEDED(hrMap)) a_ctx->Unmap(s_cbCross, 0);
					s_origDrawIndexed(a_ctx, a_count, a_start, a_base);
					return;
				}
				float cb[4] = { cx, cy, 1.0f, 1.0f };
				memcpy(ms.pData, cb, sizeof(cb));
				a_ctx->Unmap(s_cbCross, 0);

				// 保存全部要改的状态
				ID3D11PixelShader* oldPS = nullptr;
				a_ctx->PSGetShader(&oldPS, nullptr, nullptr);
				ID3D11DepthStencilState* oldDS = nullptr;
				UINT oldRef = 0;
				a_ctx->OMGetDepthStencilState(&oldDS, &oldRef);
				ID3D11Buffer* oldCB0 = nullptr;
				a_ctx->PSGetConstantBuffers(0, 1, &oldCB0);

				a_ctx->PSSetShader(s_psCross, nullptr, 0);
				a_ctx->PSSetConstantBuffers(0, 1, &s_cbCross);
				a_ctx->OMSetDepthStencilState(s_dsNoStencil, 0);

				// 只画 B 组
				s_origDrawIndexed(a_ctx, 192, a_start, a_base);

				// 恢复（PS + CB + DS 全部还原）
				if (oldPS) { a_ctx->PSSetShader(oldPS, nullptr, 0); oldPS->Release(); }
				else a_ctx->PSSetShader(nullptr, nullptr, 0);
				a_ctx->PSSetConstantBuffers(0, 1, &oldCB0);
				if (oldCB0) oldCB0->Release();
				if (oldDS) {
					a_ctx->OMSetDepthStencilState(oldDS, oldRef);
					oldDS->Release();
				} else {
					a_ctx->OMSetDepthStencilState(nullptr, oldRef);   // 恢复默认状态
				}
				return;   // 不画原始 384
			}
		}

		// 红/绿诊断：把 384（Lense）拆成两片分别染色，确定哪片是目镜
		if (g_diagSplit && a_count == 384 && g_ads.load()) {
			ID3D11PixelShader* psD = nullptr;
			a_ctx->PSGetShader(&psD, nullptr, nullptr);
			const bool colorPassD = (psD != nullptr);
			if (psD) psD->Release();
			if (colorPassD && s_psRed && s_psGreen) {
				static int s_splitLog = 0;
				if (s_splitLog < 3) {
					s_splitLog++;
					Log("[DIAG] split start=%u base=%d -> B[0..191]=red A[192..383]=green", a_start, a_base);
				}
				// B 组：index 0..191 染红
				a_ctx->PSSetShader(s_psRed, nullptr, 0);
				s_origDrawIndexed(a_ctx, 192, a_start, a_base);
				// A 组：index 192..383 染绿
				a_ctx->PSSetShader(s_psGreen, nullptr, 0);
				s_origDrawIndexed(a_ctx, 192, a_start + 192, a_base);
				// 恢复原 PS
				a_ctx->PSSetShader(nullptr, nullptr, 0);
				return;   // 不画原始的 384，避免覆盖诊断色
			}
		}

		// 实验 B：分别抑制两个 384 候选（按 voff 区分），用于身份辨认
		//   s_suppressWhich == 1 → 只抑制 voff < 10000000 的 384
		//   s_suppressWhich == 2 → 只抑制 voff >= 10000000 的 384
		if (s_suppressActive.load() && a_count == 384 && g_ads.load()) {
			ID3D11Buffer* vbB = nullptr;
			UINT strideB = 0, voffB = 0;
			a_ctx->IAGetVertexBuffers(0, 1, &vbB, &strideB, &voffB);
			if (vbB) vbB->Release();
			ID3D11PixelShader* psB = nullptr;
			a_ctx->PSGetShader(&psB, nullptr, nullptr);
			const bool isColor = (psB != nullptr);
			if (psB) psB->Release();
			if (isColor && strideB == 20) {
				const int which = s_suppressWhich.load();
				const bool match = (which == 1 && voffB < 10000000u) || (which == 2 && voffB >= 10000000u);
				if (match) {
					const int left = s_suppressFrames.fetch_sub(1) - 1;
					s_suppressSkipped.fetch_add(1);
					if (left <= 0) {
						s_suppressActive.store(false);
						Log("[SUPPRESS] 384 window closed, skipped=%d voff=%u", s_suppressSkipped.load(), voffB);
					}
					return;
				}
			}
		}

		// 受控抑制：F8 启动约 60 帧窗口，只跳过「144 索引 + PS 非空 + stride 20」
		// 的颜色 pass draw，用于 A/B 对比身份。
		if (s_suppressActive.load() && a_count == 144 && g_ads.load()) {
			ID3D11PixelShader* psChk = nullptr;
			a_ctx->PSGetShader(&psChk, nullptr, nullptr);
			if (psChk) {
				psChk->Release();
				ID3D11Buffer* vbChk = nullptr;
				UINT strideChk = 0, offChk = 0;
				a_ctx->IAGetVertexBuffers(0, 1, &vbChk, &strideChk, &offChk);
				if (vbChk) vbChk->Release();
				if (strideChk == 20) {
					const int left = s_suppressFrames.fetch_sub(1) - 1;
					s_suppressSkipped.fetch_add(1);
					if (left <= 0) {
						s_suppressActive.store(false);
						Log("[SUPPRESS] window closed, total skipped=%d", s_suppressSkipped.load());
					} else if (left % 10 == 0) {
						Log("[SUPPRESS] skipping, frames left=%d", left);
					}
					return;   // 跳过这次绘制
				}
			}
		}
		// 实验 A：记录后续所有 144 候选 draw 的轻量元数据（最多 8 次，不拷贝缓冲），
		// 用来判断同一网格是否在不同 pass（尤其深度/阴影与颜色）中重复出现。
		static int s_metaCount = 0;
		if (a_count == 144 && g_ads.load() && s_metaCount < 8) {
			s_metaCount++;
			ID3D11PixelShader* psM = nullptr;
			ID3D11VertexShader* vsM = nullptr;
			ID3D11RenderTargetView* rtvM = nullptr;
			ID3D11DepthStencilView* dsvM = nullptr;
			a_ctx->PSGetShader(&psM, nullptr, nullptr);
			a_ctx->VSGetShader(&vsM, nullptr, nullptr);
			a_ctx->OMGetRenderTargets(1, &rtvM, &dsvM);
			ID3D11Buffer* vbM = nullptr;
			UINT strideM = 0, voffM = 0;
			a_ctx->IAGetVertexBuffers(0, 1, &vbM, &strideM, &voffM);
			ID3D11Buffer* ibM = nullptr;
			DXGI_FORMAT fmtM = DXGI_FORMAT_UNKNOWN;
			UINT ioffM = 0;
			a_ctx->IAGetIndexBuffer(&ibM, &fmtM, &ioffM);

			Log("[META] #%d ps=%p vs=%p rtv=%p dsv=%p voff=%u ioff=%u stride=%u",
				s_metaCount, (void*)psM, (void*)vsM, (void*)rtvM, (void*)dsvM, voffM, ioffM, strideM);

			if (psM) psM->Release();
			if (vsM) vsM->Release();
			if (rtvM) rtvM->Release();
			if (dsvM) dsvM->Release();
			if (vbM) vbM->Release();
			if (ibM) ibM->Release();
		}

		// 一次性只读诊断：只捕获【颜色 pass】（PS != nullptr）的 144 候选。
		// 之前先命中 PS=NULL 的深度预 pass，导致顶点数据看起来反常。
		if (!s_fadeCaptured && a_count == 144 && g_ads.load()) {
			ID3D11PixelShader* psCheck = nullptr;
			a_ctx->PSGetShader(&psCheck, nullptr, nullptr);
			if (psCheck) {
				psCheck->Release();
				s_fadeCaptured = true;
				CaptureScopeFade(a_ctx, a_count, a_start, a_base);
			}
		}

		// 一次性只读诊断：捕获 384（Lense:0）的颜色 pass 顶点，用来判断它是
		// 「完整镜片面」还是「筒壁/环形」。需要先勾 FORCE Lense 让它被绘制。
		if (!s_lenseCaptured && a_count == 384 && g_ads.load()) {
			ID3D11PixelShader* psL = nullptr;
			a_ctx->PSGetShader(&psL, nullptr, nullptr);
			if (psL) {
				psL->Release();
				s_lenseCaptured = true;
				Log("[CAP] === LENSE (384) capture start ===");
				CaptureScopeFade(a_ctx, a_count, a_start, a_base);
			}
		}

		// 照 TTS 的 IsTargetDrawCall：用「索引数 + 顶点 stride」双重匹配。
		// 光比索引数会误伤（96 有两个几何体、384 和天空撞车），
		// stride 才是我们圆盘特有的指纹。TTS 用 28，我们的 LensCircle 实测是 24。
		if (s_diskIndexCount > 0 && a_count == s_diskIndexCount && g_ads.load()) {
			ID3D11Buffer* vb = nullptr;
			UINT stride = 0, off = 0;
			a_ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &off);
			if (vb) vb->Release();

			// r99 时间线实测：自挂 LensCircle.nif 的 stride 是 24。
			// 之前写 28 命中的是另一个 96-index 几何体（#8，depthFunc=2）。
			// 只在颜色 pass 接管：深度预 pass（PS 为空）不该调用 LensDraw，
			// 那会往无 PS 的 pass 里塞我们的着色器。
			ID3D11PixelShader* psChk2 = nullptr;
			a_ctx->PSGetShader(&psChk2, nullptr, nullptr);
			const bool hasPS = (psChk2 != nullptr);
			if (psChk2) psChk2->Release();

			if (stride == 24 && hasPS) {
				// 阶段 A 验证：记录每次识别的完整指纹，用于确认稳定性与不误命中
				static int s_hitLog = 0;
				static UINT s_lastVoff = 0xFFFFFFFF;
				if (s_hitLog < 40) {
					s_hitLog++;
					Log("[IDENT] #%d count=%u stride=%u voff=%u start=%u base=%d%s",
						s_hitLog, a_count, stride, off, a_start, a_base,
						(off == s_lastVoff ? "" : "  <-- voff CHANGED"));
					s_lastVoff = off;
				}
				LensDraw::Draw(a_ctx, a_count, a_start, a_base);
				return;
			}
		}
		s_origDrawIndexed(a_ctx, a_count, a_start, a_base);
	}

	// ======================= Input Layout 取证 =======================
	// IAGetInputLayout 只给指针，拿不到元素描述。这里 hook CreateInputLayout，
	// 把每个 InputLayout 指针和它的 element desc 关联起来，供捕获时反查。
	struct IlRecord
	{
		void* ptr = nullptr;
		UINT count = 0;
		D3D11_INPUT_ELEMENT_DESC descs[16]{};
	};
	static IlRecord s_ilRecords[64];
	static int s_ilRecordCount = 0;

	using CreateInputLayout_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
	static CreateInputLayout_t s_origCreateInputLayout = nullptr;

	static HRESULT STDMETHODCALLTYPE hkCreateInputLayout(ID3D11Device* a_dev, const D3D11_INPUT_ELEMENT_DESC* a_desc, UINT a_num,
		const void* a_code, SIZE_T a_codeSize, ID3D11InputLayout** a_out)
	{
		const HRESULT hr = s_origCreateInputLayout(a_dev, a_desc, a_num, a_code, a_codeSize, a_out);
		if (SUCCEEDED(hr) && a_out && *a_out && s_ilRecordCount < 64) {
			auto& rec = s_ilRecords[s_ilRecordCount++];
			rec.ptr = *a_out;
			rec.count = (a_num < 16) ? a_num : 16;
			for (UINT i = 0; i < rec.count; ++i) rec.descs[i] = a_desc[i];
			Log("[IL] created ptr=%p elements=%u", rec.ptr, rec.count);
			for (UINT i = 0; i < rec.count; ++i) {
				Log("[IL]   %s%d fmt=%d slot=%u off=%u perInst=%d",
					rec.descs[i].SemanticName ? rec.descs[i].SemanticName : "?",
					rec.descs[i].SemanticIndex, (int)rec.descs[i].Format,
					rec.descs[i].InputSlot, rec.descs[i].AlignedByteOffset,
					rec.descs[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA ? 1 : 0);
			}
		}
		return hr;
	}

	static void DumpIlFor(void* a_ptr)
	{
		for (int i = 0; i < s_ilRecordCount; ++i) {
			if (s_ilRecords[i].ptr != a_ptr) continue;
			auto& rec = s_ilRecords[i];
			Log("[CAP] IL desc for %p (%u elements):", a_ptr, rec.count);
			for (UINT k = 0; k < rec.count; ++k) {
				Log("[CAP]   %s%d fmt=%d slot=%u off=%u",
					rec.descs[k].SemanticName ? rec.descs[k].SemanticName : "?",
					rec.descs[k].SemanticIndex, (int)rec.descs[k].Format,
					rec.descs[k].InputSlot, rec.descs[k].AlignedByteOffset);
			}
			return;
		}
		Log("[CAP] IL desc for %p: NOT FOUND in records (created before hook)", a_ptr);
	}

	// ======================= r163: P1/P2 两条 384 draw 的身份与几何 =======================
	// 背景：r155/r162 的日志显示每帧有两条 384 draw（voff 7670272 / 12181248），
	// 但 CaptureScopeFade 只被调用一次，所以从来没有同时拿到两条的完整数据。
	// 这里按 (VB 指针, voff) 做身份去重，每个身份只取证一次，最多 4 个。
	// 只读：不动任何渲染状态，所有 COM 引用都释放。
	// （struct / 静态槽位 / 原型已在文件前部声明）
	static float HalfToFloat(uint16_t h)
	{
		const uint32_t sign = (h & 0x8000u) ? 0x80000000u : 0u;
		uint32_t exp = (h >> 10) & 0x1Fu;
		uint32_t man = h & 0x3FFu;
		uint32_t f = 0;
		if (exp == 0) {
			if (man == 0) {
				f = sign;
			} else {
				exp = 1;
				while ((man & 0x400u) == 0) { man <<= 1; --exp; }
				man &= 0x3FFu;
				f = sign | ((exp + 127u - 15u) << 23) | (man << 13);
			}
		} else if (exp == 31) {
			f = sign | 0x7F800000u | (man << 13);
		} else {
			f = sign | ((exp + 127u - 15u) << 23) | (man << 13);
		}
		float out;
		memcpy(&out, &f, 4);
		return out;
	}

	static int FindOrAddLenseSlot(ID3D11Buffer* a_vb, UINT a_voff)
	{
		for (int i = 0; i < 4; ++i) {
			if (s_lenseSlots[i].used && s_lenseSlots[i].vb == a_vb && s_lenseSlots[i].voff == a_voff) return i;
		}
		for (int i = 0; i < 4; ++i) {
			if (!s_lenseSlots[i].used) {
				s_lenseSlots[i].used = true;
				s_lenseSlots[i].vb = a_vb;
				s_lenseSlots[i].voff = a_voff;
				return i;
			}
		}
		return -1;
	}

	// r165: 并排取证 —— 同一 slot 的 depth pass 与 color pass 的完整渲染状态
	// 目的：判断"目镜内全黑"是否由深度预 pass 写深度导致
	static void ExaminePassState(ID3D11DeviceContext* a_ctx, int a_slot, bool a_isColor, UINT a_voff)
	{
		const int pi = a_isColor ? 1 : 0;
		if (a_slot < 0 || a_slot >= 4) return;
		if (s_passStateLogged[a_slot][pi]) return;
		s_passStateLogged[a_slot][pi] = true;

		// 深度/模板状态
		ID3D11DepthStencilState* ds = nullptr;
		UINT ref = 0;
		a_ctx->OMGetDepthStencilState(&ds, &ref);
		int dEn = -1, dWm = -1, dFunc = -1;
		int sEn = -1, sRm = -1, sWm = -1, sFunc = -1, sPass = -1, sFail = -1, sDfail = -1;
		if (ds) {
			D3D11_DEPTH_STENCIL_DESC dd{};
			ds->GetDesc(&dd);
			dEn = dd.DepthEnable ? 1 : 0;
			dWm = (int)dd.DepthWriteMask;
			dFunc = (int)dd.DepthFunc;
			sEn = dd.StencilEnable ? 1 : 0;
			sRm = dd.StencilReadMask;
			sWm = dd.StencilWriteMask;
			sFunc = (int)dd.FrontFace.StencilFunc;
			sPass = (int)dd.FrontFace.StencilPassOp;
			sFail = (int)dd.FrontFace.StencilFailOp;
			sDfail = (int)dd.FrontFace.StencilDepthFailOp;
		}

		// DSV 格式
		ID3D11DepthStencilView* dsv = nullptr;
		a_ctx->OMGetRenderTargets(0, nullptr, &dsv);
		int dsvViewFmt = -1, dsvResFmt = -1, dsvW = 0, dsvH = 0;
		if (dsv) {
			D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
			dsv->GetDesc(&dvd);
			dsvViewFmt = (int)dvd.Format;
			ID3D11Resource* res = nullptr;
			dsv->GetResource(&res);
			if (res) {
				D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
				res->GetType(&dim);
				if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
					D3D11_TEXTURE2D_DESC td{};
					((ID3D11Texture2D*)res)->GetDesc(&td);
					dsvResFmt = (int)td.Format;
					dsvW = (int)td.Width;
					dsvH = (int)td.Height;
				}
				res->Release();
			}
			dsv->Release();
		}

		// RTV 数量与各自格式尺寸
		ID3D11RenderTargetView* rtvs[8] = {};
		ID3D11DepthStencilView* dsv2 = nullptr;
		a_ctx->OMGetRenderTargets(8, rtvs, &dsv2);
		if (dsv2) dsv2->Release();
		int rtvCount = 0;
		for (int i = 0; i < 8; ++i) if (rtvs[i]) ++rtvCount;

		// Viewport
		D3D11_VIEWPORT vp{};
		UINT vpCount = 1;
		a_ctx->RSGetViewports(&vpCount, &vp);

		Log("[PASS] slot=%d pass=%s voff=%u | Depth(en=%d wm=%d func=%d) | Stencil(en=%d rm=0x%X wm=0x%X func=%d pass=%d fail=%d dfail=%d ref=%u)",
			a_slot, a_isColor ? "COLOR" : "DEPTH", a_voff,
			dEn, dWm, dFunc, sEn, sRm, sWm, sFunc, sPass, sFail, sDfail, ref);
		Log("[PASS] slot=%d pass=%s | DSV(view=%d res=%d %dx%d) | RTVn=%d | VP=(%.0f,%.0f %.0fx%.0f)",
			a_slot, a_isColor ? "COLOR" : "DEPTH",
			dsvViewFmt, dsvResFmt, dsvW, dsvH, rtvCount,
			vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);

		for (int i = 0; i < 8; ++i) {
			if (!rtvs[i]) continue;
			ID3D11Resource* res = nullptr;
			rtvs[i]->GetResource(&res);
			if (res) {
				D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
				res->GetType(&dim);
				if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
					D3D11_TEXTURE2D_DESC td{};
					((ID3D11Texture2D*)res)->GetDesc(&td);
					Log("[PASS] slot=%d pass=%s | RTV[%d] %dx%d fmt=%d",
						a_slot, a_isColor ? "COLOR" : "DEPTH", i, td.Width, td.Height, (int)td.Format);
				}
				res->Release();
			}
			rtvs[i]->Release();
		}

		if (ds) ds->Release();
	}

	// 一次只读取证：身份 + 索引 + 顶点（POSITION0 = R16G16B16A16_FLOAT @ offset 0）
	static void ExamineLenseDraw(ID3D11DeviceContext* a_ctx, UINT a_count, UINT a_start, INT a_base, int a_slot)
	{
		ID3D11Device* dev = nullptr;
		a_ctx->GetDevice(&dev);
		if (!dev) { Log("[P1] GetDevice FAILED"); return; }

		ID3D11Buffer* vb = nullptr;
		UINT stride = 0, voff = 0;
		a_ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
		ID3D11Buffer* ib = nullptr;
		DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
		UINT ioff = 0;
		a_ctx->IAGetIndexBuffer(&ib, &ibFmt, &ioff);

		D3D11_BUFFER_DESC vbd{}, ibd{};
		if (vb) vb->GetDesc(&vbd);
		if (ib) ib->GetDesc(&ibd);

		ID3D11VertexShader* vs = nullptr;
		ID3D11PixelShader* ps = nullptr;
		ID3D11InputLayout* il = nullptr;
		a_ctx->VSGetShader(&vs, nullptr, nullptr);
		a_ctx->PSGetShader(&ps, nullptr, nullptr);
		a_ctx->IAGetInputLayout(&il);

		ID3D11RenderTargetView* rtvs[8] = {};
		ID3D11DepthStencilView* dsv = nullptr;
		a_ctx->OMGetRenderTargets(8, rtvs, &dsv);
		int rtvCount = 0;
		for (int i = 0; i < 8; ++i) if (rtvs[i]) ++rtvCount;

		ID3D11RasterizerState* rs = nullptr;
		a_ctx->RSGetState(&rs);
		int cull = -1;
		if (rs) { D3D11_RASTERIZER_DESC rd{}; rs->GetDesc(&rd); cull = (int)rd.CullMode; }

		Log("[P1] slot=%d count=%u start=%u base=%d | stride=%u voff=%u | ioff=%u ifmt=%d",
			a_slot, a_count, a_start, a_base, stride, voff, ioff, (int)ibFmt);
		Log("[P1] slot=%d VB=%p bytes=%u | IB=%p bytes=%u",
			a_slot, (void*)vb, vbd.ByteWidth, (void*)ib, ibd.ByteWidth);
		Log("[P1] slot=%d VS=%p PS=%p IL=%p | RTVcount=%d DSV=%p cull=%d",
			a_slot, (void*)vs, (void*)ps, (void*)il, rtvCount, (void*)dsv, cull);

		if (vs) vs->Release();
		if (ps) ps->Release();
		if (il) il->Release();
		if (rs) rs->Release();
		for (int i = 0; i < 8; ++i) if (rtvs[i]) rtvs[i]->Release();
		if (dsv) dsv->Release();

		// ---- P2a: 索引 ----
		const UINT idxBytes = (ibFmt == DXGI_FORMAT_R32_UINT) ? 4u : 2u;
		s_slotIdx[a_slot].clear();
		if (ib && a_count > 0) {
			const UINT readOff = ioff + a_start * idxBytes;
			const UINT readBytes = a_count * idxBytes;
			if (readOff + readBytes <= ibd.ByteWidth) {
				D3D11_BUFFER_DESC sd{};
				sd.ByteWidth = readBytes;
				sd.Usage = D3D11_USAGE_STAGING;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				ID3D11Buffer* stg = nullptr;
				if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &stg)) && stg) {
					D3D11_BOX box{};
					box.left = readOff; box.right = readOff + readBytes;
					box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
					a_ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, ib, 0, &box);
					D3D11_MAPPED_SUBRESOURCE ms{};
					if (SUCCEEDED(a_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &ms))) {
						const auto* raw = (const uint8_t*)ms.pData;
						UINT mn = 0xFFFFFFFFu, mx = 0;
						std::vector<bool> seen;
						UINT uniq = 0;
						for (UINT i = 0; i < a_count; ++i) {
							uint32_t v = (idxBytes == 4)
								? *((const uint32_t*)(raw + i * 4))
								: (uint32_t)(*((const uint16_t*)(raw + i * 2)));
							s_slotIdx[a_slot].push_back(v);
							if (v < mn) mn = v;
							if (v > mx) mx = v;
							if (v >= seen.size()) seen.resize(v + 1, false);
							if (!seen[v]) { seen[v] = true; ++uniq; }
						}
						Log("[P2] slot=%d IDX count=%u min=%u max=%u unique=%u", a_slot, a_count, mn, mx, uniq);
						// 索引前 36 个（12 个三角形）：直接看拓扑是否是「环」还是「盘」
						char line[700]; int w = 0;
						const UINT showN = (a_count < 36) ? a_count : 36;
						for (UINT i = 0; i < showN; ++i) {
							w += sprintf_s(line + w, sizeof(line) - w, "%u ", s_slotIdx[a_slot][i]);
						}
						Log("[P2] slot=%d IDX[0..%u] %s", a_slot, showN, line);
						a_ctx->Unmap(stg, 0);
					}
					stg->Release();
				}
			}
		}

		// ---- P2b: 顶点（只有 stride=20 才按 POSITION0 = 4×half 解码）----
		s_slotPos[a_slot].clear();
		if (vb && stride >= 8) {
			const UINT want = stride * 240;
			const UINT readBytes = (vbd.ByteWidth < want) ? vbd.ByteWidth : want;
			if (readBytes > 0 && voff + readBytes <= vbd.ByteWidth) {
				D3D11_BUFFER_DESC sd{};
				sd.ByteWidth = readBytes;
				sd.Usage = D3D11_USAGE_STAGING;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				ID3D11Buffer* stg = nullptr;
				if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &stg)) && stg) {
					D3D11_BOX box{};
					box.left = voff; box.right = voff + readBytes;
					box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
					a_ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, vb, 0, &box);
					D3D11_MAPPED_SUBRESOURCE ms{};
					if (SUCCEEDED(a_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &ms))) {
						const auto* raw = (const uint8_t*)ms.pData;
						const UINT nVerts = readBytes / stride;
						float lo[3] = { 1e30f, 1e30f, 1e30f };
						float hi[3] = { -1e30f, -1e30f, -1e30f };
						for (UINT v = 0; v < nVerts; ++v) {
							const uint16_t* hp = (const uint16_t*)(raw + v * stride);
							float p[3] = { HalfToFloat(hp[0]), HalfToFloat(hp[1]), HalfToFloat(hp[2]) };
							for (int k = 0; k < 3; ++k) {
								if (p[k] < lo[k]) lo[k] = p[k];
								if (p[k] > hi[k]) hi[k] = p[k];
								s_slotPos[a_slot].push_back(p[k]);
							}
						}
						Log("[P2] slot=%d VERT n=%u stride=%u bbox x[%.2f..%.2f] y[%.2f..%.2f] z[%.2f..%.2f]",
							a_slot, nVerts, stride,
							lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);

						// 按「最分散的轴」分层：回答「是不是多层同心环」
						int axis = 0;
						for (int k = 1; k < 3; ++k) if ((hi[k] - lo[k]) > (hi[axis] - lo[axis])) axis = k;
						const char axName[3] = { 'X', 'Y', 'Z' };
						float layerLo = 1e30f, layerHi = -1e30f;
						UINT layerN = 0, layerNo = 0;
						float rMin = 1e30f, rMax = -1e30f;
						auto flushLayer = [&]() {
							if (layerN == 0) return;
							Log("[P2] slot=%d  layer#%u %c in [%.2f..%.2f] n=%u  radial r[%.2f..%.2f]",
								a_slot, layerNo, axName[axis], layerLo, layerHi, layerN, rMin, rMax);
							++layerNo;
							layerN = 0; rMin = 1e30f; rMax = -1e30f;
						};
						for (UINT v = 0; v < nVerts; ++v) {
							const float* p = &s_slotPos[a_slot][v * 3];
							const float a = p[axis];
							if (layerN > 0 && fabsf(a - layerLo) <= 1.0f) {
								// 同一层
							} else if (layerN > 0 && fabsf(a - layerHi) <= 1.0f) {
								// 同一层
							} else {
								flushLayer();
								layerLo = a; layerHi = a;
							}
							if (a < layerLo) layerLo = a;
							if (a > layerHi) layerHi = a;
							++layerN;
							float r = 0.0f;
							for (int k = 0; k < 3; ++k) if (k != axis) r += p[k] * p[k];
							r = sqrtf(r);
							if (r < rMin) rMin = r;
							if (r > rMax) rMax = r;
						}
						flushLayer();
						a_ctx->Unmap(stg, 0);
					}
					stg->Release();
				}
			}
		}

		// ---- P2c: 与前面已捕获的 slot 对比 ----
		for (int s = 0; s < a_slot; ++s) {
			if (!s_lenseSlots[s].captured) continue;
			const bool sameIdx = (!s_slotIdx[s].empty() && s_slotIdx[s] == s_slotIdx[a_slot]);
			bool samePos = (!s_slotPos[s].empty() && s_slotPos[s] == s_slotPos[a_slot]);
			Log("[P2] slot=%d vs slot=%d: idxSame=%d(%zu/%zu) posSame=%d(%zu/%zu)",
				a_slot, s, sameIdx ? 1 : 0, s_slotIdx[s].size(), s_slotIdx[a_slot].size(),
				samePos ? 1 : 0, s_slotPos[s].size() / 3, s_slotPos[a_slot].size() / 3);
		}
		Log("[P1] slot=%d === capture done ===", a_slot);
	}

	// ======================= ScopeNormal 子树递归枚举 =======================
	// 递归列出整个 ScopeNormal 下的所有节点/几何，确认 [NORMAL] 是否漏了 shape
	static void DumpSubtreeRec(RE::NiAVObject* a_obj, int a_depth, int& a_shapes)
	{
		if (!a_obj || a_depth > 6) return;
		const char* type = "?";
		auto* tri = a_obj->IsTriShape();
		if (tri) {
			type = "TriShape";
			a_shapes++;
			Log("[SUB] %*s%s tri=%u vert=%u cull=%d", a_depth * 2, "", a_obj->name.c_str(),
				tri->numTriangles, (unsigned)tri->numVertices, a_obj->GetAppCulled() ? 1 : 0);
		} else if (a_obj->IsNode()) {
			type = "Node";
			auto* node = a_obj->IsNode();
			Log("[SUB] %*s%s (%s) children=%zu cull=%d", a_depth * 2, "", a_obj->name.c_str(),
				type, node->children.size(), a_obj->GetAppCulled() ? 1 : 0);
			for (auto& ch : node->children) {
				if (ch) DumpSubtreeRec(ch.get(), a_depth + 1, a_shapes);
			}
			return;
		} else {
			Log("[SUB] %*s%s (other) cull=%d", a_depth * 2, "", a_obj->name.c_str(), a_obj->GetAppCulled() ? 1 : 0);
			return;
		}
	}

	// ======================= 场景树侧取证（ScopeFade） =======================
	// 记录 ScopeFade 这个 BSTriShape 的实际字段，供后续与渲染层 Draw 建立映射。
	// 只读；指针仅用于本次运行诊断，不写死进任何识别逻辑。
	static void DumpScopeFadeObject(RE::PlayerCharacter* a_pc)
	{
		if (!a_pc) return;
		auto* fade = FindNode(a_pc, "ScopeFade");
		if (!fade) { Log("[OBJ] ScopeFade NOT FOUND"); return; }

		auto* tri = fade->IsTriShape();
		if (!tri) { Log("[OBJ] ScopeFade is not a TriShape"); return; }

		Log("[OBJ] ScopeFade ptr=%p name=%s", (void*)tri, tri->name.c_str());
		Log("[OBJ] tri=%u vert=%u", tri->numTriangles, (unsigned)tri->numVertices);

		const auto& w = tri->world;
		Log("[OBJ] world: pos=(%.3f,%.3f,%.3f) scale=%.3f", w.translate.x, w.translate.y, w.translate.z, w.scale);
		Log("[OBJ] worldBound: center=(%.3f,%.3f,%.3f) r=%.3f",
			tri->worldBound.center.x, tri->worldBound.center.y, tri->worldBound.center.z, tri->worldBound.fRadius);
		Log("[OBJ] local: pos=(%.3f,%.3f,%.3f) scale=%.3f",
			tri->local.translate.x, tri->local.translate.y, tri->local.translate.z, tri->local.scale);

		auto* p = tri->parent;
		int d = 0;
		while (p && d < 8) {
			Log("[OBJ] parent[%d] %s (%p)", d, p->name.c_str(), (void*)p);
			p = p->parent;
			++d;
		}
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

				// 停用 ForceLenseVisible：这一坨黑色就是 Lense:0，本该被 STS 剔除。
				// 强制显示它会让镜筒内部被黑色筒壁挡住，看不到后面。
				// Lense:0 是一组节点（Lense + polySurface22 + Mount1 + Mount2 同级）。
				// uncull 父链会把整组都带出来，所以这里做「互斥显示」：
				// 勾哪个就只显示哪个，其余三个强制隐藏。
				auto ShowOnly = [a_pc](const char* a_target) {
					auto* n = FindNode(a_pc, a_target);
					if (!n) return;
					if (n->GetAppCulled()) n->SetAppCulled(false);
					// 目标自己的祖先链要 uncull
					auto* p = n->parent;
					int guard = 0;
					while (p && guard++ < 10) {
						if (p->GetAppCulled()) p->SetAppCulled(false);
						p = p->parent;
					}
					// 其余三个强制隐藏
					const char* others[] = { "Lense", "polySurface22", "Mount1", "Mount2" };
					for (auto* o : others) {
						if (strcmp(o, a_target) == 0) continue;
						if (auto* on = FindNode(a_pc, o)) {
							if (!on->GetAppCulled()) on->SetAppCulled(true);
						}
					}
				};
				if (g_forceLense)  ShowOnly("Lense");
				else if (g_forcePoly)   ShowOnly("polySurface22");
				else if (g_forceMount1) ShowOnly("Mount1");
				else if (g_forceMount2) ShowOnly("Mount2");
				// 注意：不做「取消勾选后恢复」。SetAppCulled 是节点状态，我们无权改动，
				// 强行 cull 会污染 STS 自己的剔除逻辑（导致腰射没有瞄具）。
				// 取消勾选时什么都不做，让 STS 按自己的 ADS 逻辑管理可见性。

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

				// 每次 ADS 进入时列出 ScopeNormal 支的全部子节点（物镜在这里）
				if (!s_lastADS) {
					// P0：递归枚举整个 ScopeNormal 子树（确认是否漏了 shape）
					if (auto* sn2 = FindNode(a_pc, "ScopeNormal")) {
						static int s_subLog = 0;
						if (s_subLog < 2) {
							s_subLog++;
							int shapes = 0;
							Log("[SUB] === ScopeNormal subtree ===");
							DumpSubtreeRec(sn2, 0, shapes);
							Log("[SUB] === total drawable shapes = %d ===", shapes);
						}
					}
					if (auto* normalNode = FindNode(a_pc, "ScopeNormal")) {
						if (auto* nn = normalNode->IsNode()) {
							Log("[NORMAL] ScopeNormal children (%u):", (unsigned)nn->children.size());
							int ci = 0;
							for (auto& child : nn->children) {
								if (!child) { ++ci; continue; }
								auto* tri = child->IsTriShape();
								Log("[NORMAL]   [%d] %s tri=%u vert=%u cull=%d world=(%.2f,%.2f,%.2f)",
									ci, child->name.c_str(),
									tri ? tri->numTriangles : 0u,
									tri ? (unsigned)tri->numVertices : 0u,
									child->GetAppCulled() ? 1 : 0,
									child->world.translate.x, child->world.translate.y, child->world.translate.z);
								++ci;
							}
						}
					} else {
						Log("[NORMAL] ScopeNormal NOT FOUND");
					}
				}

				if (!s_lastADS) {
					// 进入开镜：读遮罩索引数 + 挂上覆盖"洞"的圆盘透镜
					s_lensIndexCount = 0;
					CacheLenseIndexCount(a_pc);
					SpawnLens();
					Log("[STAGE-A] ADS enter: diskIndexCount=%u lensIndexCount=%u", s_diskIndexCount, s_lensIndexCount);

					// 场景树侧：记录 ScopeFade 的实际字段（供 Draw 映射用）
					DumpScopeFadeObject(a_pc);

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

	// ======================= 自动截图（BMP） =======================
	// 在 Present 前把 backbuffer 存成 24 位 BMP，免去人工截图。
	static int s_shotIndex = 0;

	static void SaveBackbufferBmp(IDXGISwapChain* a_sc, ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, const char* a_tag)
	{
		if (!a_sc || !a_dev || !a_ctx) return;
		ID3D11Texture2D* back = nullptr;
		if (FAILED(a_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) {
			Log("[SHOT] GetBuffer FAILED");
			return;
		}
		D3D11_TEXTURE2D_DESC td{};
		back->GetDesc(&td);

		D3D11_TEXTURE2D_DESC sd = td;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		ID3D11Texture2D* staging = nullptr;
		if (FAILED(a_dev->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
			Log("[SHOT] staging FAILED");
			back->Release();
			return;
		}
		a_ctx->CopyResource(staging, back);

		D3D11_MAPPED_SUBRESOURCE ms{};
		if (FAILED(a_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms))) {
			Log("[SHOT] Map FAILED");
			staging->Release();
			back->Release();
			return;
		}

		const UINT W = td.Width, H = td.Height;
		const UINT rowBytes = W * 3;
		const UINT pad = (4 - (rowBytes % 4)) % 4;
		const UINT fileRow = rowBytes + pad;
		const UINT dataSize = fileRow * H;
		const UINT fileSize = 54 + dataSize;

		wchar_t path[MAX_PATH];
		swprintf_s(path, L"E:\\Custom Fallout4\\shots\\%s_%d.bmp",
			a_tag, s_shotIndex++);

		HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f != INVALID_HANDLE_VALUE) {
			uint8_t hdr[54] = {};
			hdr[0] = 'B'; hdr[1] = 'M';
			memcpy(hdr + 2, &fileSize, 4);
			uint32_t off = 54; memcpy(hdr + 10, &off, 4);
			uint32_t hs = 40; memcpy(hdr + 14, &hs, 4);
			memcpy(hdr + 18, &W, 4);
			memcpy(hdr + 22, &H, 4);
			uint16_t planes = 1; memcpy(hdr + 26, &planes, 2);
			uint16_t bpp = 24; memcpy(hdr + 28, &bpp, 2);
			memcpy(hdr + 34, &dataSize, 4);
			DWORD wrote = 0;
			WriteFile(f, hdr, 54, &wrote, nullptr);

			// 逐行写（BMP 是自底向上，且 BGR）
			std::vector<uint8_t> row(fileRow);
			for (int y = (int)H - 1; y >= 0; --y) {
				const uint8_t* src = (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch;
				for (UINT x = 0; x < W; ++x) {
					row[x * 3 + 0] = src[x * 4 + 0];  // B
					row[x * 3 + 1] = src[x * 4 + 1];  // G
					row[x * 3 + 2] = src[x * 4 + 2];  // R
				}
				WriteFile(f, row.data(), fileRow, &wrote, nullptr);
			}
			CloseHandle(f);
			Log("[SHOT] saved %ls", path);
		} else {
			Log("[SHOT] CreateFile FAILED (%ls)", path);
		}

		a_ctx->Unmap(staging, 0);
		staging->Release();
		back->Release();
	}

	// ======================= Present =======================
	using Present_t = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
	static Present_t s_origPresent = nullptr;

	static HRESULT WINAPI hkPresent(IDXGISwapChain* a_sc, UINT a_sync, UINT a_flags)
	{
		// F7 开关调参菜单；F8 武装一次抑制实验
		static bool s_lastF7 = false;
		const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
		if (f7 && !s_lastF7) ImGui_Toggle();
		s_lastF7 = f7;

		static bool s_lastF8 = false;
		const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
		if (f8 && !s_lastF8) {
			s_suppressFrames.store(300);
			s_suppressSkipped.store(0);
			s_suppressWhich.store(0);
			s_suppressActive.store(true);
			Log("[SUPPRESS] armed: 144-index color draw (ScopeFade, 5s)");
		}
		s_lastF8 = f8;

		// F11：抑制低 voff 的 384（候选 A）；F12：抑制高 voff 的 384（候选 B）
		static bool s_lastF11 = false;
		const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
		if (f11 && !s_lastF11) {
			s_suppressFrames.store(300);
			s_suppressSkipped.store(0);
			s_suppressWhich.store(1);
			s_suppressActive.store(true);
			Log("[SUPPRESS] armed: 384 candidate A (low voff, 5s)");
		}
		s_lastF11 = f11;

		static bool s_lastF12 = false;
		const bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
		if (f12 && !s_lastF12) {
			s_suppressFrames.store(300);
			s_suppressSkipped.store(0);
			s_suppressWhich.store(2);
			s_suppressActive.store(true);
			Log("[SUPPRESS] armed: 384 candidate B (high voff, 5s)");
		}
		s_lastF12 = f12;

		// F10：启动 Gate A 时间线诊断窗口（避开游戏的 F5/F9 快存快读）（60 帧）
		static bool s_lastF9 = false;
		const bool f9 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
		if (f9 && !s_lastF9) {
			s_timelineFrames.store(60);
			s_timelineActive.store(true);
			s_timelineSeq = 0;
			Log("[TL] window armed: 60 frames, will log 96/144/384 draws in order");
		}
		s_lastF9 = f9;
		if (s_timelineActive.load()) {
			if (s_timelineFrames.fetch_sub(1) <= 0) {
				s_timelineActive.store(false);
				Log("[TL] window closed, total draws logged=%d", s_timelineSeq);
			}
		}

		ImGui_Render(a_sc);

		// 每帧摘要
		s_candFrame++;
		s_candLog = 0;
		if (s_frameStats && s_frameDraws > 0) {
			static int s_frameNo = 0;
			if (s_frameNo < 60) {
				Log("[FRAME] #%d draws=%d stencilOn=%d", s_frameNo++, s_frameDraws, s_frameStencilOn);
			}
			s_frameDraws = 0;
			s_frameStencilOn = 0;
		}

		// 截图：F6 手动；抑制窗口内每 60 帧自动一张
		{
			auto* rdShot = RE::BSGraphics::RendererData::GetSingleton();
			if (rdShot && rdShot->device && rdShot->context) {
				auto* devShot = (ID3D11Device*)static_cast<void*>(rdShot->device);
				auto* ctxShot = (ID3D11DeviceContext*)static_cast<void*>(rdShot->context);

				static bool s_lastF6 = false;
				const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
				if (g_wantShot) { g_wantShot = false; SaveBackbufferBmp(a_sc, devShot, ctxShot, "manual"); }
				if (f6 && !s_lastF6) {
					SaveBackbufferBmp(a_sc, devShot, ctxShot, "manual");
				}
				s_lastF6 = f6;

				static int s_autoTick = 0;
				if (s_suppressActive.load()) {
					if ((s_autoTick++ % 60) == 0) {
						SaveBackbufferBmp(a_sc, devShot, ctxShot, "suppress");
					}
				} else {
					s_autoTick = 0;
				}
			}
		}

		// 回到屏幕层方案（r37 状态）：准星固定在屏幕中心，圆形遮罩可调。
		// 3D 接管那套因为"索引数无法唯一标识几何体"而误伤天空，已停用。
		if (g_screenOverlay) OverlayDraw(a_sc);
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
		CreateDirectoryW(L"E:\\Custom Fallout4\\shots", nullptr);

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

		// Input Layout 取证：hook ID3D11Device::CreateInputLayout (vtable[11])
		{
			auto* rdDev = RE::BSGraphics::RendererData::GetSingleton();
			if (rdDev && rdDev->device) {
				auto* dev = (ID3D11Device*)static_cast<void*>(rdDev->device);
				void** vtDev = *(void***)dev;
				if (MH_CreateHook(vtDev[11], (LPVOID)&hkCreateInputLayout, (LPVOID*)&s_origCreateInputLayout) == MH_OK) {
					MH_EnableHook(vtDev[11]);
					Log("[INIT] CreateInputLayout hooked");
					InitDiagShaders(dev);
				} else {
					Log("[INIT] CreateInputLayout hook FAILED");
				}
			}
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
