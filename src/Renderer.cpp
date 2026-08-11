#include "Renderer.h"
#include "UpscalerNV.h"

#include <d3dcompiler.h>
#include <d3d10.h>       // ID3D10Multithread
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ---- Shaders -----------------------------------------------------------------
static const char* kVS = R"(
struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.uv  = i.uv;
    return o;
}
)";

static const char* kPS = R"(
Texture2D    gTex  : register(t0);
SamplerState gSamp : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return gTex.Sample(gSamp, uv);
}
)";

// NV12: luma in plane 0 (R8), chroma interleaved in plane 1 (R8G8). The
// matrix comes from a constant buffer so BT.601 / BT.709 / BT.2020 and
// limited/full range all share one shader.
static const char* kPSNV12 = R"(
Texture2D<float>  gY    : register(t0);
Texture2D<float2> gUV   : register(t1);
SamplerState      gSamp : register(s0);
cbuffer YuvCB : register(b0) {
    float4 gOffset;   // subtracted from (Y, U, V)
    float4 gRowR;
    float4 gRowG;
    float4 gRowB;
};
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float  y  = gY.Sample(gSamp, uv);
    float2 cb = gUV.Sample(gSamp, uv);
    float3 t  = float3(y, cb.x, cb.y) - gOffset.xyz;
    float3 rgb = float3(dot(gRowR.xyz, t), dot(gRowG.xyz, t), dot(gRowB.xyz, t));
    return float4(saturate(rgb), 1.0);
}
)";

struct QuadVertex { float x, y, u, v; };

static const QuadVertex kQuadVerts[] = {
    { -1.0f, -1.0f, 0.0f, 1.0f },
    { -1.0f,  1.0f, 0.0f, 0.0f },
    {  1.0f, -1.0f, 1.0f, 1.0f },
    {  1.0f,  1.0f, 1.0f, 0.0f },
};

struct YuvConstants
{
    float offset[4];
    float rowR[4];
    float rowG[4];
    float rowB[4];
};

// ---- ctor/dtor ---------------------------------------------------------------
Renderer::Renderer()
{
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    qpcFreq_ = f.QuadPart;
}
Renderer::~Renderer() { Shutdown(); }

// ---- Init / Shutdown ---------------------------------------------------------
bool Renderer::Initialize(HWND hwnd, int width, int height)
{
    hwnd_ = hwnd;
    width_ = width;
    height_ = height;

    if (!CreateDeviceAndSwap(hwnd, width, height)) return false;
    if (!CreateBackbufferViews())                  return false;
    if (!CreatePipeline())                         return false;
    return true;
}

void Renderer::Shutdown()
{
    DestroyBackbufferViews();
    convSRV_.Reset();
    convRTV_.Reset();
    convTex_.Reset();
    videoSRVChroma_.Reset();
    videoSRV_.Reset();
    videoTex_.Reset();
    colorCB_.Reset();
    vb_.Reset();
    inputLayout_.Reset();
    psNV12_.Reset();
    ps_.Reset();
    vs_.Reset();
    sampler_.Reset();
    raster_.Reset();
    blendOpaque_.Reset();
    if (frameLatencyWaitable_) {
        // Waitable handle is owned by the swap chain; don't CloseHandle.
        frameLatencyWaitable_ = nullptr;
    }
    swap_.Reset();
    if (context_) context_->ClearState();
    context_.Reset();
    device_.Reset();
}

// ---- Device + SwapChain ------------------------------------------------------
bool Renderer::CreateDeviceAndSwap(HWND hwnd, int width, int height)
{
    // VIDEO_SUPPORT + BGRA_SUPPORT are required for MF's DXGI device manager
    // to accept this device (otherwise MFCreateSourceReaderFromMediaSource
    // fails with MF_E_UNSUPPORTED_D3D_TYPE).
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL usedFl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        fls, (UINT)std::size(fls),
        D3D11_SDK_VERSION,
        device_.GetAddressOf(), &usedFl, context_.GetAddressOf());

    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            fls, (UINT)std::size(fls),
            D3D11_SDK_VERSION,
            device_.GetAddressOf(), &usedFl, context_.GetAddressOf());
    }
    if (FAILED(hr)) return false;

    // Can this device sample NV12 directly? If not we have to make Media
    // Foundation convert to RGB32 for us. The capability bit alone isn't
    // enough — what we actually need is a per-plane view, so build a tiny
    // throwaway texture and try it. Getting this wrong at runtime would mean
    // a black window, and there's no cheap way to renegotiate mid-stream.
    {
        UINT support = 0;
        bool ok = SUCCEEDED(device_->CheckFormatSupport(DXGI_FORMAT_NV12, &support)) &&
                  (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) &&
                  (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
        if (ok) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = 16; td.Height = 16; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_NV12; td.SampleDesc = { 1, 0 };
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            CPtr<ID3D11Texture2D> probe;
            ok = SUCCEEDED(device_->CreateTexture2D(&td, nullptr, probe.GetAddressOf()));
            if (ok) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                CPtr<ID3D11ShaderResourceView> luma, chroma;
                sd.Format = DXGI_FORMAT_R8_UNORM;
                ok = SUCCEEDED(device_->CreateShaderResourceView(probe.Get(), &sd, luma.GetAddressOf()));
                sd.Format = DXGI_FORMAT_R8G8_UNORM;
                ok = ok && SUCCEEDED(device_->CreateShaderResourceView(probe.Get(), &sd, chroma.GetAddressOf()));
            }
        }
        nv12Supported_ = ok;
        wprintf(L"[dx] NV12 sampling %s\n", nv12Supported_ ? L"supported" : L"NOT supported");
    }

    // Required by Media Foundation's DXGI device manager — lets the capture
    // thread issue context calls (CopySubresourceRegion) while the render
    // thread is doing its work.
    CPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device_.As(&mt)))
        mt->SetMultithreadProtected(TRUE);

    CPtr<IDXGIDevice>   dxgiDev;
    CPtr<IDXGIAdapter>  adapter;
    CPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgiDev)))            return false;
    if (FAILED(dxgiDev->GetAdapter(&adapter)))   return false;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    // Variable-refresh / uncapped presentation needs DXGI 1.5 tearing
    // support. Query it up front so the UI can gray out the option.
    {
        CPtr<IDXGIFactory5> factory5;
        BOOL allow = FALSE;
        if (SUCCEEDED(factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
        {
            tearingSupported_ = (allow == TRUE);
        }
        wprintf(L"[dx] tearing %s\n", tearingSupported_ ? L"supported" : L"NOT supported");
    }

    // Waitable swap chain. We gate every frame on DWM's "I'm ready for the
    // next one" event via GetFrameLatencyWaitableObject, so frames land in
    // lockstep with composition regardless of how light the per-frame CPU
    // work happens to be. This is the documented cure for "frame rate
    // oscillates between 45-60" jitter on flip-model swap chains.
    //
    // Three buffers rather than two: at 120/144/240 Hz the extra buffer is
    // what keeps Present from blocking when one frame runs long, and the
    // waitable object still holds latency to a single frame.
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width       = width;
    scd.Height      = height;
    scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.Stereo      = FALSE;
    scd.SampleDesc  = { 1, 0 };
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 3;
    scd.Scaling     = DXGI_SCALING_STRETCH;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    scd.Flags       = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearingSupported_)
        scd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    CPtr<IDXGISwapChain1> swap1;
    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &scd, nullptr, nullptr, swap1.GetAddressOf());
    if (FAILED(hr)) {
        // Older systems without waitable/tearing support — fall back to a
        // plain flip swap chain.
        tearingSupported_ = false;
        scd.Flags = 0;
        hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &scd, nullptr, nullptr, swap1.GetAddressOf());
        if (FAILED(hr)) return false;
    }
    if (FAILED(swap1.As(&swap_))) return false;

    swapFlags_ = scd.Flags;
    if (swapFlags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        swap_->SetMaximumFrameLatency(1);
        frameLatencyWaitable_ = swap_->GetFrameLatencyWaitableObject();
    }

    // Disable automatic Alt+Enter handling (we manage fullscreen ourselves).
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool Renderer::CreateBackbufferViews()
{
    CPtr<ID3D11Texture2D> back;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    return SUCCEEDED(device_->CreateRenderTargetView(back.Get(), nullptr, rtv_.ReleaseAndGetAddressOf()));
}

void Renderer::DestroyBackbufferViews()
{
    if (context_) context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
}

void Renderer::Resize(int width, int height)
{
    if (!swap_ || width <= 0 || height <= 0) return;
    if (width == width_ && height == height_) return;

    width_  = width;
    height_ = height;

    DestroyBackbufferViews();
    HRESULT hr = swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapFlags_);
    if (SUCCEEDED(hr))
        CreateBackbufferViews();
}

int Renderer::MonitorRefreshHz() const
{
    if (!hwnd_) return 0;
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return (int)dm.dmDisplayFrequency;
}

void Renderer::SetPresentMode(PresentMode m)
{
    if (m == PresentMode::Tearing && !tearingSupported_) m = PresentMode::VSync;
    presentMode_ = m;
    nextFrameQpc_ = 0;
}

void Renderer::WaitForFrame()
{
    if (presentMode_ == PresentMode::VSync) {
        if (!frameLatencyWaitable_) return;
        // 200 ms bail-out — if DWM never signals we'd rather keep the UI
        // responsive than deadlock.
        WaitForSingleObjectEx(frameLatencyWaitable_, 200, TRUE);
        return;
    }

    // Tearing mode: DWM isn't pacing us, so honour the user's FPS cap
    // ourselves. Sleep for the bulk of the wait, then spin the last ~1 ms so
    // the cadence stays tight at 144/240 Hz.
    if (fpsLimit_ <= 0 || qpcFreq_ <= 0) return;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const uint64_t period = (uint64_t)(qpcFreq_ / fpsLimit_);
    if (nextFrameQpc_ == 0 || (uint64_t)now.QuadPart > nextFrameQpc_ + period * 4) {
        nextFrameQpc_ = (uint64_t)now.QuadPart + period;
        return;
    }
    while ((uint64_t)now.QuadPart < nextFrameQpc_) {
        const int64_t remainingUs = (int64_t)((nextFrameQpc_ - (uint64_t)now.QuadPart) * 1000000 / qpcFreq_);
        if (remainingUs > 1500) Sleep((DWORD)((remainingUs - 1000) / 1000));
        else                    YieldProcessor();
        QueryPerformanceCounter(&now);
    }
    nextFrameQpc_ += period;
}

void Renderer::SetFrameLatency(UINT frames)
{
    if (!swap_) return;
    if (frames < 1) frames = 1;
    if (frames > 16) frames = 16;
    // With a waitable swap chain the latency goes on the swap chain itself,
    // not on IDXGIDevice1. Fall back to the old device-level setter if
    // waitable isn't in use.
    if (swapFlags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        swap_->SetMaximumFrameLatency(frames);
    } else if (device_) {
        CPtr<IDXGIDevice1> dxgiDev1;
        if (SUCCEEDED(device_.As(&dxgiDev1)))
            dxgiDev1->SetMaximumFrameLatency(frames);
    }
}

// ---- Pipeline ----------------------------------------------------------------
static HRESULT CompileShader(const char* src, const char* entry, const char* target, ID3DBlob** out)
{
    CPtr<ID3DBlob> err;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                             entry, target, flags, 0, out, err.GetAddressOf());
    if (FAILED(hr) && err) {
        OutputDebugStringA((const char*)err->GetBufferPointer());
    }
    return hr;
}

bool Renderer::CreatePipeline()
{
    CPtr<ID3DBlob> vsBlob, psBlob, psNvBlob;
    if (FAILED(CompileShader(kVS, "main", "vs_4_0", vsBlob.GetAddressOf()))) return false;
    if (FAILED(CompileShader(kPS, "main", "ps_4_0", psBlob.GetAddressOf()))) return false;
    if (FAILED(CompileShader(kPSNV12, "main", "ps_4_0", psNvBlob.GetAddressOf()))) return false;

    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vs_.GetAddressOf()))) return false;
    if (FAILED(device_->CreatePixelShader (psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ps_.GetAddressOf()))) return false;
    if (FAILED(device_->CreatePixelShader (psNvBlob->GetBufferPointer(), psNvBlob->GetBufferSize(), nullptr, psNV12_.GetAddressOf()))) return false;

    const D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(device_->CreateInputLayout(ied, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), inputLayout_.GetAddressOf())))
        return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(kQuadVerts);
    bd.Usage     = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA srd{};
    srd.pSysMem = kQuadVerts;
    if (FAILED(device_->CreateBuffer(&bd, &srd, vb_.GetAddressOf()))) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = sizeof(YuvConstants);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&cbd, nullptr, colorCB_.GetAddressOf()))) return false;
    colorDirty_ = true;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter        = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD        = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, sampler_.GetAddressOf()))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rd, raster_.GetAddressOf()))) return false;

    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bld, blendOpaque_.GetAddressOf()))) return false;

    return true;
}

// ---- Color space -------------------------------------------------------------
void Renderer::SetVideoColorSpace(int matrix, bool fullRange)
{
    std::lock_guard<std::mutex> lk(videoTexMutex_);
    if (matrix < 0 || matrix > 2) matrix = 1;
    if (colorMatrix_ == matrix && colorFullRange_ == fullRange) return;
    colorMatrix_    = matrix;
    colorFullRange_ = fullRange;
    colorDirty_     = true;
}

void Renderer::UploadColorConstants()
{
    if (!colorDirty_ || !colorCB_ || !context_) return;

    // Rows are the standard inverse YCbCr matrices. Limited range also
    // rescales 16..235 / 16..240 up to 0..1.
    YuvConstants c{};
    if (colorFullRange_) {
        c.offset[0] = 0.0f; c.offset[1] = 0.5f; c.offset[2] = 0.5f;
        switch (colorMatrix_) {
            case 0: // BT.601 full
                c.rowR[0] = 1.0f; c.rowR[1] =  0.0f;      c.rowR[2] =  1.402000f;
                c.rowG[0] = 1.0f; c.rowG[1] = -0.344136f; c.rowG[2] = -0.714136f;
                c.rowB[0] = 1.0f; c.rowB[1] =  1.772000f; c.rowB[2] =  0.0f;
                break;
            case 2: // BT.2020 full
                c.rowR[0] = 1.0f; c.rowR[1] =  0.0f;      c.rowR[2] =  1.474600f;
                c.rowG[0] = 1.0f; c.rowG[1] = -0.164550f; c.rowG[2] = -0.571350f;
                c.rowB[0] = 1.0f; c.rowB[1] =  1.881400f; c.rowB[2] =  0.0f;
                break;
            default: // BT.709 full
                c.rowR[0] = 1.0f; c.rowR[1] =  0.0f;      c.rowR[2] =  1.574800f;
                c.rowG[0] = 1.0f; c.rowG[1] = -0.187324f; c.rowG[2] = -0.468124f;
                c.rowB[0] = 1.0f; c.rowB[1] =  1.855600f; c.rowB[2] =  0.0f;
                break;
        }
    } else {
        c.offset[0] = 16.0f / 255.0f; c.offset[1] = 128.0f / 255.0f; c.offset[2] = 128.0f / 255.0f;
        switch (colorMatrix_) {
            case 0: // BT.601 limited
                c.rowR[0] = 1.164383f; c.rowR[1] =  0.0f;      c.rowR[2] =  1.596027f;
                c.rowG[0] = 1.164383f; c.rowG[1] = -0.391762f; c.rowG[2] = -0.812968f;
                c.rowB[0] = 1.164383f; c.rowB[1] =  2.017232f; c.rowB[2] =  0.0f;
                break;
            case 2: // BT.2020 limited
                c.rowR[0] = 1.164383f; c.rowR[1] =  0.0f;      c.rowR[2] =  1.678674f;
                c.rowG[0] = 1.164383f; c.rowG[1] = -0.187326f; c.rowG[2] = -0.650424f;
                c.rowB[0] = 1.164383f; c.rowB[1] =  2.141772f; c.rowB[2] =  0.0f;
                break;
            default: // BT.709 limited
                c.rowR[0] = 1.164383f; c.rowR[1] =  0.0f;      c.rowR[2] =  1.792741f;
                c.rowG[0] = 1.164383f; c.rowG[1] = -0.213249f; c.rowG[2] = -0.532909f;
                c.rowB[0] = 1.164383f; c.rowB[1] =  2.112402f; c.rowB[2] =  0.0f;
                break;
        }
    }

    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(context_->Map(colorCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, &c, sizeof(c));
        context_->Unmap(colorCB_.Get(), 0);
        colorDirty_ = false;
    }
}

// ---- Video texture -----------------------------------------------------------
bool Renderer::EnsureVideoTexture(int width, int height, DXGI_FORMAT format)
{
    if (videoTex_ && videoW_ == width && videoH_ == height && videoFormat_ == format)
        return true;

    videoSRVChroma_.Reset();
    videoSRV_.Reset();
    videoTex_.Reset();
    convSRV_.Reset();
    convRTV_.Reset();
    convTex_.Reset();
    convW_ = convH_ = 0;
    videoW_ = videoH_ = 0;
    videoFormat_ = DXGI_FORMAT_UNKNOWN;

    const bool nv12 = (format == DXGI_FORMAT_NV12);
    if (nv12) {
        // Planar formats need even dimensions.
        width  &= ~1;
        height &= ~1;
        if (width <= 0 || height <= 0) return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width          = (UINT)width;
    td.Height         = (UINT)height;
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = format;
    td.SampleDesc     = { 1, 0 };
    td.Usage          = D3D11_USAGE_DEFAULT;
    // NV12 render targets aren't universally supported; we only ever sample
    // from these.
    td.BindFlags      = nv12 ? D3D11_BIND_SHADER_RESOURCE
                             : (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    td.CPUAccessFlags = 0;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, videoTex_.GetAddressOf()))) return false;

    if (nv12) {
        // Plane 0 (luma) is viewed as R8, plane 1 (chroma) as R8G8 at half
        // resolution — D3D picks the plane from the view format.
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(device_->CreateShaderResourceView(videoTex_.Get(), &sd, videoSRV_.GetAddressOf())))
            return false;
        sd.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(device_->CreateShaderResourceView(videoTex_.Get(), &sd, videoSRVChroma_.GetAddressOf())))
            return false;
    } else {
        if (FAILED(device_->CreateShaderResourceView(videoTex_.Get(), nullptr, videoSRV_.GetAddressOf())))
            return false;
    }

    videoW_ = width;
    videoH_ = height;
    videoFormat_ = format;
    return true;
}

void Renderer::SubmitVideoTexture(ID3D11Texture2D* src, UINT subresource, int width, int height)
{
    if (!src || !device_) return;

    // Adapt to the source's actual size/format — MF-delivered textures can
    // be padded to alignment or use a format other than the one we passed
    // to SetCurrentMediaType (e.g. DXGI_FORMAT_B8G8R8A8_UNORM vs UINT).
    D3D11_TEXTURE2D_DESC srcDesc{};
    src->GetDesc(&srcDesc);

    // Prefer the negotiated frame size so alignment padding doesn't end up
    // stretched across the quad; never ask for more than the source has.
    int useW = (width  > 0 && (UINT)width  < srcDesc.Width)  ? width  : (int)srcDesc.Width;
    int useH = (height > 0 && (UINT)height < srcDesc.Height) ? height : (int)srcDesc.Height;
    if (srcDesc.Format == DXGI_FORMAT_NV12) { useW &= ~1; useH &= ~1; }

    std::lock_guard<std::mutex> lk(videoTexMutex_);
    if (!EnsureVideoTexture(useW, useH, srcDesc.Format)) return;

    if ((UINT)videoW_ == srcDesc.Width && (UINT)videoH_ == srcDesc.Height) {
        // Null box = copy the entire subresource; avoids the silent reject
        // that happens if an explicit box extends past the source's dims.
        context_->CopySubresourceRegion(videoTex_.Get(), 0, 0, 0, 0, src, subresource, nullptr);
    } else {
        D3D11_BOX box{};
        box.left = 0; box.top = 0; box.front = 0;
        box.right = (UINT)videoW_; box.bottom = (UINT)videoH_; box.back = 1;
        context_->CopySubresourceRegion(videoTex_.Get(), 0, 0, 0, 0, src, subresource, &box);
    }
}

void Renderer::SubmitVideoFrame(int width, int height, int stride, const uint8_t* bgra)
{
    if (!bgra || width <= 0 || height <= 0 || !device_) return;

    std::lock_guard<std::mutex> lk(videoTexMutex_);
    if (!EnsureVideoTexture(width, height, DXGI_FORMAT_B8G8R8A8_UNORM)) return;

    if (stride > 0) {
        context_->UpdateSubresource(videoTex_.Get(), 0, nullptr, bgra, (UINT)stride, 0);
    } else {
        // Bottom-up: flip into a scratch buffer first.
        const int absStride = -stride;
        const size_t need = (size_t)absStride * (size_t)height;
        if (cpuFlipBuf_.size() < need) cpuFlipBuf_.resize(need);
        for (int y = 0; y < height; ++y) {
            std::memcpy(cpuFlipBuf_.data() + (size_t)absStride * y,
                        bgra + (size_t)absStride * (height - 1 - y),
                        (size_t)width * 4);
        }
        context_->UpdateSubresource(videoTex_.Get(), 0, nullptr, cpuFlipBuf_.data(), (UINT)absStride, 0);
    }
}

// ---- Frame ops ---------------------------------------------------------------
void Renderer::BeginFrame()
{
    const float clear[4] = { 0.05f, 0.05f, 0.07f, 1.0f };
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clear);

    D3D11_VIEWPORT vp{};
    vp.Width    = (float)width_;
    vp.Height   = (float)height_;
    vp.MinDepth = 0.f;
    vp.MaxDepth = 1.f;
    context_->RSSetViewports(1, &vp);
}

void Renderer::BindVideoShader(bool nv12)
{
    const UINT stride = sizeof(QuadVertex), offset = 0;
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->IASetInputLayout(inputLayout_.Get());
    context_->IASetVertexBuffers(0, 1, vb_.GetAddressOf(), &stride, &offset);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(nv12 ? psNV12_.Get() : ps_.Get(), nullptr, 0);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    if (nv12) {
        UploadColorConstants();
        context_->PSSetConstantBuffers(0, 1, colorCB_.GetAddressOf());
    }
    context_->RSSetState(raster_.Get());
    const float bf[4] = { 1,1,1,1 };
    context_->OMSetBlendState(blendOpaque_.Get(), bf, 0xffffffff);
}

ID3D11Texture2D* Renderer::EnsureBgraConversion()
{
    if (!videoTex_ || videoFormat_ != DXGI_FORMAT_NV12) return videoTex_.Get();

    if (!convTex_ || convW_ != videoW_ || convH_ != videoH_) {
        convSRV_.Reset(); convRTV_.Reset(); convTex_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width      = (UINT)videoW_;
        td.Height     = (UINT)videoH_;
        td.MipLevels  = 1;
        td.ArraySize  = 1;
        td.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc = { 1, 0 };
        td.Usage      = D3D11_USAGE_DEFAULT;
        td.BindFlags  = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, convTex_.GetAddressOf()))) return nullptr;
        if (FAILED(device_->CreateRenderTargetView(convTex_.Get(), nullptr, convRTV_.GetAddressOf()))) return nullptr;
        if (FAILED(device_->CreateShaderResourceView(convTex_.Get(), nullptr, convSRV_.GetAddressOf()))) return nullptr;
        convW_ = videoW_; convH_ = videoH_;
    }

    // Full-screen NV12 -> BGRA pass into the scratch target.
    context_->OMSetRenderTargets(1, convRTV_.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width = (float)convW_; vp.Height = (float)convH_;
    vp.MinDepth = 0.f; vp.MaxDepth = 1.f;
    context_->RSSetViewports(1, &vp);

    BindVideoShader(true);
    ID3D11ShaderResourceView* srvs[2] = { videoSRV_.Get(), videoSRVChroma_.Get() };
    context_->PSSetShaderResources(0, 2, srvs);
    context_->Draw(4, 0);

    ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
    context_->PSSetShaderResources(0, 2, nullSrvs);
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    return convTex_.Get();
}

void Renderer::DrawVideo()
{
    std::lock_guard<std::mutex> lk(videoTexMutex_);
    if (!videoSRV_) return;

    const bool nv12 = (videoFormat_ == DXGI_FORMAT_NV12);

    // Pick which SRV to sample: upscaled if the NVIDIA pass ran successfully
    // this frame, otherwise the original capture.
    ID3D11ShaderResourceView* drawSRV = videoSRV_.Get();
    bool  drawNV12 = nv12;
    int   drawW    = videoW_;
    int   drawH    = videoH_;
    if (upscaler_ && upscaler_->IsEnabled()) {
        // The upscaler only speaks BGRA — convert first when we're on the
        // NV12 capture path.
        ID3D11Texture2D* srcTex = nv12 ? EnsureBgraConversion() : videoTex_.Get();
        if (srcTex) {
            if (auto* srv = upscaler_->Process(srcTex, videoW_, videoH_)) {
                drawSRV  = srv;
                drawNV12 = false;
                drawW    = upscaler_->OutputWidth();
                drawH    = upscaler_->OutputHeight();
            } else if (nv12 && convSRV_) {
                // Conversion happened but SR bailed — draw the BGRA copy.
                drawSRV  = convSRV_.Get();
                drawNV12 = false;
            }
        }
    }

    if (drawW <= 0 || drawH <= 0) return;

    const float winAspect = (float)width_ / (float)height_;
    const float vidAspect = (float)drawW  / (float)drawH;

    D3D11_VIEWPORT vp{};
    if (winAspect > vidAspect) {
        vp.Height   = (float)height_;
        vp.Width    = vp.Height * vidAspect;
        vp.TopLeftX = ((float)width_ - vp.Width) * 0.5f;
        vp.TopLeftY = 0.f;
    } else {
        vp.Width    = (float)width_;
        vp.Height   = vp.Width / vidAspect;
        vp.TopLeftX = 0.f;
        vp.TopLeftY = ((float)height_ - vp.Height) * 0.5f;
    }
    vp.MinDepth = 0.f; vp.MaxDepth = 1.f;
    context_->RSSetViewports(1, &vp);

    BindVideoShader(drawNV12);
    if (drawNV12) {
        ID3D11ShaderResourceView* srvs[2] = { videoSRV_.Get(), videoSRVChroma_.Get() };
        context_->PSSetShaderResources(0, 2, srvs);
    } else {
        context_->PSSetShaderResources(0, 1, &drawSRV);
    }

    context_->Draw(4, 0);

    // Leave nothing bound — the capture thread copies into videoTex_ from
    // another thread and a stale SRV binding would make that a hazard.
    ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
    context_->PSSetShaderResources(0, 2, nullSrvs);
}

void Renderer::DrawUI()
{
    D3D11_VIEWPORT vp{};
    vp.Width = (float)width_; vp.Height = (float)height_;
    vp.MinDepth = 0.f; vp.MaxDepth = 1.f;
    context_->RSSetViewports(1, &vp);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void Renderer::EndFrame()
{
    // VSync mode: the WaitForFrame at the top of the main loop already gated
    // us to DWM composition, so Present(0, 0) here just hands the buffer to
    // DXGI without adding a second vsync-wait.
    // Tearing mode: present immediately so a 144/240 Hz capture isn't held
    // to the desktop's composition rate.
    UINT sync  = frameLatencyWaitable_ ? 0 : 1;
    UINT flags = 0;
    if (presentMode_ == PresentMode::Tearing &&
        (swapFlags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
        sync  = 0;
        flags = DXGI_PRESENT_ALLOW_TEARING;
    }
    HRESULT hr = swap_->Present(sync, flags);
    (void)hr;

    ++framesSinceTick_;
    const uint64_t now = GetTickCount64();
    if (lastFpsTickMs_ == 0) lastFpsTickMs_ = now;
    const uint64_t dt = now - lastFpsTickMs_;
    if (dt >= 500) {
        fps_ = (float)framesSinceTick_ * 1000.0f / (float)dt;
        framesSinceTick_ = 0;
        lastFpsTickMs_ = now;
    }
}
