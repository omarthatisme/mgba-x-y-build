/* Direct3D renderers for the mGBA Qt frontend.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "D3DRenderer.h"

#ifdef BUILD_D3D

#include <d3d11.h>
#include <d3d9.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// All Direct3D / DXGI / compiler entry points are resolved at run time, so the
// executable has no import dependency on them and still starts on systems
// where one of them is missing (the display driver then reports "unsupported").

namespace QGBA {
namespace {

template<typename T>
class ComPtr {
public:
	ComPtr() = default;
	~ComPtr() { reset(); }
	ComPtr(const ComPtr&) = delete;
	ComPtr& operator=(const ComPtr&) = delete;

	T* get() const { return m_ptr; }
	T* operator->() const { return m_ptr; }
	explicit operator bool() const { return m_ptr != nullptr; }

	// Releases the current object and returns the address to receive a new one.
	T** put() {
		reset();
		return &m_ptr;
	}

	void** putVoid() { return reinterpret_cast<void**>(put()); }

	void reset() {
		if (m_ptr) {
			m_ptr->Release();
			m_ptr = nullptr;
		}
	}

	template<typename U>
	HRESULT as(ComPtr<U>& out) const {
		return m_ptr->QueryInterface(__uuidof(U), out.putVoid());
	}

private:
	T* m_ptr = nullptr;
};

template<typename Fn>
Fn loadProc(HMODULE module, const char* name) {
	return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// Only ever load these from System32 so a DLL dropped next to a portable
// mGBA.exe can't hijack the renderer.
HMODULE loadSystemLibrary(const wchar_t* name) {
	wchar_t path[MAX_PATH];
	UINT len = GetSystemDirectoryW(path, MAX_PATH);
	if (!len || len + wcslen(name) + 2 >= MAX_PATH) {
		return nullptr;
	}
	wcscat(path, L"\\");
	wcscat(path, name);
	return LoadLibraryW(path);
}

std::string describe(const char* what, HRESULT hr) {
	char buffer[160];
	snprintf(buffer, sizeof(buffer), "%s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(hr));
	return buffer;
}

// Pixel conversion helpers for the D3D9 path. Frames arrive as bytes
// R,G,B,X and D3DFMT_A8R8G8B8 wants B,G,R,A in memory.
inline uint32_t frameToArgb(uint32_t p) {
	return 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
}

inline uint32_t overlayToArgb(uint32_t p) {
	return (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
}

// ---------------------------------------------------------------------------
// Direct3D 11
// ---------------------------------------------------------------------------

const char* const kShaderSource = R"HLSL(
cbuffer Params : register(b0) {
	float4 rect;   // left, top, right, bottom in clip space
	float4 params; // x = opacity
};

struct VSOut {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
	float2 c = float2((float) (id & 1u), (float) (id >> 1u));
	VSOut o;
	o.pos = float4(lerp(rect.x, rect.z, c.x), lerp(rect.y, rect.w, c.y), 0.0, 1.0);
	o.uv = c;
	return o;
}

Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);

float4 PSFrame(VSOut i) : SV_Target {
	return float4(tex0.Sample(smp0, i.uv).rgb, params.x);
}

float4 PSOverlay(VSOut i) : SV_Target {
	return tex0.Sample(smp0, i.uv);
}
)HLSL";

typedef HRESULT(WINAPI* D3D11CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                             ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob**, ID3DBlob**);

// Not all MinGW headers know these (they are Windows 8/10 additions).
const DXGI_SWAP_EFFECT kFlipSequential = static_cast<DXGI_SWAP_EFFECT>(3);
const DXGI_SWAP_EFFECT kFlipDiscard = static_cast<DXGI_SWAP_EFFECT>(4);
const UINT kWaitableFlag = 0x40; // DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT

class D3D11Renderer final : public D3DRenderer {
public:
	~D3D11Renderer() override { shutdown(); }

	const char* name() const override { return "Direct3D 11"; }

	bool init(HWND hwnd, unsigned width, unsigned height) override {
		shutdown();
		m_error.clear();
		if (!loadLibraries() || !createDevice() || !createShaders() || !createStates()) {
			shutdown();
			return false;
		}
		if (!createSwapChain(hwnd, std::max(1u, width), std::max(1u, height)) || !createTarget()) {
			shutdown();
			return false;
		}
		m_hwnd = hwnd;
		m_width = std::max(1u, width);
		m_height = std::max(1u, height);
		return true;
	}

	void shutdown() override {
		if (m_ctx) {
			m_ctx->ClearState();
			m_ctx->Flush();
		}
		for (Texture& t : m_frame) {
			t.reset();
		}
		m_overlay.reset();
		m_rtv.reset();
		m_vs.reset();
		m_psFrame.reset();
		m_psOverlay.reset();
		m_cb.reset();
		m_point.reset();
		m_linear.reset();
		m_blendAlpha.reset();
		m_blendPremul.reset();
		m_raster.reset();
		m_sc.reset();
		if (m_waitable) {
			CloseHandle(m_waitable);
			m_waitable = nullptr;
		}
		m_ctx.reset();
		m_dev.reset();
		m_hwnd = nullptr;
		m_ticket = false;
		m_hasFrame = false;
		m_hasHistory = false;
		m_hasOverlay = false;
		m_cur = 0;
	}

	bool resize(unsigned width, unsigned height) override {
		if (!m_sc) {
			return false;
		}
		width = std::max(1u, width);
		height = std::max(1u, height);
		m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
		m_rtv.reset();
		m_ctx->Flush();
		HRESULT hr = m_sc->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, m_scFlags);
		if (FAILED(hr)) {
			m_error = describe("IDXGISwapChain::ResizeBuffers", hr);
			return false;
		}
		m_width = width;
		m_height = height;
		m_ticket = false;
		m_hasFrame = false;
		m_hasHistory = false;
		m_hasOverlay = false;
		if (!createTarget()) {
			return false;
		}
		limitLatency();
		return true;
	}

	bool uploadFrame(const uint32_t* pixels, unsigned width, unsigned height, unsigned pitch) override {
		if (!m_dev || !width || !height) {
			return false;
		}
		if (m_frame[0].w != width || m_frame[0].h != height) {
			if (!createTexture(m_frame[0], width, height) || !createTexture(m_frame[1], width, height)) {
				return false;
			}
			m_hasFrame = false;
			m_hasHistory = false;
			m_cur = 0;
		}
		int next = m_cur ^ 1;
		if (!fill(m_frame[next], pixels, width, height, pitch)) {
			return false;
		}
		m_cur = next;
		m_hasHistory = m_hasFrame;
		m_hasFrame = true;
		return true;
	}

	bool uploadOverlay(const uint32_t* pixels, unsigned width, unsigned height, unsigned pitch) override {
		if (!m_dev || !width || !height) {
			return false;
		}
		if (m_overlay.w != width || m_overlay.h != height) {
			if (!createTexture(m_overlay, width, height)) {
				m_hasOverlay = false;
				return false;
			}
		}
		m_hasOverlay = fill(m_overlay, pixels, width, height, pitch);
		return m_hasOverlay;
	}

	void clearOverlay() override { m_hasOverlay = false; }

	void waitForPresent(unsigned timeoutMs) override {
		// The latency object hands out one ticket per presented frame. Only
		// take one if we are not already holding one, so a wait that is not
		// followed by a present (hidden window, nothing to draw) can't leak
		// tickets and stall the next frame.
		if (m_waitable && !m_ticket) {
			if (WaitForSingleObjectEx(m_waitable, timeoutMs, TRUE) == WAIT_OBJECT_0) {
				m_ticket = true;
			}
		}
	}

	bool draw(const Rect& dest, bool filter, bool blend) override {
		if (!m_rtv) {
			return false;
		}
		ID3D11RenderTargetView* targets[] = { m_rtv.get() };
		m_ctx->OMSetRenderTargets(1, targets, nullptr);
		D3D11_VIEWPORT vp = {};
		vp.Width = static_cast<FLOAT>(m_width);
		vp.Height = static_cast<FLOAT>(m_height);
		vp.MaxDepth = 1.0f;
		m_ctx->RSSetViewports(1, &vp);
		const FLOAT black[4] = { 0.f, 0.f, 0.f, 1.f };
		m_ctx->ClearRenderTargetView(m_rtv.get(), black);

		m_ctx->IASetInputLayout(nullptr);
		m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		m_ctx->VSSetShader(m_vs.get(), nullptr, 0);
		ID3D11Buffer* buffers[] = { m_cb.get() };
		m_ctx->VSSetConstantBuffers(0, 1, buffers);
		m_ctx->PSSetConstantBuffers(0, 1, buffers);
		m_ctx->RSSetState(m_raster.get());
		const FLOAT factor[4] = { 0.f, 0.f, 0.f, 0.f };

		if (m_hasFrame && dest.w > 0 && dest.h > 0) {
			ID3D11SamplerState* sampler = filter ? m_linear.get() : m_point.get();
			m_ctx->PSSetSamplers(0, 1, &sampler);
			m_ctx->PSSetShader(m_psFrame.get(), nullptr, 0);
			setParams(dest, 1.0f);
			if (blend && m_hasHistory) {
				m_ctx->OMSetBlendState(nullptr, factor, 0xFFFFFFFF);
				drawQuad(m_frame[m_cur ^ 1].srv.get());
				setParams(dest, 0.5f);
				m_ctx->OMSetBlendState(m_blendAlpha.get(), factor, 0xFFFFFFFF);
				drawQuad(m_frame[m_cur].srv.get());
			} else {
				m_ctx->OMSetBlendState(nullptr, factor, 0xFFFFFFFF);
				drawQuad(m_frame[m_cur].srv.get());
			}
		}

		if (m_hasOverlay) {
			ID3D11SamplerState* sampler = m_point.get();
			m_ctx->PSSetSamplers(0, 1, &sampler);
			m_ctx->PSSetShader(m_psOverlay.get(), nullptr, 0);
			setParams({ 0, 0, static_cast<int>(m_width), static_cast<int>(m_height) }, 1.0f);
			m_ctx->OMSetBlendState(m_blendPremul.get(), factor, 0xFFFFFFFF);
			drawQuad(m_overlay.srv.get());
		}

		ID3D11ShaderResourceView* none[] = { nullptr };
		m_ctx->PSSetShaderResources(0, 1, none);
		return true;
	}

	bool present(bool vsync) override {
		if (!m_sc) {
			return false;
		}
		HRESULT hr = m_sc->Present(vsync ? 1 : 0, 0);
		m_ticket = false;
		if (FAILED(hr)) {
			m_error = describe("IDXGISwapChain::Present", hr);
			return false;
		}
		return true;
	}

	bool capture(std::vector<uint32_t>& rgba, unsigned& width, unsigned& height) override {
		if (!m_sc) {
			return false;
		}
		ComPtr<ID3D11Texture2D> back;
		if (FAILED(m_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), back.putVoid()))) {
			return false;
		}
		D3D11_TEXTURE2D_DESC desc;
		back->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> staging;
		if (FAILED(m_dev->CreateTexture2D(&desc, nullptr, staging.put()))) {
			return false;
		}
		m_ctx->CopyResource(staging.get(), back.get());
		D3D11_MAPPED_SUBRESOURCE ms;
		if (FAILED(m_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &ms))) {
			return false;
		}
		width = desc.Width;
		height = desc.Height;
		rgba.resize(static_cast<size_t>(width) * height);
		for (unsigned y = 0; y < height; ++y) {
			memcpy(&rgba[static_cast<size_t>(y) * width], static_cast<const uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch,
			       static_cast<size_t>(width) * 4);
		}
		m_ctx->Unmap(staging.get(), 0);
		return true;
	}

private:
	struct Texture {
		ComPtr<ID3D11Texture2D> tex;
		ComPtr<ID3D11ShaderResourceView> srv;
		unsigned w = 0;
		unsigned h = 0;

		void reset() {
			srv.reset();
			tex.reset();
			w = 0;
			h = 0;
		}
	};

	bool fail(const char* what, HRESULT hr) {
		m_error = describe(what, hr);
		return false;
	}

	bool loadLibraries() {
		if (!m_d3d11) {
			m_d3d11 = loadSystemLibrary(L"d3d11.dll");
		}
		if (!m_compiler) {
			m_compiler = loadSystemLibrary(L"d3dcompiler_47.dll");
		}
		if (!m_d3d11) {
			m_error = "d3d11.dll is not available";
			return false;
		}
		if (!m_compiler) {
			m_error = "d3dcompiler_47.dll is not available";
			return false;
		}
		m_createDevice = loadProc<D3D11CreateDeviceFn>(m_d3d11, "D3D11CreateDevice");
		m_compile = loadProc<D3DCompileFn>(m_compiler, "D3DCompile");
		if (!m_createDevice || !m_compile) {
			m_error = "Direct3D 11 entry points are missing";
			return false;
		}
		return true;
	}

	bool createDevice() {
		D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
		D3D_FEATURE_LEVEL got;
		HRESULT hr = m_createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 4, D3D11_SDK_VERSION, m_dev.put(), &got,
		                            m_ctx.put());
		if (hr == E_INVALIDARG) {
			// Runtimes without feature level 11.1 reject the whole list
			hr = m_createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels + 1, 3, D3D11_SDK_VERSION, m_dev.put(), &got,
			                    m_ctx.put());
		}
		if (FAILED(hr)) {
			return fail("D3D11CreateDevice", hr);
		}
		return true;
	}

	bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
		ComPtr<ID3DBlob> errors;
		HRESULT hr = m_compile(kShaderSource, strlen(kShaderSource), "mgba-d3d11.hlsl", nullptr, nullptr, entry, target, 1 << 15, 0,
		                       blob.put(), errors.put());
		if (FAILED(hr)) {
			m_error = describe("D3DCompile", hr);
			if (errors && errors->GetBufferSize()) {
				m_error += ": ";
				m_error.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
			}
			return false;
		}
		return true;
	}

	bool createShaders() {
		ComPtr<ID3DBlob> vs, psFrame, psOverlay;
		if (!compile("VSMain", "vs_4_0", vs) || !compile("PSFrame", "ps_4_0", psFrame) || !compile("PSOverlay", "ps_4_0", psOverlay)) {
			return false;
		}
		HRESULT hr = m_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, m_vs.put());
		if (FAILED(hr)) {
			return fail("CreateVertexShader", hr);
		}
		hr = m_dev->CreatePixelShader(psFrame->GetBufferPointer(), psFrame->GetBufferSize(), nullptr, m_psFrame.put());
		if (FAILED(hr)) {
			return fail("CreatePixelShader", hr);
		}
		hr = m_dev->CreatePixelShader(psOverlay->GetBufferPointer(), psOverlay->GetBufferSize(), nullptr, m_psOverlay.put());
		if (FAILED(hr)) {
			return fail("CreatePixelShader", hr);
		}
		return true;
	}

	bool createStates() {
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(float) * 8;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT hr = m_dev->CreateBuffer(&bd, nullptr, m_cb.put());
		if (FAILED(hr)) {
			return fail("CreateBuffer", hr);
		}

		D3D11_SAMPLER_DESC sd = {};
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		hr = m_dev->CreateSamplerState(&sd, m_point.put());
		if (FAILED(hr)) {
			return fail("CreateSamplerState", hr);
		}
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		hr = m_dev->CreateSamplerState(&sd, m_linear.put());
		if (FAILED(hr)) {
			return fail("CreateSamplerState", hr);
		}

		D3D11_BLEND_DESC blend = {};
		blend.RenderTarget[0].BlendEnable = TRUE;
		blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		hr = m_dev->CreateBlendState(&blend, m_blendAlpha.put());
		if (FAILED(hr)) {
			return fail("CreateBlendState", hr);
		}
		blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		hr = m_dev->CreateBlendState(&blend, m_blendPremul.put());
		if (FAILED(hr)) {
			return fail("CreateBlendState", hr);
		}

		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		hr = m_dev->CreateRasterizerState(&rd, m_raster.put());
		if (FAILED(hr)) {
			return fail("CreateRasterizerState", hr);
		}
		return true;
	}

	bool createSwapChain(HWND hwnd, unsigned width, unsigned height) {
		ComPtr<IDXGIDevice> dxgiDevice;
		ComPtr<IDXGIAdapter> adapter;
		ComPtr<IDXGIFactory2> factory;
		HRESULT hr = m_dev.as(dxgiDevice);
		if (FAILED(hr)) {
			return fail("QueryInterface(IDXGIDevice)", hr);
		}
		hr = dxgiDevice->GetAdapter(adapter.put());
		if (FAILED(hr)) {
			return fail("IDXGIDevice::GetAdapter", hr);
		}
		hr = adapter->GetParent(__uuidof(IDXGIFactory2), factory.putVoid());
		if (FAILED(hr)) {
			return fail("IDXGIAdapter::GetParent(IDXGIFactory2)", hr);
		}

		struct Attempt {
			DXGI_SWAP_EFFECT effect;
			UINT buffers;
			UINT flags;
		};
		// Flip model is what gets the frame to the screen with the least delay
		// (no extra copy by the window manager). Older systems fall back step
		// by step to the legacy blit model.
		static const Attempt attempts[] = {
			{ kFlipDiscard, 2, kWaitableFlag },
			{ kFlipSequential, 2, kWaitableFlag },
			{ kFlipDiscard, 2, 0 },
			{ DXGI_SWAP_EFFECT_DISCARD, 1, 0 },
		};

		hr = E_FAIL;
		const Attempt* used = nullptr;
		for (const Attempt& a : attempts) {
			DXGI_SWAP_CHAIN_DESC1 desc = {};
			desc.Width = width;
			desc.Height = height;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = a.buffers;
			desc.Scaling = DXGI_SCALING_STRETCH;
			desc.SwapEffect = a.effect;
			desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
			desc.Flags = a.flags;
			hr = factory->CreateSwapChainForHwnd(m_dev.get(), hwnd, &desc, nullptr, nullptr, m_sc.put());
			if (SUCCEEDED(hr)) {
				used = &a;
				break;
			}
		}
		if (!used) {
			return fail("IDXGIFactory2::CreateSwapChainForHwnd", hr);
		}
		m_scFlags = used->flags;
		factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
		limitLatency();
		return true;
	}

	// Keeps at most one frame queued between us and the screen.
	void limitLatency() {
		if (m_scFlags & kWaitableFlag) {
			ComPtr<IDXGISwapChain2> sc2;
			if (SUCCEEDED(m_sc.as(sc2))) {
				sc2->SetMaximumFrameLatency(1);
				if (!m_waitable) {
					m_waitable = sc2->GetFrameLatencyWaitableObject();
				}
			}
		} else {
			ComPtr<IDXGIDevice1> dxgiDevice;
			if (SUCCEEDED(m_dev.as(dxgiDevice))) {
				dxgiDevice->SetMaximumFrameLatency(1);
			}
		}
	}

	bool createTarget() {
		ComPtr<ID3D11Texture2D> back;
		HRESULT hr = m_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), back.putVoid());
		if (FAILED(hr)) {
			return fail("IDXGISwapChain::GetBuffer", hr);
		}
		hr = m_dev->CreateRenderTargetView(back.get(), nullptr, m_rtv.put());
		if (FAILED(hr)) {
			return fail("CreateRenderTargetView", hr);
		}
		return true;
	}

	bool createTexture(Texture& t, unsigned width, unsigned height) {
		t.reset();
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT hr = m_dev->CreateTexture2D(&desc, nullptr, t.tex.put());
		if (FAILED(hr)) {
			return fail("CreateTexture2D", hr);
		}
		hr = m_dev->CreateShaderResourceView(t.tex.get(), nullptr, t.srv.put());
		if (FAILED(hr)) {
			t.reset();
			return fail("CreateShaderResourceView", hr);
		}
		t.w = width;
		t.h = height;
		return true;
	}

	bool fill(Texture& t, const uint32_t* pixels, unsigned width, unsigned height, unsigned pitch) {
		D3D11_MAPPED_SUBRESOURCE ms;
		HRESULT hr = m_ctx->Map(t.tex.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
		if (FAILED(hr)) {
			return fail("ID3D11DeviceContext::Map", hr);
		}
		const uint8_t* src = reinterpret_cast<const uint8_t*>(pixels);
		uint8_t* dst = static_cast<uint8_t*>(ms.pData);
		for (unsigned y = 0; y < height; ++y) {
			memcpy(dst + static_cast<size_t>(y) * ms.RowPitch, src + static_cast<size_t>(y) * pitch, static_cast<size_t>(width) * 4);
		}
		m_ctx->Unmap(t.tex.get(), 0);
		return true;
	}

	void setParams(const Rect& r, float opacity) {
		D3D11_MAPPED_SUBRESOURCE ms;
		if (FAILED(m_ctx->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
			return;
		}
		float* p = static_cast<float*>(ms.pData);
		const float w = static_cast<float>(m_width);
		const float h = static_cast<float>(m_height);
		p[0] = r.x / w * 2.0f - 1.0f;
		p[1] = 1.0f - r.y / h * 2.0f;
		p[2] = (r.x + r.w) / w * 2.0f - 1.0f;
		p[3] = 1.0f - (r.y + r.h) / h * 2.0f;
		p[4] = opacity;
		p[5] = p[6] = p[7] = 0.0f;
		m_ctx->Unmap(m_cb.get(), 0);
	}

	void drawQuad(ID3D11ShaderResourceView* srv) {
		ID3D11ShaderResourceView* views[] = { srv };
		m_ctx->PSSetShaderResources(0, 1, views);
		m_ctx->Draw(4, 0);
	}

	HMODULE m_d3d11 = nullptr;
	HMODULE m_compiler = nullptr;
	D3D11CreateDeviceFn m_createDevice = nullptr;
	D3DCompileFn m_compile = nullptr;

	HWND m_hwnd = nullptr;
	unsigned m_width = 0;
	unsigned m_height = 0;
	UINT m_scFlags = 0;
	HANDLE m_waitable = nullptr;
	bool m_ticket = false;

	ComPtr<ID3D11Device> m_dev;
	ComPtr<ID3D11DeviceContext> m_ctx;
	ComPtr<IDXGISwapChain1> m_sc;
	ComPtr<ID3D11RenderTargetView> m_rtv;
	ComPtr<ID3D11VertexShader> m_vs;
	ComPtr<ID3D11PixelShader> m_psFrame;
	ComPtr<ID3D11PixelShader> m_psOverlay;
	ComPtr<ID3D11Buffer> m_cb;
	ComPtr<ID3D11SamplerState> m_point;
	ComPtr<ID3D11SamplerState> m_linear;
	ComPtr<ID3D11BlendState> m_blendAlpha;
	ComPtr<ID3D11BlendState> m_blendPremul;
	ComPtr<ID3D11RasterizerState> m_raster;

	Texture m_frame[2];
	Texture m_overlay;
	int m_cur = 0;
	bool m_hasFrame = false;
	bool m_hasHistory = false;
	bool m_hasOverlay = false;
};

// ---------------------------------------------------------------------------
// Direct3D 9Ex (fixed function, no shaders)
// ---------------------------------------------------------------------------

typedef HRESULT(WINAPI* Direct3DCreate9ExFn)(UINT, IDirect3D9Ex**);

const D3DSWAPEFFECT kFlipEx = static_cast<D3DSWAPEFFECT>(5);

// Some MinGW headers lack this Direct3D 9Ex present flag
#ifndef D3DPRESENT_FORCEIMMEDIATE
#define D3DPRESENT_FORCEIMMEDIATE 0x00000100L
#endif

class D3D9Renderer final : public D3DRenderer {
public:
	~D3D9Renderer() override { shutdown(); }

	const char* name() const override { return "Direct3D 9"; }

	bool init(HWND hwnd, unsigned width, unsigned height) override {
		shutdown();
		m_error.clear();
		if (!m_d3d9) {
			m_d3d9 = loadSystemLibrary(L"d3d9.dll");
		}
		if (!m_d3d9) {
			m_error = "d3d9.dll is not available";
			return false;
		}
		Direct3DCreate9ExFn create = loadProc<Direct3DCreate9ExFn>(m_d3d9, "Direct3DCreate9Ex");
		if (!create) {
			m_error = "Direct3DCreate9Ex is not available (requires Windows Vista or newer)";
			return false;
		}
		HRESULT hr = create(D3D_SDK_VERSION, m_d3d.put());
		if (FAILED(hr)) {
			return fail("Direct3DCreate9Ex", hr);
		}

		width = std::max(1u, width);
		height = std::max(1u, height);
		struct Attempt {
			D3DSWAPEFFECT effect;
			UINT buffers;
		};
		// FlipEx is the low-latency flip model for D3D9 (Windows 7+ with DWM)
		static const Attempt attempts[] = {
			{ kFlipEx, 2 },
			{ D3DSWAPEFFECT_DISCARD, 1 },
		};
		static const DWORD behaviors[] = { D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DCREATE_SOFTWARE_VERTEXPROCESSING };
		hr = E_FAIL;
		for (const Attempt& a : attempts) {
			for (DWORD behavior : behaviors) {
				D3DPRESENT_PARAMETERS pp = {};
				pp.BackBufferWidth = width;
				pp.BackBufferHeight = height;
				pp.BackBufferFormat = D3DFMT_X8R8G8B8;
				pp.BackBufferCount = a.buffers;
				pp.SwapEffect = a.effect;
				pp.hDeviceWindow = hwnd;
				pp.Windowed = TRUE;
				pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
				hr = m_d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
				                           behavior | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE | D3DCREATE_NOWINDOWCHANGES, &pp,
				                           nullptr, m_dev.put());
				if (SUCCEEDED(hr)) {
					m_pp = pp;
					break;
				}
			}
			if (SUCCEEDED(hr)) {
				break;
			}
		}
		if (FAILED(hr)) {
			shutdown();
			return fail("IDirect3D9Ex::CreateDeviceEx", hr);
		}
		m_dev->SetMaximumFrameLatency(1);
		m_hwnd = hwnd;
		return true;
	}

	void shutdown() override {
		for (Texture& t : m_frame) {
			t.reset();
		}
		m_overlay.reset();
		m_dev.reset();
		m_d3d.reset();
		m_hwnd = nullptr;
		m_hasFrame = false;
		m_hasHistory = false;
		m_hasOverlay = false;
		m_cur = 0;
	}

	bool resize(unsigned width, unsigned height) override {
		if (!m_dev) {
			return false;
		}
		for (Texture& t : m_frame) {
			t.reset();
		}
		m_overlay.reset();
		m_hasFrame = false;
		m_hasHistory = false;
		m_hasOverlay = false;
		m_pp.BackBufferWidth = std::max(1u, width);
		m_pp.BackBufferHeight = std::max(1u, height);
		HRESULT hr = m_dev->ResetEx(&m_pp, nullptr);
		if (FAILED(hr)) {
			return fail("IDirect3DDevice9Ex::ResetEx", hr);
		}
		m_dev->SetMaximumFrameLatency(1);
		return true;
	}

	bool uploadFrame(const uint32_t* pixels, unsigned width, unsigned height, unsigned pitch) override {
		if (!m_dev || !width || !height) {
			return false;
		}
		if (m_frame[0].w != width || m_frame[0].h != height) {
			if (!createTexture(m_frame[0], width, height) || !createTexture(m_frame[1], width, height)) {
				return false;
			}
			m_hasFrame = false;
			m_hasHistory = false;
			m_cur = 0;
		}
		int next = m_cur ^ 1;
		if (!fill(m_frame[next], pixels, width, height, pitch, frameToArgb)) {
			return false;
		}
		m_cur = next;
		m_hasHistory = m_hasFrame;
		m_hasFrame = true;
		return true;
	}

	bool uploadOverlay(const uint32_t* pixels, unsigned width, unsigned height, unsigned pitch) override {
		if (!m_dev || !width || !height) {
			return false;
		}
		if (m_overlay.w != width || m_overlay.h != height) {
			if (!createTexture(m_overlay, width, height)) {
				m_hasOverlay = false;
				return false;
			}
		}
		m_hasOverlay = fill(m_overlay, pixels, width, height, pitch, overlayToArgb);
		return m_hasOverlay;
	}

	void clearOverlay() override { m_hasOverlay = false; }

	void waitForPresent(unsigned) override {
		// PresentEx blocks on its own once SetMaximumFrameLatency(1) is reached
	}

	bool draw(const Rect& dest, bool filter, bool blend) override {
		if (!m_dev) {
			return false;
		}
		HRESULT hr = m_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
		if (FAILED(hr)) {
			return fail("IDirect3DDevice9Ex::Clear", hr);
		}
		hr = m_dev->BeginScene();
		if (FAILED(hr)) {
			return fail("IDirect3DDevice9Ex::BeginScene", hr);
		}

		m_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		m_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		m_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		m_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		m_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		m_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		m_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		m_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		m_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

		if (m_hasFrame && dest.w > 0 && dest.h > 0) {
			const DWORD mode = filter ? D3DTEXF_LINEAR : D3DTEXF_POINT;
			m_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, mode);
			m_dev->SetSamplerState(0, D3DSAMP_MINFILTER, mode);
			m_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			// Alpha comes from the vertex colour; the frame texture has none
			m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
			m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
			if (blend && m_hasHistory) {
				drawQuad(m_frame[m_cur ^ 1].tex.get(), dest, D3DCOLOR_ARGB(0xFF, 0xFF, 0xFF, 0xFF));
				m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
				m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
				drawQuad(m_frame[m_cur].tex.get(), dest, D3DCOLOR_ARGB(0x80, 0xFF, 0xFF, 0xFF));
				m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			} else {
				drawQuad(m_frame[m_cur].tex.get(), dest, D3DCOLOR_ARGB(0xFF, 0xFF, 0xFF, 0xFF));
			}
		}

		if (m_hasOverlay) {
			m_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			m_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
			m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
			m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
			m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
			m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
			Rect full = { 0, 0, static_cast<int>(m_pp.BackBufferWidth), static_cast<int>(m_pp.BackBufferHeight) };
			drawQuad(m_overlay.tex.get(), full, D3DCOLOR_ARGB(0xFF, 0xFF, 0xFF, 0xFF));
			m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		}

		m_dev->SetTexture(0, nullptr);
		m_dev->EndScene();
		return true;
	}

	bool present(bool vsync) override {
		if (!m_dev) {
			return false;
		}
		HRESULT hr = m_dev->PresentEx(nullptr, nullptr, nullptr, nullptr, vsync ? 0 : D3DPRESENT_FORCEIMMEDIATE);
		if (FAILED(hr) && !vsync) {
			hr = m_dev->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
		}
		if (FAILED(hr)) {
			m_error = describe("IDirect3DDevice9Ex::PresentEx", hr);
			return false;
		}
		// S_PRESENT_MODE_CHANGED: display mode changed under us, rebuild
		if (hr == static_cast<HRESULT>(0x08760877)) {
			m_error = "Display mode changed";
			return false;
		}
		return true;
	}

	bool capture(std::vector<uint32_t>& rgba, unsigned& width, unsigned& height) override {
		if (!m_dev) {
			return false;
		}
		ComPtr<IDirect3DSurface9> back;
		if (FAILED(m_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.put()))) {
			return false;
		}
		D3DSURFACE_DESC desc;
		back->GetDesc(&desc);
		ComPtr<IDirect3DSurface9> staging;
		if (FAILED(m_dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, staging.put(), nullptr))) {
			return false;
		}
		if (FAILED(m_dev->GetRenderTargetData(back.get(), staging.get()))) {
			return false;
		}
		D3DLOCKED_RECT lr;
		if (FAILED(staging->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
			return false;
		}
		width = desc.Width;
		height = desc.Height;
		rgba.resize(static_cast<size_t>(width) * height);
		for (unsigned y = 0; y < height; ++y) {
			const uint32_t* src = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch);
			uint32_t* dst = &rgba[static_cast<size_t>(y) * width];
			for (unsigned x = 0; x < width; ++x) {
				uint32_t v = src[x];
				dst[x] = 0xFF000000u | ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu);
			}
		}
		staging->UnlockRect();
		return true;
	}

private:
	struct Texture {
		ComPtr<IDirect3DTexture9> tex;
		unsigned w = 0;
		unsigned h = 0;

		void reset() {
			tex.reset();
			w = 0;
			h = 0;
		}
	};

	struct Vertex {
		float x, y, z, rhw;
		DWORD color;
		float u, v;
	};

	bool fail(const char* what, HRESULT hr) {
		m_error = describe(what, hr);
		return false;
	}

	bool createTexture(Texture& t, unsigned width, unsigned height) {
		t.reset();
		HRESULT hr = m_dev->CreateTexture(width, height, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, t.tex.put(), nullptr);
		if (FAILED(hr)) {
			return fail("IDirect3DDevice9Ex::CreateTexture", hr);
		}
		t.w = width;
		t.h = height;
		return true;
	}

	bool fill(Texture& t, const uint32_t* pixels, unsigned width, unsigned height, unsigned pitch, uint32_t (*convert)(uint32_t)) {
		D3DLOCKED_RECT lr;
		HRESULT hr = t.tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD);
		if (FAILED(hr)) {
			return fail("IDirect3DTexture9::LockRect", hr);
		}
		for (unsigned y = 0; y < height; ++y) {
			const uint32_t* src = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(pixels) + static_cast<size_t>(y) * pitch);
			uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch);
			for (unsigned x = 0; x < width; ++x) {
				dst[x] = convert(src[x]);
			}
		}
		t.tex->UnlockRect(0);
		return true;
	}

	void drawQuad(IDirect3DTexture9* tex, const Rect& r, DWORD color) {
		// -0.5 aligns texel centres with pixel centres for pre-transformed vertices
		const float l = static_cast<float>(r.x) - 0.5f;
		const float t = static_cast<float>(r.y) - 0.5f;
		const float rt = static_cast<float>(r.x + r.w) - 0.5f;
		const float b = static_cast<float>(r.y + r.h) - 0.5f;
		const Vertex v[4] = {
			{ l, t, 0.f, 1.f, color, 0.f, 0.f },
			{ rt, t, 0.f, 1.f, color, 1.f, 0.f },
			{ l, b, 0.f, 1.f, color, 0.f, 1.f },
			{ rt, b, 0.f, 1.f, color, 1.f, 1.f },
		};
		m_dev->SetTexture(0, tex);
		m_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
	}

	HMODULE m_d3d9 = nullptr;
	HWND m_hwnd = nullptr;
	D3DPRESENT_PARAMETERS m_pp = {};
	ComPtr<IDirect3D9Ex> m_d3d;
	ComPtr<IDirect3DDevice9Ex> m_dev;
	Texture m_frame[2];
	Texture m_overlay;
	int m_cur = 0;
	bool m_hasFrame = false;
	bool m_hasHistory = false;
	bool m_hasOverlay = false;
};

} // namespace

std::unique_ptr<D3DRenderer> D3DRenderer::create(Backend backend) {
	switch (backend) {
	case Backend::D3D11:
		return std::unique_ptr<D3DRenderer>(new D3D11Renderer);
	case Backend::D3D9:
		return std::unique_ptr<D3DRenderer>(new D3D9Renderer);
	}
	return nullptr;
}

bool D3DRenderer::probe(Backend backend, std::string* error) {
	static int results[2] = { -1, -1 };
	static std::string errors[2];
	int i = backend == Backend::D3D11 ? 0 : 1;
	if (results[i] < 0) {
		HWND hwnd = CreateWindowExW(0, L"STATIC", L"mGBA Direct3D probe", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
		                            GetModuleHandleW(nullptr), nullptr);
		if (!hwnd) {
			errors[i] = "could not create a probe window";
			results[i] = 0;
		} else {
			std::unique_ptr<D3DRenderer> renderer = create(backend);
			bool ok = renderer->init(hwnd, 64, 64);
			if (!ok) {
				errors[i] = renderer->lastError();
			}
			renderer->shutdown();
			renderer.reset();
			DestroyWindow(hwnd);
			results[i] = ok ? 1 : 0;
		}
	}
	if (error) {
		*error = errors[i];
	}
	return results[i] == 1;
}

}

#endif
