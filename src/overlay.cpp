#include "PCH.h"
#include "Shared.h"

namespace RC
{
	// ======================= DDS 加载（只处理 mip0） =======================
	// 支持：BC1(DXT1) / BC3(DXT5) / BC7(DX10) / R8G8B8A8
	struct DdsInfo
	{
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t dataOffset = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint32_t rowPitch = 0;
	};

	static bool ParseDds(const uint8_t* a_data, size_t a_size, DdsInfo& a_out)
	{
		if (a_size < 128) return false;
		if (std::memcmp(a_data, "DDS ", 4) != 0) return false;

		const uint32_t height = *reinterpret_cast<const uint32_t*>(a_data + 12);
		const uint32_t width = *reinterpret_cast<const uint32_t*>(a_data + 16);
		const uint32_t pfFlags = *reinterpret_cast<const uint32_t*>(a_data + 80);
		const char* fourCC = reinterpret_cast<const char*>(a_data + 84);

		a_out.width = width;
		a_out.height = height;
		a_out.dataOffset = 128;

		if (std::memcmp(fourCC, "DX10", 4) == 0) {
			if (a_size < 148) return false;
			a_out.dataOffset = 148;
			a_out.format = static_cast<DXGI_FORMAT>(*reinterpret_cast<const uint32_t*>(a_data + 128));
		} else if (std::memcmp(fourCC, "DXT1", 4) == 0) {
			a_out.format = DXGI_FORMAT_BC1_UNORM;
		} else if (std::memcmp(fourCC, "DXT5", 4) == 0) {
			a_out.format = DXGI_FORMAT_BC3_UNORM;
		} else if ((pfFlags & 0x40) != 0) {  // DDPF_RGB
			a_out.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		} else {
			return false;
		}

		// 每块字节数：BC1=8，BC3/BC7=16，未压缩 RGBA8 按像素算
		uint32_t bytesPerBlock = 16;
		if (a_out.format == DXGI_FORMAT_BC1_UNORM || a_out.format == DXGI_FORMAT_BC1_UNORM_SRGB) {
			bytesPerBlock = 8;
		}
		a_out.rowPitch = (a_out.format == DXGI_FORMAT_R8G8B8A8_UNORM)
			? width * 4u
			: ((width + 3u) / 4u) * bytesPerBlock;

		const uint64_t need = (uint64_t)a_out.dataOffset + (uint64_t)a_out.rowPitch * ((height + 3u) / 4u);
		if ((uint64_t)a_size < need && a_out.format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
		return width > 0 && height > 0;
	}

	static bool LoadDds(ID3D11Device* a_dev, const wchar_t* a_path, ID3D11ShaderResourceView** a_outSrv)
	{
		HANDLE f = ::CreateFileW(a_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f == INVALID_HANDLE_VALUE) return false;

		LARGE_INTEGER sz{};
		::GetFileSizeEx(f, &sz);
		std::vector<uint8_t> buf(static_cast<size_t>(sz.QuadPart));
		DWORD read = 0;
		const bool okRead = ::ReadFile(f, buf.data(), (DWORD)buf.size(), &read, nullptr) != 0;
		::CloseHandle(f);
		if (!okRead) return false;

		DdsInfo info;
		if (!ParseDds(buf.data(), buf.size(), info)) return false;

		D3D11_TEXTURE2D_DESC td{};
		td.Width = info.width;
		td.Height = info.height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = info.format;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA init{};
		init.pSysMem = buf.data() + info.dataOffset;
		init.SysMemPitch = info.rowPitch;

		ID3D11Texture2D* tex = nullptr;
		if (FAILED(a_dev->CreateTexture2D(&td, &init, &tex)) || !tex) return false;

		const HRESULT hr = a_dev->CreateShaderResourceView(tex, nullptr, a_outSrv);
		tex->Release();
		return SUCCEEDED(hr);
	}

	// ======================= Shader =======================
	static const char* kVS = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
cbuffer Cb : register(b0) {
    float screenW, screenH, centerX, centerY, boxW, boxH, alphaMul, maskR;
    float maskCX, maskCY, pad0, pad1;
};
VSOut main(uint id : SV_VertexID) {
    float2 c[4]  = { float2(-0.5,-0.5), float2(0.5,-0.5), float2(-0.5,0.5), float2(0.5,0.5) };
    float2 uv[4] = { float2(0.0,0.0),  float2(1.0,0.0),  float2(0.0,1.0), float2(1.0,1.0) };
    float2 px = float2(centerX, centerY) + c[id] * float2(boxW, boxH);
    VSOut o;
    o.pos = float4( px.x / screenW * 2.0 - 1.0,
                    1.0 - px.y / screenH * 2.0,
                    0.5, 1.0 );
    o.uv = uv[id];
    return o;
}
)";

	static const char* kPS = R"(
cbuffer Cb : register(b0) {
    float screenW, screenH, centerX, centerY, boxW, boxH, alphaMul, maskR;
    float maskCX, maskCY, pad0, pad1;
};
Texture2D    gTex  : register(t0);
SamplerState gSamp : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    if (maskR > 0.5) {
        float2 d = pos.xy - float2(maskCX, maskCY);
        float dist = sqrt(dot(d, d));
        if (dist > maskR) discard;
        // 调试用：遮罩边界画一圈红色，方便肉眼确认圆的位置和大小
        if (dist > maskR - 4.0) return float4(1.0, 0.0, 0.0, 1.0);
    }
    float4 c = gTex.Sample(gSamp, uv);
    return float4(c.rgb, c.a * alphaMul);
}
)";

	struct CbData
	{
		float screenW, screenH;
		float centerX, centerY;
		float boxW, boxH;
		float alphaMul, maskR;
		float maskCX, maskCY;
		float pad0, pad1;
	};

	struct State
	{
		ID3D11VertexShader* vs = nullptr;
		ID3D11PixelShader* ps = nullptr;
		ID3D11Buffer* cb = nullptr;
		ID3D11BlendState* blend = nullptr;
		ID3D11DepthStencilState* ds = nullptr;
		ID3D11RasterizerState* rs = nullptr;
		ID3D11SamplerState* samp = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		ID3D11Texture2D* bbTex = nullptr;
		ID3D11RenderTargetView* bbRtv = nullptr;
		bool tried = false;
		bool ok = false;
		bool texFromFile = false;
		int drawLogs = 0;
	};

	static State s_state;

	typedef HRESULT(WINAPI* D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
		ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

	static bool InitState(ID3D11Device* a_dev)
	{
		if (s_state.tried) return s_state.ok;
		s_state.tried = true;

		HMODULE comp = ::LoadLibraryA("d3dcompiler_47.dll");
		if (!comp) { BoxLog("[OVL] d3dcompiler_47 load FAILED"); return false; }
		auto compile = (D3DCompile_t)::GetProcAddress(comp, "D3DCompile");
		if (!compile) { BoxLog("[OVL] D3DCompile not found"); return false; }

		ID3DBlob* vsBlob = nullptr;
		ID3DBlob* psBlob = nullptr;
		ID3DBlob* err = nullptr;

		if (FAILED(compile(kVS, strlen(kVS), "RC_VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &err))) {
			BoxLog("[OVL] VS compile FAILED: %s", err ? (const char*)err->GetBufferPointer() : "?");
			if (err) err->Release();
			return false;
		}
		if (FAILED(compile(kPS, strlen(kPS), "RC_PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &err))) {
			BoxLog("[OVL] PS compile FAILED: %s", err ? (const char*)err->GetBufferPointer() : "?");
			if (err) err->Release();
			vsBlob->Release();
			return false;
		}

		HRESULT hv = a_dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &s_state.vs);
		HRESULT hp = a_dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &s_state.ps);
		vsBlob->Release();
		psBlob->Release();
		if (FAILED(hv) || FAILED(hp)) {
			BoxLog("[OVL] Create*Shader FAILED 0x%08X/0x%08X", (unsigned)hv, (unsigned)hp);
			return false;
		}

		D3D11_BUFFER_DESC cbd{};
		cbd.ByteWidth = sizeof(CbData);
		cbd.Usage = D3D11_USAGE_DYNAMIC;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(a_dev->CreateBuffer(&cbd, nullptr, &s_state.cb))) { BoxLog("[OVL] CreateBuffer FAILED"); return false; }

		D3D11_BLEND_DESC bd{};
		bd.RenderTarget[0].BlendEnable = TRUE;
		bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(a_dev->CreateBlendState(&bd, &s_state.blend))) { BoxLog("[OVL] CreateBlendState FAILED"); return false; }

		D3D11_DEPTH_STENCIL_DESC dd{};
		dd.DepthEnable = FALSE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
		dd.StencilEnable = FALSE;
		if (FAILED(a_dev->CreateDepthStencilState(&dd, &s_state.ds))) { BoxLog("[OVL] CreateDepthStencilState FAILED"); return false; }

		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = FALSE;
		rd.ScissorEnable = FALSE;
		if (FAILED(a_dev->CreateRasterizerState(&rd, &s_state.rs))) { BoxLog("[OVL] CreateRasterizerState FAILED"); return false; }

		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.MinLOD = 0.0f;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(a_dev->CreateSamplerState(&sd, &s_state.samp))) { BoxLog("[OVL] CreateSamplerState FAILED"); return false; }

		// 贴图
		wchar_t wpath[MAX_PATH] = {};
		::MultiByteToWideChar(CP_ACP, 0, kReticleDds, -1, wpath, MAX_PATH);
		if (LoadDds(a_dev, wpath, &s_state.srv)) {
			s_state.texFromFile = true;
			BoxLog("[OVL] reticle DDS loaded: %s", kReticleDds);
		} else {
			// 1x1 绿色兜底
			const uint32_t px = 0xFF00FF00;
			D3D11_TEXTURE2D_DESC td{};
			td.Width = td.Height = 1;
			td.MipLevels = td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_IMMUTABLE;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA init{};
			init.pSysMem = &px;
			init.SysMemPitch = 4;
			ID3D11Texture2D* t = nullptr;
			if (FAILED(a_dev->CreateTexture2D(&td, &init, &t)) || !t) { BoxLog("[OVL] fallback tex FAILED"); return false; }
			a_dev->CreateShaderResourceView(t, nullptr, &s_state.srv);
			t->Release();
			BoxLog("[OVL] reticle DDS FAILED -> GREEN fallback");
		}

		s_state.ok = true;
		BoxLog("[OVL] init OK");
		return true;
	}

	static bool EnsureBackBuffer(IDXGISwapChain* a_sc, ID3D11Device* a_dev)
	{
		ID3D11Texture2D* tex = nullptr;
		if (FAILED(a_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) return false;
		if (tex == s_state.bbTex && s_state.bbRtv) { tex->Release(); return true; }

		if (s_state.bbRtv) { s_state.bbRtv->Release(); s_state.bbRtv = nullptr; }
		if (s_state.bbTex) { s_state.bbTex->Release(); s_state.bbTex = nullptr; }

		if (FAILED(a_dev->CreateRenderTargetView(tex, nullptr, &s_state.bbRtv))) {
			tex->Release();
			return false;
		}
		s_state.bbTex = tex;
		return true;
	}

	void OverlayDraw(IDXGISwapChain* a_sc)
	{
		// 只在 ADS 时画。遮罩参数现在用屏幕中心 + 固定半径（见下面的 CbData），
		// 不再依赖 g_maskR —— 镜片投影还没校准好，依赖它会整块裁掉准星。
		if (!g_ads.load()) return;

		ID3D11Device* dev = nullptr;
		ID3D11DeviceContext* ctx = nullptr;
		if (FAILED(a_sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;
		dev->GetImmediateContext(&ctx);
		if (!ctx) { dev->Release(); return; }

		do {
			if (!InitState(dev)) break;
			if (!EnsureBackBuffer(a_sc, dev)) break;

			D3D11_TEXTURE2D_DESC td{};
			s_state.bbTex->GetDesc(&td);

			// ---- 备份 ----
			ID3D11RenderTargetView* oldRtv = nullptr;
			ID3D11DepthStencilView* oldDsv = nullptr;
			ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);

			UINT nVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			D3D11_VIEWPORT oldVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			ctx->RSGetViewports(&nVp, oldVp);

			ID3D11VertexShader* oldVs = nullptr;
			ID3D11PixelShader* oldPs = nullptr;
			ID3D11InputLayout* oldIl = nullptr;
			ID3D11Buffer* oldVb = nullptr;
			ID3D11Buffer* oldCb[1]{};
			ID3D11Buffer* oldPsCb[1]{};
			ID3D11ShaderResourceView* oldSrv[2]{};
			ID3D11SamplerState* oldSamp[1]{};
			UINT oldStride = 0, oldOffset = 0;
			D3D11_PRIMITIVE_TOPOLOGY oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
			ID3D11RasterizerState* oldRs = nullptr;
			ID3D11BlendState* oldBs = nullptr;
			FLOAT oldBf[4]{};
			UINT oldMask = 0;
			ID3D11DepthStencilState* oldDss = nullptr;
			UINT oldStencil = 0;

			ctx->VSGetShader(&oldVs, nullptr, nullptr);
			ctx->PSGetShader(&oldPs, nullptr, nullptr);
			ctx->VSGetConstantBuffers(0, 1, oldCb);
			ctx->PSGetConstantBuffers(0, 1, oldPsCb);
			ctx->PSGetShaderResources(0, 2, oldSrv);
			ctx->PSGetSamplers(0, 1, oldSamp);
			ctx->IAGetInputLayout(&oldIl);
			ctx->IAGetVertexBuffers(0, 1, &oldVb, &oldStride, &oldOffset);
			ctx->IAGetPrimitiveTopology(&oldTopo);
			ctx->RSGetState(&oldRs);
			ctx->OMGetBlendState(&oldBs, oldBf, &oldMask);
			ctx->OMGetDepthStencilState(&oldDss, &oldStencil);

			// ---- 设置 ----
			ID3D11RenderTargetView* rtvs[1] = { s_state.bbRtv };
			ctx->OMSetRenderTargets(1, rtvs, nullptr);

			D3D11_VIEWPORT vp{};
			vp.Width = (FLOAT)td.Width;
			vp.Height = (FLOAT)td.Height;
			vp.MaxDepth = 1.0f;
			ctx->RSSetViewports(1, &vp);
			ctx->RSSetState(s_state.rs);
			FLOAT bf[4]{};
			ctx->OMSetBlendState(s_state.blend, bf, 0xFFFFFFFF);
			ctx->OMSetDepthStencilState(s_state.ds, 0);

			ctx->IASetInputLayout(nullptr);
			ID3D11Buffer* noVb[1] = { nullptr };
			UINT zero = 0;
			ctx->IASetVertexBuffers(0, 1, noVb, &zero, &zero);
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

			CbData cb{};
			cb.screenW = (float)td.Width;
			cb.screenH = (float)td.Height;

			// 全部参数来自 ImGui 菜单（F7 打开），玩家自己调到位
			cb.centerX = (float)td.Width * 0.5f + g_reticleOffX;
			cb.centerY = (float)td.Height * 0.5f + g_reticleOffY;
			// 准星大小只由 g_reticleSize * g_reticleScale 决定，与遮罩半径解耦
			const float reticleSide = g_reticleSize * g_reticleScale;
			cb.boxW = reticleSide;
			cb.boxH = reticleSide;
			cb.alphaMul = 1.0f;
			// 遮罩：自动模式用镜片方向算出的值（可再乘一个缩放），
			// 否则用菜单里的固定值
			const float maskR = g_maskAuto ? (g_maskR * g_maskRadiusScale)
			                               : ((g_maskRadius > 5.0f) ? g_maskRadius : ((float)td.Height * 0.15f));
			cb.maskR = maskR;
			cb.maskCX = (float)td.Width * 0.5f + (g_maskAuto ? g_maskCX : g_maskOffX);
			cb.maskCY = (float)td.Height * 0.5f + (g_maskAuto ? g_maskCY : g_maskOffY);

			D3D11_MAPPED_SUBRESOURCE ms{};
			if (SUCCEEDED(ctx->Map(s_state.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
				std::memcpy(ms.pData, &cb, sizeof(cb));
				ctx->Unmap(s_state.cb, 0);
			}

			ID3D11Buffer* cbs[1] = { s_state.cb };
			ctx->VSSetConstantBuffers(0, 1, cbs);
			ctx->PSSetConstantBuffers(0, 1, cbs);
			ctx->VSSetShader(s_state.vs, nullptr, 0);
			ctx->PSSetShader(s_state.ps, nullptr, 0);
			ctx->PSSetShaderResources(0, 1, &s_state.srv);
			ctx->PSSetSamplers(0, 1, &s_state.samp);

			ctx->Draw(4, 0);

			if (s_state.drawLogs < 5) {
				s_state.drawLogs++;
				BoxLog("[DRAW] bb=%ux%u mask=(%.0f,%.0f,r=%.0f) tex=%s",
					td.Width, td.Height, cb.maskCX, cb.maskCY, cb.maskR,
					s_state.texFromFile ? "DDS" : "GREEN");
			}

			// ---- 恢复 ----
			ctx->VSSetShader(oldVs, nullptr, 0);
			ctx->PSSetShader(oldPs, nullptr, 0);
			ctx->VSSetConstantBuffers(0, 1, oldCb);
			ctx->PSSetConstantBuffers(0, 1, oldPsCb);
			ID3D11ShaderResourceView* nullSrv[2] = { nullptr, nullptr };
			ctx->PSSetShaderResources(0, 2, (oldSrv[0] || oldSrv[1]) ? oldSrv : nullSrv);
			if (oldSamp[0]) ctx->PSSetSamplers(0, 1, oldSamp);
			ctx->IASetInputLayout(oldIl);
			ctx->IASetVertexBuffers(0, 1, &oldVb, &oldStride, &oldOffset);
			ctx->IASetPrimitiveTopology(oldTopo);
			ctx->RSSetState(oldRs);
			ctx->OMSetBlendState(oldBs, oldBf, oldMask);
			ctx->OMSetDepthStencilState(oldDss, oldStencil);
			ctx->RSSetViewports(nVp, oldVp);
			ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);

			if (oldVs) oldVs->Release();
			if (oldPs) oldPs->Release();
			if (oldCb[0]) oldCb[0]->Release();
			if (oldPsCb[0]) oldPsCb[0]->Release();
			if (oldSrv[0]) oldSrv[0]->Release();
			if (oldSrv[1]) oldSrv[1]->Release();
			if (oldSamp[0]) oldSamp[0]->Release();
			if (oldIl) oldIl->Release();
			if (oldVb) oldVb->Release();
			if (oldRs) oldRs->Release();
			if (oldBs) oldBs->Release();
			if (oldDss) oldDss->Release();
			if (oldRtv) oldRtv->Release();
			if (oldDsv) oldDsv->Release();
		} while (false);

		ctx->Release();
		dev->Release();
	}

	void OverlayShutdown()
	{
		if (s_state.srv) s_state.srv->Release();
		if (s_state.samp) s_state.samp->Release();
		if (s_state.rs) s_state.rs->Release();
		if (s_state.ds) s_state.ds->Release();
		if (s_state.blend) s_state.blend->Release();
		if (s_state.cb) s_state.cb->Release();
		if (s_state.ps) s_state.ps->Release();
		if (s_state.vs) s_state.vs->Release();
		if (s_state.bbRtv) s_state.bbRtv->Release();
		if (s_state.bbTex) s_state.bbTex->Release();
		s_state = State{};
	}
}
