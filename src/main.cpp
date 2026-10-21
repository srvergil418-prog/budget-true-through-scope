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
		auto* fadeNode = FindNode(pc, "ScopeFade");
		// r190: 没有 ScopeFade 的武器（非 STS 瞄具）不创建任何东西，
		// 否则会在这个武器上挂出一个悬空的白圆盘。
		if (!fadeNode) return;
		{
			const RE::NiPoint3 c = fadeNode->worldBound.center;
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
	static ID3D11DepthStencilState* s_dsNoDepthWrite = nullptr;   // r166: 不写深度
	static ID3D11DepthStencilState* s_dsNoDepthTest = nullptr;   // r168: 完全不测深度
	// r169: 目镜 mask 资源
	// r191: 瞄具白名单配置（必须在使用点之前声明）
	// K98k = index-subset（一个 shape 两片，只画 B 组）；ATACR = whole-shape（rear_glass 整片）
	struct ScopeProfile
	{
		const char* name = "unknown";
		UINT targetIndexCount = 0;
		UINT maskStart = 0;
		UINT maskCount = 0;
		UINT expectedStride = 0;   // r199: 该瞄具的顶点步长（诊断/校验用，0=未知）
		UINT maxVertexIdx = 0;     // r227: 目标几何的最大顶点索引上限（用于排除同 count 的其他几何）
	};
	static ScopeProfile s_scopeProfile;

	// ======================= r229: Phase 1 最小适配器 =======================
	// 列出当前 ScopeNormal 子树里所有可绘制 shape，让用户在 GUI 里挑一个当目镜。
	// 选中后只设置临时 profile（不写盘），配合 mask debug view 验证。
	struct ShapeEntry
	{
		char   name[64] = {};
		UINT   tri = 0;
		UINT   vert = 0;
		UINT   count = 0;    // tri * 3
		UINT   maxIdx = 0;   // vert - 1
		char   source = '?'; // 'A'=ScopeAiming / 'N'=ScopeNormal
	};
	static constexpr int kMaxShapeEntries = 32;
	static ShapeEntry s_shapeList[kMaxShapeEntries];
	static int        s_shapeCount = 0;
	static bool       s_shapeListDirty = true;   // ADS 进入时置 true，重新扫描
	static int        s_userPickedShape = -1;    // 用户点选的索引（-1 = 用内置 profile）

	static ID3D11Texture2D* s_maskTex = nullptr;
	static ID3D11Texture2D* s_maskStaging = nullptr;   // r172: 读回用
	static std::atomic<int> s_maskWrites{ 0 };   // r173: mask 写入总次数
	// r178: mask 写入时记录的几何身份（VB 指针 + 顶点偏移），用于限定 SKIP DEPTH 的命中范围
	static ID3D11Buffer* s_maskVB = nullptr;
	static UINT s_maskVoff = 0;
	static std::atomic<int> s_dwSkipped{ 0 };   // r178: 本帧跳过深度写入的次数
	// r195: 颜色 pass 两层诊断统计
	static int s_cStatTotal = 0;
	static int s_cStatStride[65] = {};
	static int s_cStatSmall = 0;
	static int s_cStatLarge = 0;
	static int s_cStatStartNZ = 0;
	static int s_cStatDetail = 0;
	static int s_cStatFrames = 0;

	// r181: 严格 Lense 识别 —— 用索引缓冲区内容判定（读一次，缓存结果）
	// 依据（S 级，来自 NIF 字节级比对）：Lense:0 的 384 个索引只引用 0..81 顶点；
	// 另一条同名 384 Draw 的索引引用范围更大（实测最大 113）。
	static bool s_lenseIdentified = false;
	static ID3D11Buffer* s_lenseVB = nullptr;      // 持有独立 COM 引用
	static int s_idTries = 0;                     // r200: 识别尝试计数（profile 切换时重置）
	static UINT s_lenseVoff = 0;
	static UINT s_lenseStride = 0;
	static const void* s_lenseIbAddr = nullptr;
	static UINT s_lenseIOff = 0;
	static int  s_lenseIFmt = -1;
	static int  s_lenseMaxIdx = -1;
	static ID3D11Buffer* s_ibStaging = nullptr;    // 读回索引缓冲区用
	static UINT s_ibStagingSize = 0;
	static ID3D11RenderTargetView* s_maskRTV = nullptr;
	static ID3D11PixelShader* s_psMask = nullptr;
	ID3D11ShaderResourceView* g_maskSRV = nullptr;

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

		// r166: 用于替换 Lense 深度预 pass 的状态 —— 深度测试照常，但不写深度
		a_dev->CreateDepthStencilState(&dsC, &s_dsNoDepthWrite);

		// r168: 十字用的「完全不测深度」状态
		D3D11_DEPTH_STENCIL_DESC dsN{};
		dsN.DepthEnable = FALSE;
		dsN.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dsN.DepthFunc = D3D11_COMPARISON_ALWAYS;
		dsN.StencilEnable = FALSE;
		a_dev->CreateDepthStencilState(&dsN, &s_dsNoDepthTest);

		// r169: 目镜 mask —— R8 纹理，尺寸先用 1920x1080（与 Lense 的 viewport 一致）
		{
			D3D11_TEXTURE2D_DESC mtd{};
			mtd.Width = 1920;
			mtd.Height = 1080;
			mtd.MipLevels = 1;
			mtd.ArraySize = 1;
			mtd.Format = DXGI_FORMAT_R8_UNORM;
			mtd.SampleDesc.Count = 1;
			mtd.Usage = D3D11_USAGE_DEFAULT;
			mtd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			if (SUCCEEDED(a_dev->CreateTexture2D(&mtd, nullptr, &s_maskTex)) && s_maskTex) {
				a_dev->CreateRenderTargetView(s_maskTex, nullptr, &s_maskRTV);
				a_dev->CreateShaderResourceView(s_maskTex, nullptr, &g_maskSRV);
				// staging：CPU 读回 mask 中心值（诊断用）
				D3D11_TEXTURE2D_DESC std2 = mtd;
				std2.Usage = D3D11_USAGE_STAGING;
				std2.BindFlags = 0;
				std2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				a_dev->CreateTexture2D(&std2, nullptr, &s_maskStaging);
			}

			// mask PS：覆盖即写 1
			const char* kMaskPs = R"(
float4 main() : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }
)";
			ID3DBlob* mb = nullptr; ID3DBlob* me = nullptr;
			if (SUCCEEDED(D3DCompile(kMaskPs, strlen(kMaskPs), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &mb, &me)) && mb) {
				a_dev->CreatePixelShader(mb->GetBufferPointer(), mb->GetBufferSize(), nullptr, &s_psMask);
				mb->Release();
			}
			if (me) me->Release();
			Log("[MASK] tex=%d rtv=%d srv=%d ps=%d", s_maskTex ? 1 : 0, s_maskRTV ? 1 : 0, g_maskSRV ? 1 : 0, s_psMask ? 1 : 0);
		}

		Log("[DIAG] cross ps=%d cb=%d dsNoStencil=%d dsNoDepthWrite=%d",
			s_psCross ? 1 : 0, s_cbCross ? 1 : 0, s_dsNoStencil ? 1 : 0, s_dsNoDepthWrite ? 1 : 0);
	}

	// ======================= r181: 严格 Lense 识别 =======================
	// 读索引缓冲区的 [ioff, ioff + count*2) 范围，返回其中最大的顶点索引。
	// 用途：Lense:0 的 384 个索引只引用 0..81（82 顶点）；另一条 384 Draw 引用到 113。
	// 只在识别阶段调用一次，不进每帧热路径。
	static int ReadIndexBufferMax(ID3D11DeviceContext* a_ctx, ID3D11Device* a_dev,
		ID3D11Buffer* a_ib, UINT a_ioff, DXGI_FORMAT a_fmt, UINT a_count)
	{
		if (!a_ctx || !a_dev || !a_ib) return -1;
		if (a_fmt != DXGI_FORMAT_R16_UINT) return -1;

		const UINT need = a_count * 2u;
		D3D11_BUFFER_DESC bd{};
		a_ib->GetDesc(&bd);
		if (bd.ByteWidth < a_ioff + need) return -1;

		if (!s_ibStaging || s_ibStagingSize < need) {
			if (s_ibStaging) { s_ibStaging->Release(); s_ibStaging = nullptr; }
			D3D11_BUFFER_DESC sd{};
			sd.ByteWidth = need;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(a_dev->CreateBuffer(&sd, nullptr, &s_ibStaging))) { s_ibStaging = nullptr; return -1; }
			s_ibStagingSize = need;
		}

		D3D11_BOX box{};
		box.left = a_ioff;
		box.right = a_ioff + need;
		box.top = 0; box.bottom = 1;
		box.front = 0; box.back = 1;
		a_ctx->CopySubresourceRegion(s_ibStaging, 0, 0, 0, 0, a_ib, 0, &box);

		D3D11_MAPPED_SUBRESOURCE ms{};
		if (FAILED(a_ctx->Map(s_ibStaging, 0, D3D11_MAP_READ, 0, &ms))) return -1;
		const uint16_t* p = (const uint16_t*)ms.pData;
		int mx = -1;
		for (UINT i = 0; i < a_count; ++i) {
			if ((int)p[i] > mx) mx = (int)p[i];
		}
		a_ctx->Unmap(s_ibStaging, 0);
		return mx;
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

		// ======================= r195: 颜色 pass 两层诊断 =======================
		// 层 A：全量统计（不因任何过滤而丢失覆盖率）
		// 层 B：候选详单（100~1000 索引，或 start != 0），记录 stride 但不做硬过滤
		if (g_ads.load()) {
			ID3D11PixelShader* psS = nullptr;
			a_ctx->PSGetShader(&psS, nullptr, nullptr);
			const bool colorS = (psS != nullptr);
			if (psS) psS->Release();

			if (colorS) {
				ID3D11Buffer* vbS = nullptr;
				UINT stS = 0, ofS = 0;
				a_ctx->IAGetVertexBuffers(0, 1, &vbS, &stS, &ofS);

				// --- 层 A：统计 ---
				++s_cStatTotal;
				if (stS < 65) ++s_cStatStride[stS];
				if (a_count >= 100 && a_count <= 1000) ++s_cStatSmall;
				else if (a_count > 1000) ++s_cStatLarge;
				if (a_start != 0) ++s_cStatStartNZ;

				// --- r197: 精确追踪 282±65 的颜色 draw（不受 60 条额度限制）---
				if (a_count >= 220 && a_count <= 350) {
					static int s_c282Log = 0;
					if (s_c282Log < 80) {
						++s_c282Log;
						Log("[C282?] count=%u start=%u base=%d stride=%u voff=%u | profile=%s expect=%u",
							a_count, a_start, a_base, stS, ofS, s_scopeProfile.name, s_scopeProfile.targetIndexCount);
					}
				}

				// --- 层 B：候选详单 ---
				const bool isCand = (a_count >= 100 && a_count <= 1000) || (a_start != 0);
				if (isCand && s_cStatDetail < 60) {
					++s_cStatDetail;
					ID3D11Buffer* ibS = nullptr;
					DXGI_FORMAT fmtS = DXGI_FORMAT_UNKNOWN;
					UINT ioffS = 0;
					a_ctx->IAGetIndexBuffer(&ibS, &fmtS, &ioffS);

					D3D11_BUFFER_DESC vbd{}, ibd{};
					if (vbS) vbS->GetDesc(&vbd);
					if (ibS) ibS->GetDesc(&ibd);

					Log("[CDETAIL] #%d count=%u start=%u base=%d | stride=%u voff=%u | VB=%p bw=%u | IB=%p bw=%u fmt=%d ioff=%u | expect=%u",
						s_cStatDetail, a_count, a_start, a_base, stS, ofS,
						(void*)vbS, vbd.ByteWidth, (void*)ibS, ibd.ByteWidth, (int)fmtS, ioffS,
						s_scopeProfile.targetIndexCount);
					if (ibS) ibS->Release();
				}
				if (vbS) vbS->Release();
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

		// ======================= r169: mask 模式 =======================
		// 保留原始材质绘制，再追加一次 B 组绘制写进 R8 mask
		if (g_maskEnabled && g_ads.load() && s_scopeProfile.targetIndexCount > 0
			&& a_count == s_scopeProfile.targetIndexCount && a_start == 0 && s_maskRTV && s_psMask) {
			// r222: 目标几何会在多个 pass 里被绘制（主颜色 + 阴影）。
			// 日志实测：同一 VB/voff 先出现在 vp=1920x1080 rtv0=有效，
			// 之后又在 vp=3501x2691 rtv0=NULL（阴影 pass）出现。
			// 阴影 pass 里强行绑我们的 mask RTV → 坐标完全错位 → 草地里出现"遮罩"。
			// 判据：必须有颜色 RTV，且 viewport 覆盖主画面尺寸。
			bool isMainColorPass = false;
			{
				ID3D11RenderTargetView* curRTV[8] = {};
				ID3D11DepthStencilView* curDSV = nullptr;
				a_ctx->OMGetRenderTargets(8, curRTV, &curDSV);
				const bool hasColorRTV = (curRTV[0] != nullptr);
				UINT vpN2 = 1;
				D3D11_VIEWPORT vp2{};
				a_ctx->RSGetViewports(&vpN2, &vp2);
				isMainColorPass = hasColorRTV && vp2.Width >= 1280.0f && vp2.Height >= 720.0f
					&& vp2.Width <= 4096.0f && vp2.Height <= 4096.0f;

				// r225: RTV 身份锁定 —— 同一个几何还有别的带 RTV 的 pass（不只是阴影）。
				// 第一次见到"看起来像主 pass"的 RTV 就记住它，之后只认这一个。
				// 这样即使有第二个带 RTV 的 pass，也不会再往 mask 里画第二份。
				static ID3D11RenderTargetView* s_mainRTV = nullptr;
				if (isMainColorPass) {
					if (!s_mainRTV) {
						s_mainRTV = curRTV[0];
						s_mainRTV->AddRef();
						Log("[MASK] learned main RTV = %p (vp=%.0fx%.0f), will only accept this one",
							(void*)s_mainRTV, vp2.Width, vp2.Height);
					} else if (curRTV[0] != s_mainRTV) {
						static int s_rtvMismatchLog = 0;
						if (s_rtvMismatchLog < 10) {
							++s_rtvMismatchLog;
							Log("[MASK-SKIP] rtv mismatch: got %p want %p (vp=%.0fx%.0f)",
								(void*)curRTV[0], (void*)s_mainRTV, vp2.Width, vp2.Height);
						}
						isMainColorPass = false;
					}
				}

				for (int i = 0; i < 8; ++i) if (curRTV[i]) curRTV[i]->Release();
				if (curDSV) curDSV->Release();
				if (!isMainColorPass) {
					static int s_passSkipLog = 0;
					if (s_passSkipLog < 10) {
						++s_passSkipLog;
						Log("[MASK-SKIP] not main color pass: rtv=%p vp=(%.0f,%.0f %.0fx%.0f)",
							(void*)curRTV[0], vp2.TopLeftX, vp2.TopLeftY, vp2.Width, vp2.Height);
					}
				}
			}

			if (isMainColorPass) {
			ID3D11PixelShader* psM = nullptr;
			a_ctx->PSGetShader(&psM, nullptr, nullptr);
			const bool colorM = (psM != nullptr);
			if (psM) psM->Release();

			ID3D11Buffer* vbM = nullptr;
			UINT strideM = 0, voffM = 0;
			a_ctx->IAGetVertexBuffers(0, 1, &vbM, &strideM, &voffM);
			// r182: 注意 —— 这里【不】立即 Release。
			// IAGetVertexBuffers 返回的指针带一个引用，识别时要用它做身份比较，
			// 提前 Release 会让之后保存的指针变成悬空（r181 的 bug）。
			// 释放统一放在本分支末尾（含 early return 路径）。

			// r198: 不再限制 stride —— K98k 是 20，ATACR 是 24（实测）
			if (colorM && strideM > 0) {
				// r227: 索引内容验证 —— 杂草/贴花等几何的 IndexCount 也可能是 282/384，
				// 只靠 count 会让它们也写进 mask（debug view 已证实：路旁杂草堆、建筑贴花都被点亮）。
				// 判据：读这条 draw 的 IB，最大顶点索引必须 <= profile 记录的上限。
				if (s_scopeProfile.maxVertexIdx > 0) {
					ID3D11Buffer* ibV = nullptr;
					DXGI_FORMAT fmtV = DXGI_FORMAT_UNKNOWN;
					UINT ioffV = 0;
					a_ctx->IAGetIndexBuffer(&ibV, &fmtV, &ioffV);
					ID3D11Device* devV = nullptr;
					a_ctx->GetDevice(&devV);
					if (ibV && devV) {
						const int mxV = ReadIndexBufferMax(a_ctx, devV, ibV, ioffV, fmtV, a_count);
						if (mxV < 0 || (UINT)mxV > s_scopeProfile.maxVertexIdx) {
							static int s_idxRejectLog = 0;
							if (s_idxRejectLog < 20) {
								++s_idxRejectLog;
								Log("[MASK-IDX-REJECT] voff=%u count=%u maxIdx=%d > profile limit %u",
									voffM, a_count, mxV, s_scopeProfile.maxVertexIdx);
							}
							if (devV) devV->Release();
							if (ibV) ibV->Release();
							if (vbM) vbM->Release();
							return;
						}
					}
					if (devV) devV->Release();
					if (ibV) ibV->Release();
				}
				// r208 (GPT P0): 每次 mask 写入前记录完整身份，供事后判定是否为目标几何。
				// 目的：ADS 过渡期准星提前出现在镜外 -> 怀疑 mask 被非目标几何/非主 pass 写入。
				{
					static int s_maskwLog = 0;
					if (s_maskwLog < 120) {
						++s_maskwLog;
						ID3D11Buffer* ibW = nullptr;
						DXGI_FORMAT fmtW = DXGI_FORMAT_UNKNOWN;
						UINT ioffW = 0;
						a_ctx->IAGetIndexBuffer(&ibW, &fmtW, &ioffW);
						D3D11_VIEWPORT vpW{};
						UINT vpNW = 1;
						a_ctx->RSGetViewports(&vpNW, &vpW);
						ID3D11RenderTargetView* rtvW[8] = {};
						ID3D11DepthStencilView* dsvW = nullptr;
						a_ctx->OMGetRenderTargets(8, rtvW, &dsvW);
						Log("[MASKW] #%d profile=%s | count=%u start=%u base=%d | VB=%p voff=%u stride=%u | IB=%p ioff=%u | vp=(%.0f,%.0f %.0fx%.0f) | rtv0=%p dsv=%p | target=%u",
							s_maskwLog, s_scopeProfile.name,
							a_count, a_start, a_base,
							(void*)vbM, voffM, strideM,
							(void*)ibW, ioffW,
							vpW.TopLeftX, vpW.TopLeftY, vpW.Width, vpW.Height,
							(void*)rtvW[0], (void*)dsvW,
							s_scopeProfile.targetIndexCount);
						for (int i = 0; i < 8; ++i) if (rtvW[i]) rtvW[i]->Release();
						if (dsvW) dsvW->Release();
						if (ibW) ibW->Release();
					}
				}

				// 1) 原始绘制
				s_origDrawIndexed(a_ctx, a_count, a_start, a_base);

				// 2) 追加 mask 绘制（只画 B 组 0..191）
				ID3D11RenderTargetView* oldRTVs[8] = {};
				ID3D11DepthStencilView* oldDSV = nullptr;
				a_ctx->OMGetRenderTargets(8, oldRTVs, &oldDSV);
				ID3D11PixelShader* oldPS = nullptr;
				a_ctx->PSGetShader(&oldPS, nullptr, nullptr);
				ID3D11DepthStencilState* oldDS = nullptr;
				UINT oldRef = 0;
				a_ctx->OMGetDepthStencilState(&oldDS, &oldRef);

				// 不清 mask：清空改到 Present 之后每帧一次，
				// 否则后一条 384 draw 会把前一条（Lense）的结果清掉
				ID3D11RenderTargetView* maskList[1] = { s_maskRTV };
				a_ctx->OMSetRenderTargets(1, maskList, nullptr);
				a_ctx->PSSetShader(s_psMask, nullptr, 0);
				a_ctx->OMSetDepthStencilState(s_dsNoDepthTest, 0);

				// r206: mask 纹理固定 1920x1080，但游戏颜色 pass 的 viewport 未必是这个尺寸。
				// 不设 viewport 时绘制会落在纹理的一角 -> 遮罩错位（表现为准星溢出/左右各一份）。
				D3D11_VIEWPORT oldVP{};
				UINT vpN = 1;
				a_ctx->RSGetViewports(&vpN, &oldVP);
				D3D11_VIEWPORT maskVP{ 0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f };
				a_ctx->RSSetViewports(1, &maskVP);

				// r223: 同理必须重置 scissor —— 游戏常带着上一 pass 的小裁剪矩形，
				// 会把我们这次 mask 绘制整个裁掉（实测 maskWrites=3802 但读回全 0）。
				D3D11_RECT oldScissor[16] = {};
				UINT scissorN = 16;
				a_ctx->RSGetScissorRects(&scissorN, oldScissor);
				D3D11_RECT fullScissor{ 0, 0, 1920, 1080 };
				a_ctx->RSSetScissorRects(1, &fullScissor);
				static int s_scissorLog = 0;
				if (s_scissorLog < 6) {
					++s_scissorLog;
					Log("[MASK-SCISSOR] old n=%u first=(%ld,%ld,%ld,%ld) -> set full 1920x1080",
						scissorN,
						scissorN > 0 ? oldScissor[0].left : 0L, scissorN > 0 ? oldScissor[0].top : 0L,
						scissorN > 0 ? oldScissor[0].right : 0L, scissorN > 0 ? oldScissor[0].bottom : 0L);
				}

				static int s_vpLog = 0;
				if (s_vpLog < 6) {
					++s_vpLog;
					Log("[MASK-VP] game vp=(%.0f,%.0f %.0fx%.0f) -> mask vp=1920x1080",
						oldVP.TopLeftX, oldVP.TopLeftY, oldVP.Width, oldVP.Height);
				}

				s_origDrawIndexed(a_ctx, s_scopeProfile.maskCount, a_start + s_scopeProfile.maskStart, a_base);

				// 恢复 viewport + scissor
				if (vpN > 0) a_ctx->RSSetViewports(1, &oldVP);
				if (scissorN > 0) a_ctx->RSSetScissorRects(scissorN, oldScissor);

				// 3) 恢复
				a_ctx->OMSetRenderTargets(8, oldRTVs, oldDSV);
				if (oldPS) { a_ctx->PSSetShader(oldPS, nullptr, 0); oldPS->Release(); }
				if (oldDS) { a_ctx->OMSetDepthStencilState(oldDS, oldRef); oldDS->Release(); }
				for (int i = 0; i < 8; ++i) if (oldRTVs[i]) oldRTVs[i]->Release();
				if (oldDSV) oldDSV->Release();

				static int s_maskLog = 0;
				++s_maskWrites;

				// r200: 严格识别按 profile 分流 —— K98k 专用（maxIdx<=81），ATACR 是白名单不需要。
				// 之前没做分流，ATACR 的 rear_glass（64 顶点，maxIdx=63）也满足 <=81，
				// 于是把 ATACR 的 voff 当成 K98k 的 Lense，切回 K98k 后目镜黑。
				const bool k98kProfile = (strcmp(s_scopeProfile.name, "K98k") == 0);
				if (!s_lenseIdentified && k98kProfile && a_count == s_scopeProfile.targetIndexCount) {
					if (s_idTries < 600) {
						++s_idTries;
						ID3D11Buffer* ibI = nullptr;
						DXGI_FORMAT fmtI = DXGI_FORMAT_UNKNOWN;
						UINT ioffI = 0;
						a_ctx->IAGetIndexBuffer(&ibI, &fmtI, &ioffI);
						ID3D11Device* devI = nullptr;
						a_ctx->GetDevice(&devI);
						if (ibI && devI) {
							const int mx = ReadIndexBufferMax(a_ctx, devI, ibI, ioffI, fmtI, a_count);
							if (mx >= 0 && mx <= 81) {
								s_lenseIdentified = true;
								// r182: 此时 vbM 仍然有效（引用还没释放），先建立独立引用
								if (s_lenseVB) { s_lenseVB->Release(); s_lenseVB = nullptr; }
								s_lenseVB = vbM;
								if (s_lenseVB) s_lenseVB->AddRef();
								s_lenseVoff = voffM;
								s_lenseStride = strideM;
								s_lenseIbAddr = (const void*)ibI;
								s_lenseIOff = ioffI;
								s_lenseIFmt = (int)fmtI;
								s_lenseMaxIdx = mx;
								Log("[LENSE-ID] color MATCH | VB=%p voff=%u stride=%u | IB=%p fmt=%d ioff=%u | start=%u count=%u | maxIdx=%d",
									(void*)vbM, voffM, strideM, (void*)ibI, (int)fmtI, ioffI,
									a_start, a_count, mx);
							} else {
								Log("[LENSE-ID] color reject | VB=%p voff=%u maxIdx=%d", (void*)vbM, voffM, mx);
							}
						}
						if (devI) devI->Release();
						if (ibI) ibI->Release();
					}
				}

				// r178: 记录这条几何的身份（宽松模式下的参考值）
				s_maskVB = vbM;
				s_maskVoff = voffM;
				if (s_maskLog < 40) {
					s_maskLog++;
					Log("[MASK] B-group drawn into mask (voff=%u) total=%d", voffM, (int)s_maskWrites);
				}
				if (vbM) vbM->Release();   // r182: 分支末尾统一释放
				return;
			}
			if (vbM) vbM->Release();       // r182: 未命中 color 条件时也要释放
			}   // r222: if (isMainColorPass)
		}

		// ======================= r166: 深度预 pass 不写深度（验证深度因果） =======================
		// 假设：深度预 pass 写入 Lense 深度 → 目镜区域内的场景被深度测试拒绝 → 全黑。
		// 实验：只把这条 pass 的 DepthWriteMask 改成 0，其余全部不动。
		if (g_skipDepthWrite && g_ads.load() && s_scopeProfile.targetIndexCount > 0
			&& a_count == s_scopeProfile.targetIndexCount && a_start == 0) {
			ID3D11PixelShader* psDW = nullptr;
			a_ctx->PSGetShader(&psDW, nullptr, nullptr);
			const bool isDepthDW = (psDW == nullptr);
			if (psDW) psDW->Release();

			ID3D11Buffer* vbDW = nullptr;
			UINT strideDW = 0, voffDW = 0;
			a_ctx->IAGetVertexBuffers(0, 1, &vbDW, &strideDW, &voffDW);

			// r188: 严格模式的安全兜底 —— 识别还没完成时按宽松处理。
			// 否则「一键开启 BTTS」(strict=on) 但识别未完成时，目镜会全黑。
			const bool isTargetGeo = !g_strictDW
				|| !s_lenseIdentified
				|| (s_lenseVB != nullptr && vbDW == s_lenseVB && voffDW == s_lenseVoff);

			// r183: 只在「严格模式 + 已完成识别」时记录，避免额度被识别前的记录耗尽
			{
				static int s_dwCheckLog = 0;
				if (g_strictDW && s_lenseIdentified && s_dwCheckLog < 40
				    && isDepthDW && strideDW == 20) {
					s_dwCheckLog++;
					Log("[DW-CHECK] curVB=%p voff=%u stride=%u | lenseVB=%p lenseVoff=%u id=%d | maskVB=%p maskVoff=%u | strict=%d -> %s",
						(void*)vbDW, voffDW, strideDW,
						(void*)s_lenseVB, s_lenseVoff, s_lenseIdentified ? 1 : 0,
						(void*)s_maskVB, s_maskVoff,
						g_strictDW ? 1 : 0,
						(isTargetGeo ? "SKIP" : "ALLOW"));
				}
			}
			if (vbDW) vbDW->Release();

			if (isDepthDW && strideDW > 0 && isTargetGeo && s_dsNoDepthWrite) {   // r198: 放开 stride
				ID3D11DepthStencilState* oldDS = nullptr;
				UINT oldRef = 0;
				a_ctx->OMGetDepthStencilState(&oldDS, &oldRef);
				a_ctx->OMSetDepthStencilState(s_dsNoDepthWrite, oldRef);
				s_origDrawIndexed(a_ctx, a_count, a_start, a_base);
				if (oldDS) { a_ctx->OMSetDepthStencilState(oldDS, oldRef); oldDS->Release(); }
				else { a_ctx->OMSetDepthStencilState(nullptr, oldRef); }

				static int s_dwLog = 0;
				if (s_dwLog < 6) {
					s_dwLog++;
					Log("[DW] depth-write skipped (target geo, voff=%u)", voffDW);
				}
				++s_dwSkipped;
				return;
			}
			// 非目标几何：记一次「放行」，证明范围被正确限制
			static int s_dwPassLog = 0;
			if (isDepthDW && strideDW == 20 && !isTargetGeo && s_dwPassLog < 6) {
				s_dwPassLog++;
				Log("[DW] depth-write LEFT ALONE (other geo, voff=%u maskVB=%p maskVoff=%u)",
					voffDW, (void*)s_maskVB, s_maskVoff);
			}
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
				// r168: 跳过深度写入时，十字也不能测深度（否则深度缓冲里没有 Lense 的深度，测试失败）
				if (g_skipDepthWrite && s_dsNoDepthTest) {
					a_ctx->OMSetDepthStencilState(s_dsNoDepthTest, 0);
				} else {
					a_ctx->OMSetDepthStencilState(s_dsNoStencil, 0);
				}

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

	// ======================= r191: 瞄具白名单（结构定义已上移到全局区）=======================

	// 递归查找名字包含指定子串的 TriShape
	static RE::BSTriShape* FindShapeLike(RE::NiAVObject* a_root, const char* a_sub)
	{
		if (!a_root) return nullptr;
		if (a_root->IsTriShape()) {
			if (strstr(a_root->name.c_str(), a_sub)) return a_root->IsTriShape();
			return nullptr;
		}
		if (auto* node = a_root->IsNode()) {
			for (auto& ch : node->children) {
				if (!ch) continue;
				if (auto* r = FindShapeLike(ch.get(), a_sub)) return r;
			}
		}
		return nullptr;
	}

	// r200: 清空 Lense 识别身份（profile 切换时调用）。
	// 必须先 Release 旧 COM 引用，再置空 —— 否则泄漏。
	static void ResetLenseIdentity()
	{
		if (s_lenseVB) { s_lenseVB->Release(); s_lenseVB = nullptr; }
		s_lenseIdentified = false;
		s_lenseVoff = 0;
		s_lenseMaxIdx = 0;
		s_lenseStride = 0;
		s_lenseIbAddr = nullptr;
		s_lenseIOff = 0;
		s_lenseIFmt = 0;
		s_idTries = 0;
	}

	// 根据 ScopeNormal 子树判定当前瞄具属于哪个已验证家族
	static void IdentifyScopeProfile(RE::NiAVObject* a_scopeNormal)
	{
		const std::string prevName = s_scopeProfile.name;
		s_scopeProfile = ScopeProfile{};
		if (!a_scopeNormal) return;

		// K98k：节点名含 "Lense"（注意 ATACR 是 LensFrontB，不含 "Lense"）
		if (auto* l = FindShapeLike(a_scopeNormal, "Lense")) {
			s_scopeProfile.name = "K98k";
			s_scopeProfile.targetIndexCount = l->numTriangles * 3;   // 128*3 = 384
			s_scopeProfile.maskStart = 0;
			s_scopeProfile.maskCount = 192;                          // 只画 B 组
			s_scopeProfile.expectedStride = 20;
			s_scopeProfile.maxVertexIdx = 81;   // Lense:0 有 82 个顶点
			Log("[PROFILE] K98k: shape=%s tri=%u -> idxCount=%u mask=%u@%u",
				l->name.c_str(), l->numTriangles, s_scopeProfile.targetIndexCount,
				s_scopeProfile.maskCount, s_scopeProfile.maskStart);
			if (prevName != "K98k") { s_cStatFrames = 0; s_cStatDetail = 0; }
			return;
		}

		// Nightforce ATACR：rear_glass 是独立 shape，整片都画
		if (auto* g = FindShapeLike(a_scopeNormal, "rear_glass")) {
			s_scopeProfile.name = "ATACR";
			s_scopeProfile.targetIndexCount = g->numTriangles * 3;   // 94*3 = 282
			s_scopeProfile.maskStart = 0;
			s_scopeProfile.maskCount = s_scopeProfile.targetIndexCount;
			s_scopeProfile.expectedStride = 24;   // r197 实测
			s_scopeProfile.maxVertexIdx = 63;   // rear_glass 有 64 个顶点
			Log("[PROFILE] ATACR: shape=%s tri=%u -> idxCount=%u mask=%u@%u",
				g->name.c_str(), g->numTriangles, s_scopeProfile.targetIndexCount,
				s_scopeProfile.maskCount, s_scopeProfile.maskStart);
			if (prevName != "ATACR") { s_cStatFrames = 0; s_cStatDetail = 0; }

			// r194: 记录目标节点与整条祖先链的 cull 状态（GPT 要求，仅取证不改行为）
			Log("[PROFILE-CULL] target=%s culled=%d", g->name.c_str(), g->GetAppCulled() ? 1 : 0);
			{
				auto* p = g->parent;
				int d = 0;
				while (p && d < 8) {
					Log("[PROFILE-CULL] chain[%d]=%s culled=%d", d, p->name.c_str(), p->GetAppCulled() ? 1 : 0);
					p = p->parent;
					++d;
				}
			}
			return;
		}

		// r228: Vortex Strike Eagle（M700 的另一个瞄具）
		// 结构：ScopeNormal 下只有 StrikeEagle_Lo(主体) + StrikeEagleLens_Lo(镜片)
		// 镜片 = 316 三角 / 180 顶点 -> 948 索引 / maxIdx <= 179
		// 注意它是 "Lens" 不是 "Lense"，所以 K98k 规则匹配不到。
		if (auto* g = FindShapeLike(a_scopeNormal, "StrikeEagleLens")) {
			s_scopeProfile.name = "VortexSE";
			s_scopeProfile.targetIndexCount = g->numTriangles * 3;   // 316*3 = 948
			s_scopeProfile.maskStart = 0;
			s_scopeProfile.maskCount = s_scopeProfile.targetIndexCount;
			s_scopeProfile.expectedStride = 0;                        // 未知，先不校验
			s_scopeProfile.maxVertexIdx = (g->numVertices > 0) ? (UINT)(g->numVertices - 1) : 0;
			Log("[PROFILE] VortexSE: shape=%s tri=%u vert=%u -> idxCount=%u mask=%u@%u maxIdx<=%u",
				g->name.c_str(), g->numTriangles, (unsigned)g->numVertices,
				s_scopeProfile.targetIndexCount, s_scopeProfile.maskCount, s_scopeProfile.maskStart,
				s_scopeProfile.maxVertexIdx);
			if (prevName != "VortexSE") { s_cStatFrames = 0; s_cStatDetail = 0; }
			return;
		}

		Log("[PROFILE] unknown scope (no Lense / rear_glass / StrikeEagleLens)");
	}

	// r201: 同步给菜单显示
	static void SyncProfileName()
	{
		strcpy_s(g_scopeProfileName, sizeof(g_scopeProfileName), s_scopeProfile.name);
		g_scopeValid = (strcmp(s_scopeProfile.name, "unknown") != 0);
	}

	// r230: 递归收集 shape。附加来源标记 'A'=ScopeAiming / 'N'=ScopeNormal。
	static void CollectShapesRec(RE::NiAVObject* a_obj, int a_depth, char a_source)
	{
		if (!a_obj || a_depth > 6 || s_shapeCount >= kMaxShapeEntries) return;
		if (auto* tri = a_obj->IsTriShape()) {
			ShapeEntry& e = s_shapeList[s_shapeCount];
			strncpy_s(e.name, sizeof(e.name), a_obj->name.c_str(), _TRUNCATE);
			e.tri = tri->numTriangles;
			e.vert = (UINT)tri->numVertices;
			e.count = tri->numTriangles * 3;
			e.maxIdx = (e.vert > 0) ? (e.vert - 1) : 0;
			e.source = a_source;
			++s_shapeCount;
			return;
		}
		if (auto* node = a_obj->IsNode()) {
			for (auto& ch : node->children) {
				if (ch) CollectShapesRec(ch.get(), a_depth + 1, a_source);
			}
		}
	}

	// r230: 同时扫描 ScopeAiming（ADS 可见，透明玻璃）和 ScopeNormal（腰射可见，不透明目镜）。
	// 用户实测：Vortex 在开镜时会把不透明的 normal 目镜换成 aiming 的透明玻璃，
	// 只扫 ScopeNormal 会选到开镜后已被剔除的那片。
	static void CollectShapes(RE::PlayerCharacter* a_pc)
	{
		s_shapeCount = 0;
		s_shapeListDirty = false;
		if (auto* aiming = FindNode(a_pc, "ScopeAiming")) {
			CollectShapesRec(aiming, 0, 'A');
		}
		if (auto* normal = FindNode(a_pc, "ScopeNormal")) {
			CollectShapesRec(normal, 0, 'N');
		}
		Log("[ADAPTER] scanned %d shapes (A=ScopeAiming / N=ScopeNormal)", s_shapeCount);
		for (int i = 0; i < s_shapeCount; ++i) {
			Log("[ADAPTER]   [%d] (%c) %s tri=%u vert=%u -> count=%u maxIdx<=%u",
				i, s_shapeList[i].source, s_shapeList[i].name, s_shapeList[i].tri,
				s_shapeList[i].vert, s_shapeList[i].count, s_shapeList[i].maxIdx);
		}
	}

	// r229: 应用用户选中的 shape 作为临时 profile
	static bool ApplyUserPickedShape(int a_index)
	{
		if (a_index < 0) { s_userPickedShape = -1; Log("[ADAPTER] user pick cleared"); return true; }
		if (a_index >= s_shapeCount) return false;
		s_userPickedShape = a_index;
		const ShapeEntry& e = s_shapeList[a_index];
		s_scopeProfile = ScopeProfile{};
		s_scopeProfile.name = "UserPick";
		s_scopeProfile.targetIndexCount = e.count;
		s_scopeProfile.maskStart = 0;
		s_scopeProfile.maskCount = e.count;
		s_scopeProfile.expectedStride = 0;   // 未知，不校验
		s_scopeProfile.maxVertexIdx = e.maxIdx;
		ResetLenseIdentity();
		SyncProfileName();
		Log("[ADAPTER] applied user pick [%d] %s -> count=%u maxIdx<=%u",
			a_index, e.name, e.count, e.maxIdx);
		return true;
	}

	// r229: 给 GUI 的导出接口（同 namespace 内，非 static）
	int RC_ShapeCount() { return s_shapeCount; }

	bool RC_GetShape(int a_index, ShapeEntryView& a_out)
	{
		if (a_index < 0 || a_index >= s_shapeCount) return false;
		const ShapeEntry& e = s_shapeList[a_index];
		strncpy_s(a_out.name, sizeof(a_out.name), e.name, _TRUNCATE);
		a_out.tri = e.tri;
		a_out.vert = e.vert;
		a_out.count = e.count;
		a_out.maxIdx = e.maxIdx;
		return true;
	}

	bool RC_ApplyShape(int a_index) { return ApplyUserPickedShape(a_index); }

	int RC_UserPicked() { return s_userPickedShape; }

	// ======================= ScopeNormal 子树递归枚举 =======================
	// r189: 位置采集上下文（相机 / 瞄具原点 / 三根局部轴），供 DumpSubtreeRec 使用
	static RE::NiPoint3 s_subCamPos{};
	static bool         s_subHasCam = false;
	static RE::NiPoint3 s_subOrigin{};
	static RE::NiPoint3 s_subAxis[3]{};

	// 递归列出整个 ScopeNormal 下的所有节点/几何，确认 [NORMAL] 是否漏了 shape
	static void DumpSubtreeRec(RE::NiAVObject* a_obj, int a_depth, int& a_shapes)
	{
		if (!a_obj || a_depth > 6) return;
		const char* type = "?";
		auto* tri = a_obj->IsTriShape();
		if (tri) {
			type = "TriShape";
			a_shapes++;
			// r189: 位置数据语义说明
			//   nodeW  = a_obj->world.translate     —— 节点的世界变换平移（子节点常与父相同）
			//   boundC = a_obj->worldBound.center   —— 几何包围球中心（世界空间）
			//   dist   = 相机到 boundC 的欧氏距离
			//   projX/Y/Z = (boundC - scopeOrigin) 在瞄具三根局部轴上的有符号投影
			const auto& w = a_obj->world;
			const auto& wb = a_obj->worldBound;

			float dist = -1.0f;
			if (s_subHasCam) {
				const float dx = wb.center.x - s_subCamPos.x;
				const float dy = wb.center.y - s_subCamPos.y;
				const float dz = wb.center.z - s_subCamPos.z;
				dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			}
			const float rx = wb.center.x - s_subOrigin.x;
			const float ry = wb.center.y - s_subOrigin.y;
			const float rz = wb.center.z - s_subOrigin.z;
			const float px = s_subAxis[0].x * rx + s_subAxis[0].y * ry + s_subAxis[0].z * rz;
			const float py = s_subAxis[1].x * rx + s_subAxis[1].y * ry + s_subAxis[1].z * rz;
			const float pz = s_subAxis[2].x * rx + s_subAxis[2].y * ry + s_subAxis[2].z * rz;

			Log("[SUB] %*s%s tri=%u vert=%u cull=%d | nodeW=(%.1f,%.1f,%.1f) | boundC=(%.1f,%.1f,%.1f) r=%.2f | dist=%.2f | projX=%.2f projY=%.2f projZ=%.2f",
				a_depth * 2, "", a_obj->name.c_str(),
				tri->numTriangles, (unsigned)tri->numVertices, a_obj->GetAppCulled() ? 1 : 0,
				w.translate.x, w.translate.y, w.translate.z,
				wb.center.x, wb.center.y, wb.center.z, wb.fRadius,
				dist, px, py, pz);
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
			// r177: 记录 ADS 时被我们 cull 的节点（按名字 + 原始状态）
			struct CullRec { std::string name; bool orig; };
			static std::vector<CullRec> s_cullRec;
			static bool s_lastADS = false;
			static int s_frame = 0;
			static int s_hideLog = 0;
			static bool s_fovSaved = false;
			static float s_origFov = 0.0f;

			const bool ads = IsInADS(a_pc);
			g_ads.store(ads);
			if (ads) s_frame++;

			if (ads) {
				// r179: 只在「刚进入 ADS」时建立一次原始状态快照。
				// 关键修复：原来 s_cullRec.clear() 写在每帧路径里，
				// 导致 orig 记录到的是我们上一帧改过的值，退出时恢复失真。
				if (!s_lastADS) s_cullRec.clear();

				// 每帧算镜片投影（诊断用）
				UpdateMask(a_pc);

				// r178: 统一的状态记录 —— 任何我们改过的节点，都先记录原始 AppCulled 再改，
				// 退出 ADS 时按记录恢复。修复「Lense 未记录导致腰射悬空黑圆盘」。
				auto SetCullRecorded = [](RE::NiAVObject* a_node, bool a_culled) {
					if (!a_node) return;
					const std::string nm = a_node->name.c_str();
					bool already = false;
					for (auto& r : s_cullRec) {
						if (r.name == nm) { already = true; break; }
					}
					if (!already) s_cullRec.push_back({ nm, a_node->GetAppCulled() });
					if (a_node->GetAppCulled() != a_culled) a_node->SetAppCulled(a_culled);
				};
				// 诊断用的「互斥显示」：只在显式选择 polySurface/Mount 时使用
				auto ShowOnly = [a_pc, &SetCullRecorded](const char* a_target) {
					auto* n = FindNode(a_pc, a_target);
					if (!n) return;
					SetCullRecorded(n, false);
					auto* p = n->parent;
					int guard = 0;
					while (p && guard++ < 10) {
						SetCullRecorded(p, false);
						p = p->parent;
					}
					const char* others[] = { "Lense", "polySurface22", "Mount1", "Mount2" };
					for (auto* o : others) {
						if (strcmp(o, a_target) == 0) continue;
						if (auto* on = FindNode(a_pc, o)) SetCullRecorded(on, true);
					}
				};
				if (g_forceLense) {
					// r231: 按 profile 选目标节点。
					// K98k = "Lense"，ATACR = "rear_glass"，VortexSE = "StrikeEagleLens"。
					// 用户选中的 shape 优先（适配器 Phase 1）——直接用它的节点名精确查找。
					RE::NiAVObject* target = nullptr;
					if (s_userPickedShape >= 0 && s_userPickedShape < s_shapeCount) {
						target = FindNode(a_pc, s_shapeList[s_userPickedShape].name);
						if (!target) {
							Log("[CULL] user-picked shape '%s' not found in scene tree",
								s_shapeList[s_userPickedShape].name);
						}
					} else if (auto* snP = FindNode(a_pc, "ScopeNormal")) {
						if (strcmp(s_scopeProfile.name, "ATACR") == 0) {
							target = FindShapeLike(snP, "rear_glass");
						} else if (strcmp(s_scopeProfile.name, "VortexSE") == 0) {
							target = FindShapeLike(snP, "StrikeEagleLens");
						} else {
							target = FindShapeLike(snP, "Lense");
						}
					}
					if (target) {
						// r232 (GPT 步骤 1): 记录目标完整父链 + 每级 cull 状态，不改行为。
						// 目的：确认 ScopeNormal/ScopeAiming 是否是安全边界、镜筒支架挂在哪。
						{
							static int s_chainLog = 0;
							if (s_chainLog < 6) {
								++s_chainLog;
								Log("[CHAIN] === target=%s (profile=%s) ===", target->name.c_str(), s_scopeProfile.name);
								Log("[CHAIN] target cull=%d tri=%d",
									target->GetAppCulled() ? 1 : 0, target->IsTriShape() ? 1 : 0);
								auto* q = target->parent;
								int d = 0;
								while (q && d < 12) {
									Log("[CHAIN]   parent[%d] %s cull=%d tri=%d",
										d, q->name.c_str(), q->GetAppCulled() ? 1 : 0, q->IsTriShape() ? 1 : 0);
									q = q->parent;
									++d;
								}
								// 同子树下的兄弟 shape（判断谁会被祖先 uncull 带出来）
								auto* root = FindNode(a_pc, "ScopeNormal");
								if (!root) root = FindNode(a_pc, "ScopeAiming");
								if (auto* rn = root ? root->IsNode() : nullptr) {
									Log("[CHAIN] siblings under %s:", root->name.c_str());
									for (auto& ch : rn->children) {
										if (!ch) continue;
										auto* t2 = ch->IsTriShape();
										Log("[CHAIN]   sib %s tri=%d vert=%u cull=%d",
											ch->name.c_str(), t2 ? 1 : 0,
											t2 ? (unsigned)t2->numVertices : 0u,
											ch->GetAppCulled() ? 1 : 0);
									}
								}
							}
						}

						SetCullRecorded(target, false);
						auto* p = target->parent;
						int guard = 0;
						while (p && guard++ < 10) {
							SetCullRecorded(p, false);
							p = p->parent;
						}
					} else {
						Log("[CULL] forceLense: target NOT found (profile=%s)", s_scopeProfile.name);
					}

					// r233: 隐藏同子树里的其他几何。
					// 实测：ScopeNormal 本身 cull=1，uncull 它才能看见目标镜片，
					// 但这也把兄弟节点（镜筒 StrikeEagle_Lo / 主体 atacr18 / 支架）一起放出来了。
					// K98k 时代是靠硬编码 polySurface22/Mount1/Mount2 掩盖的，这里改成通用做法：
					// 在 ScopeNormal + ScopeAiming 子树内，除目标外的所有 TriShape 一律隐藏。
					if (target) {
						int hidden = 0;
						auto hideSiblings = [&](RE::NiAVObject* a_root) {
							if (!a_root) return;
							std::vector<RE::NiAVObject*> stack{ a_root };
							int guard = 0;
							while (!stack.empty() && guard++ < 512) {
								RE::NiAVObject* cur = stack.back();
								stack.pop_back();
								if (!cur) continue;
								if (auto* t = cur->IsTriShape()) {
									if (cur != target) {
										SetCullRecorded(cur, true);
										++hidden;
									}
									continue;   // shape 不再往下走
								}
								if (auto* n = cur->IsNode()) {
									for (auto& ch : n->children) {
										if (ch) stack.push_back(ch.get());
									}
								}
							}
						};
						// r234: 只处理目标所在的子树，另一个子树完全不碰。
						// ScopeAiming 装的是 STS 开镜时要显示的内容（HuntingScopeGlass / ScopeFade /
						// StrikeEagle_Aim / ScopeInside_Lo ...），cull 它会把整个瞄准画面干掉。
						RE::NiAVObject* targetRoot = nullptr;
						if (auto* sn = FindNode(a_pc, "ScopeNormal")) {
							if (FindShapeLike(sn, target->name.c_str()) == target) targetRoot = sn;
						}
						if (!targetRoot) {
							if (auto* sa = FindNode(a_pc, "ScopeAiming")) {
								if (FindShapeLike(sa, target->name.c_str()) == target) targetRoot = sa;
							}
						}
						if (targetRoot) {
							hideSiblings(targetRoot);
							Log("[CULL] hid %d sibling shape(s) inside %s (kept target=%s)",
								hidden, targetRoot->name.c_str(), target->name.c_str());
						} else {
							Log("[CULL] target %s not under ScopeNormal/ScopeAiming - skip sibling hide",
								target->name.c_str());
						}
					}

					// 2) K98k 的旧硬编码保留（对其它瞄具是 no-op）
					const char* others[] = { "polySurface22", "Mount1", "Mount2" };
					for (auto* o : others) {
						if (auto* on = FindNode(a_pc, o)) SetCullRecorded(on, true);
					}
					Log("[CULL] ADS enter: profile=%s modified %zu nodes (orig recorded)", s_scopeProfile.name, s_cullRec.size());
				}
				else if (g_forcePoly)    ShowOnly("polySurface22");
				else if (g_forceMount1)  ShowOnly("Mount1");
				else if (g_forceMount2)  ShowOnly("Mount2");
				// 注意：不做「取消勾选后恢复」。SetAppCulled 是节点状态，我们无权改动，
				// 强行 cull 会污染 STS 自己的剔除逻辑（导致腰射没有瞄具）。
				// 取消勾选时什么都不做，让 STS 按自己的 ADS 逻辑管理可见性。

				// 透镜只在开镜时可见；旧方案关闭时必须强制隐藏，否则 RCLens 会挡在目镜位置
				if (s_lensNode) {
					const bool wantVisible = g_lensEnabled;
					if (s_lensNode->GetAppCulled() == wantVisible) {
						s_lensNode->SetAppCulled(!wantVisible);
					}
				}

				// r220: 安全探针 —— 只读指针和 vtable[0]，不读任何成员字段。
				// 上一版用 +0x58 读 material 得到 0x4010ED（代码段地址），说明 properties[0] 不是 BSShaderProperty。
				{
					static bool s_propProbe = false;
					if (!s_propProbe) {
						if (auto* retP = FindNode(a_pc, "Reticle")) {
							if (auto* triP = retP->IsTriShape()) {
								s_propProbe = true;
								auto* p0 = triP->properties[0].get();
								auto* p1 = triP->properties[1].get();
								Log("[PROP] Reticle tri=%u vert=%u | p0=%p p1=%p",
									triP->numTriangles, (unsigned)triP->numVertices, (void*)p0, (void*)p1);
								if (p0) {
									auto** v0 = *(void***)p0;
									Log("[PROP] p0 vtbl=%p [0]=%p [1]=%p [2]=%p",
										(void*)v0, v0 ? v0[0] : nullptr, v0 ? v0[1] : nullptr, v0 ? v0[2] : nullptr);
								}
								if (p1) {
									auto** v1 = *(void***)p1;
									Log("[PROP] p1 vtbl=%p [0]=%p [1]=%p [2]=%p",
										(void*)v1, v1 ? v1[0] : nullptr, v1 ? v1[1] : nullptr, v1 ? v1[2] : nullptr);
								}
							}
						}
					}
				}

				// 处理 STS 原准星（贴在瞄具上会跟着晃）
				// r202: 加开关 g_showScopeReticle —— 默认隐藏（false），用户可手动显示
				{
					auto* ret = FindNode(a_pc, "Reticle");
					if (ret) {
						const bool wantHidden = !g_showScopeReticle;
						if (wantHidden != ret->GetAppCulled()) {
							ret->SetAppCulled(wantHidden);
							if (s_hideLog < 20) {
								s_hideLog++;
								Log("[CULL] %s %s", wantHidden ? "hid" : "showed", ret->name.c_str());
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
						// r229: 扫描子树里的所有 shape，供 GUI 适配器列表用
						CollectShapes(a_pc);
						// r191: 判定当前瞄具属于哪个已验证家族（白名单）
						IdentifyScopeProfile(sn2);
						// r229: 用户点选的 shape 优先于内置识别（临时 profile，不写盘）
						if (s_userPickedShape >= 0 && s_userPickedShape < s_shapeCount) {
							ApplyUserPickedShape(s_userPickedShape);
						}
					SyncProfileName();
						// r200: profile 真正变化时重置旧身份（A+B 方案的 B）
						{
							static std::string s_prevProfile;
							if (s_prevProfile != s_scopeProfile.name) {
								Log("[PROFILE-CHANGE] old=%s new=%s reset=1",
									s_prevProfile.c_str(), s_scopeProfile.name);
								s_prevProfile = s_scopeProfile.name;
								ResetLenseIdentity();
							}
						}

						// r184: 只要瞄具节点变了就重新 dump，不再限制总次数
						// （换武器/换瞄具后 ScopeNormal 指针会变，这样每个瞄具都能拿到自己的结构）
						static RE::NiAVObject* s_lastScopeNormal = nullptr;
						static std::string s_lastWeaponName;
						std::string wname;
						if (auto* w3d = a_pc->Get3D()) {
							if (auto* w = w3d->GetObjectByName("Weapon")) wname = w->name.c_str();
						}
						if (sn2 != s_lastScopeNormal || wname != s_lastWeaponName) {
							s_lastScopeNormal = sn2;
							s_lastWeaponName = wname;

							// r189: 采集相机位置与瞄具原点/三根局部轴
							s_subHasCam = false;
							if (auto* cam = RE::PlayerCamera::GetSingleton()) {
								if (cam->cameraRoot) {
									s_subCamPos = cam->cameraRoot->world.translate;
									s_subHasCam = true;
								}
							}
							s_subOrigin = sn2->world.translate;
							{
								const auto& r = sn2->world.rotate;
								s_subAxis[0] = { r.entry[0].pt[0], r.entry[0].pt[1], r.entry[0].pt[2] };
								s_subAxis[1] = { r.entry[1].pt[0], r.entry[1].pt[1], r.entry[1].pt[2] };
								s_subAxis[2] = { r.entry[2].pt[0], r.entry[2].pt[1], r.entry[2].pt[2] };
							}

							int shapes = 0;
							Log("[SUB] === ScopeNormal subtree (weapon=%s) ===", wname.c_str());
							Log("[SUB] camera=%s(%.1f,%.1f,%.1f) | scopeOrigin=(%.1f,%.1f,%.1f)",
								s_subHasCam ? "" : "NONE",
								s_subCamPos.x, s_subCamPos.y, s_subCamPos.z,
								s_subOrigin.x, s_subOrigin.y, s_subOrigin.z);
							Log("[SUB] axis0=(%.3f,%.3f,%.3f) axis1=(%.3f,%.3f,%.3f) axis2=(%.3f,%.3f,%.3f)",
								s_subAxis[0].x, s_subAxis[0].y, s_subAxis[0].z,
								s_subAxis[1].x, s_subAxis[1].y, s_subAxis[1].z,
								s_subAxis[2].x, s_subAxis[2].y, s_subAxis[2].z);
							DumpSubtreeRec(sn2, 0, shapes);
							Log("[SUB] === total drawable shapes = %d ===", shapes);
						}
					} else {
						// r221: 没有 ScopeNormal —— 机械瞄具 / 空手 / 非 STS 武器。
						// 必须清空 profile：否则 g_scopeValid 会沿用上一把枪的值，
						// 结果机瞄也画屏幕准星（用户报告的泄漏）。
						if (strcmp(s_scopeProfile.name, "unknown") != 0) {
							Log("[PROFILE-CHANGE] old=%s new=unknown (no ScopeNormal)", s_scopeProfile.name);
							s_scopeProfile = ScopeProfile{};
							SyncProfileName();
							ResetLenseIdentity();
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
				// r178: 逆序恢复所有被我们改过的节点（后改的先恢复），
				// 按名字重新查找以避免悬空指针；恢复的是记录的原始值而非固定 false。
				for (auto it = s_cullRec.rbegin(); it != s_cullRec.rend(); ++it) {
					if (auto* n = FindNode(a_pc, it->name.c_str())) {
						if (n->GetAppCulled() != it->orig) n->SetAppCulled(it->orig);
					}
				}
				if (!s_cullRec.empty()) Log("[CULL] ADS exit: restored %zu nodes", s_cullRec.size());
				s_cullRec.clear();
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

		// r195: 颜色 pass 全量统计摘要（每帧输出一次，然后清零）
		if (s_cStatTotal > 0) {
			if (s_cStatFrames < 40) {
				++s_cStatFrames;
				// 拼出 stride 分布（只列非零）
				char buf[256] = {};
				int w = 0;
				for (int i = 0; i < 65; ++i) {
					if (s_cStatStride[i] > 0 && w < 200) {
						w += sprintf_s(buf + w, sizeof(buf) - w, "%d:%d ", i, s_cStatStride[i]);
					}
				}
				Log("[CSTAT] colorDraws=%d | small(100-1000)=%d large=%d startNZ=%d | stride[%s] | profile=%s",
					s_cStatTotal, s_cStatSmall, s_cStatLarge, s_cStatStartNZ, buf, s_scopeProfile.name);
			}
			s_cStatTotal = 0;
			s_cStatSmall = 0;
			s_cStatLarge = 0;
			s_cStatStartNZ = 0;
			ZeroMemory(s_cStatStride, sizeof(s_cStatStride));
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
		// r170: M1 调试视图也要能单独打开（不依赖旧 overlay 开关）
		if (g_screenOverlay || g_maskDebug) OverlayDraw(a_sc);

		// r172: 诊断 —— 读回 mask 中心像素，确认 mask 是否真的有内容
		if (g_maskEnabled && s_maskTex && s_maskStaging) {
			static int s_rbTick = 0;
			static int s_rbLog = 0;
			if ((s_rbTick++ % 60) == 0 && s_rbLog < 6) {
				s_rbLog++;
				auto* rdR = RE::BSGraphics::RendererData::GetSingleton();
				if (rdR && rdR->context) {
					auto* cR = (ID3D11DeviceContext*)static_cast<void*>(rdR->context);
					cR->CopyResource(s_maskStaging, s_maskTex);
					D3D11_MAPPED_SUBRESOURCE mr{};
					if (SUCCEEDED(cR->Map(s_maskStaging, 0, D3D11_MAP_READ, 0, &mr))) {
						// r224: 多点采样 —— 之前只看中心和左上角，漏掉了"左侧有内容"的情况
						auto px = [&](UINT x, UINT y) -> unsigned {
							const uint8_t* r2 = (const uint8_t*)mr.pData + (size_t)y * mr.RowPitch;
							return (unsigned)r2[x];
						};
						// 统计整张 mask 的非零像素数（步长 8，足够快）
						unsigned nz = 0;
						for (UINT y = 0; y < 1080; y += 8) {
							const uint8_t* r2 = (const uint8_t*)mr.pData + (size_t)y * mr.RowPitch;
							for (UINT x = 0; x < 1920; x += 8) {
								if (r2[x]) ++nz;
							}
						}
						Log("[MASK-RB] center=%u | TL=%u TR=%u BL=%u BR=%u | L=%u R=%u T=%u B=%u | nonzero=%u | writes=%d dwSkip=%d",
							px(960, 540),
							px(0, 0), px(1919, 0), px(0, 1079), px(1919, 1079),
							px(300, 540), px(1600, 540), px(960, 200), px(960, 880),
							nz, s_maskWrites.load(), s_dwSkipped.load());
						cR->Unmap(s_maskStaging, 0);
					}
				}
			}
		}

		// r171: overlay 采样完之后清 mask，为下一帧的 Lense 绘制做准备
		if (g_maskEnabled && s_maskRTV) {
			auto* rdM = RE::BSGraphics::RendererData::GetSingleton();
			if (rdM && rdM->context) {
				const float clearM[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				((ID3D11DeviceContext*)static_cast<void*>(rdM->context))->ClearRenderTargetView(s_maskRTV, clearM);
			}
		}
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
		Log("[BUILD] BudgetThroughScope r234 (only-target-subtree) %s %s", __DATE__, __TIME__);
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
