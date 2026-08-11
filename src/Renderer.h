#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>
#include <vector>

template <class T>
using CPtr = Microsoft::WRL::ComPtr<T>;

class UpscalerNV;

class Renderer
{
public:
    // How frames reach the screen.
    enum class PresentMode
    {
        VSync   = 0, // gate on DWM's waitable, one frame per composition
        Tearing = 1, // present immediately, allow tearing (needs ALLOW_TEARING)
    };

    Renderer();
    ~Renderer();

    bool  Initialize(HWND hwnd, int width, int height);
    void  Shutdown();

    void  Resize(int width, int height);

    // Depth of the pre-render queue. 1 = lowest latency (can stutter on
    // slower systems), 3 = driver default (smoothest). Safe to call any
    // time after Initialize.
    void  SetFrameLatency(UINT frames);

    void  SetPresentMode(PresentMode m);
    PresentMode GetPresentMode() const { return presentMode_; }
    bool  TearingSupported() const { return tearingSupported_; }

    // Optional cap for the render loop, in frames per second. 0 = unlimited.
    // Only meaningful in Tearing mode (VSync is already paced by DWM).
    void  SetFpsLimit(int fps) { fpsLimit_ = fps < 0 ? 0 : fps; }
    int   GetFpsLimit() const  { return fpsLimit_; }

    // Refresh rate of the monitor the window currently sits on, in Hz.
    // 0 if it couldn't be determined.
    int   MonitorRefreshHz() const;

    // If a waitable swap chain is in use, block until the swap chain is ready
    // for the next frame (and honour the FPS limit). Cheap no-op otherwise.
    // Call at the top of every main-loop iteration.
    void  WaitForFrame();

    void  BeginFrame();
    void  DrawVideo();
    void  DrawUI();
    void  EndFrame();

    // Fast path: GPU→GPU copy from an MF-delivered D3D texture. Handles both
    // BGRA and NV12 sources; `width`/`height` are the negotiated frame size,
    // which can be smaller than the texture when MF pads for alignment.
    void  SubmitVideoTexture(ID3D11Texture2D* src, UINT subresource, int width, int height);

    // CPU fallback for when the MF pipeline couldn't give us a D3D buffer
    // (e.g., driver doesn't support DXVA advanced video processing).
    // Expects BGRA. Positive stride = top-down, negative = bottom-up.
    void  SubmitVideoFrame(int width, int height, int stride, const uint8_t* bgra);

    // YUV->RGB coefficients for the NV12 path.
    // matrix: 0 = BT.601, 1 = BT.709, 2 = BT.2020.
    void  SetVideoColorSpace(int matrix, bool fullRange);

    // True when this device can sample NV12 textures directly — if not, the
    // capture side has to ask Media Foundation for RGB32 instead.
    bool  SupportsNV12() const { return nv12Supported_; }

    ID3D11Device*        Device()  const { return device_.Get();  }
    ID3D11DeviceContext* Context() const { return context_.Get(); }
    IDXGISwapChain1*     Swap()    const { return swap_.Get();    }

    int  Width()  const { return width_;  }
    int  Height() const { return height_; }

    // Size of the frame currently being displayed (after upscaling, if any).
    void VideoTextureSize(int& w, int& h) const { w = videoW_; h = videoH_; }

    float Fps() const { return fps_; }

    // Optional NVIDIA Super Resolution pass on the video quad. Owned by
    // Application; Renderer only reads from it during DrawVideo.
    void  SetUpscaler(UpscalerNV* u) { upscaler_ = u; }

private:
    bool  CreateDeviceAndSwap(HWND hwnd, int width, int height);
    bool  CreateBackbufferViews();
    void  DestroyBackbufferViews();
    bool  CreatePipeline();
    void  UploadColorConstants();

    bool  EnsureVideoTexture(int width, int height, DXGI_FORMAT format);
    // Converts the current NV12 video texture into a BGRA render target so
    // the NVIDIA upscaler (which only speaks BGRA) can consume it.
    ID3D11Texture2D* EnsureBgraConversion();
    void  BindVideoShader(bool nv12);

    HWND                          hwnd_ = nullptr;
    int                           width_ = 0;
    int                           height_ = 0;

    CPtr<ID3D11Device>            device_;
    CPtr<ID3D11DeviceContext>     context_;
    CPtr<IDXGISwapChain2>         swap_;
    HANDLE                        frameLatencyWaitable_ = nullptr;
    UINT                          swapFlags_ = 0;
    bool                          tearingSupported_ = false;
    bool                          nv12Supported_    = false;
    PresentMode                   presentMode_ = PresentMode::VSync;
    int                           fpsLimit_ = 0;
    uint64_t                      nextFrameQpc_ = 0;
    int64_t                       qpcFreq_ = 0;
    CPtr<ID3D11RenderTargetView>  rtv_;

    CPtr<ID3D11VertexShader>      vs_;
    CPtr<ID3D11PixelShader>       ps_;      // BGRA passthrough
    CPtr<ID3D11PixelShader>       psNV12_;  // NV12 -> RGB
    CPtr<ID3D11Buffer>            colorCB_;
    CPtr<ID3D11InputLayout>       inputLayout_;
    CPtr<ID3D11Buffer>            vb_;
    CPtr<ID3D11SamplerState>      sampler_;
    CPtr<ID3D11RasterizerState>   raster_;
    CPtr<ID3D11BlendState>        blendOpaque_;

    // Shader texture (filled via CopySubresourceRegion from MF-delivered GPU
    // textures, or UpdateSubresource from CPU bytes in fallback).
    std::mutex                    videoTexMutex_;
    CPtr<ID3D11Texture2D>         videoTex_;
    CPtr<ID3D11ShaderResourceView> videoSRV_;       // BGRA, or NV12 luma
    CPtr<ID3D11ShaderResourceView> videoSRVChroma_; // NV12 chroma
    int                           videoW_ = 0;
    int                           videoH_ = 0;
    DXGI_FORMAT                   videoFormat_ = DXGI_FORMAT_UNKNOWN;
    std::vector<uint8_t>          cpuFlipBuf_;

    // NV12 -> BGRA scratch target, only allocated when the upscaler needs it.
    CPtr<ID3D11Texture2D>          convTex_;
    CPtr<ID3D11RenderTargetView>   convRTV_;
    CPtr<ID3D11ShaderResourceView> convSRV_;
    int                            convW_ = 0, convH_ = 0;

    int                           colorMatrix_ = 1;
    bool                          colorFullRange_ = false;
    bool                          colorDirty_ = true;

    // FPS tracking.
    uint64_t                      lastFpsTickMs_ = 0;
    uint32_t                      framesSinceTick_ = 0;
    float                         fps_ = 0.0f;

    UpscalerNV*                   upscaler_ = nullptr;
};
