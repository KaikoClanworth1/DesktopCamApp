#pragma once

// NVIDIA Broadcast SDK — Video Effects "Super Resolution" upscaler.
//
// This is an **experimental** feature — requires:
//   * An NVIDIA RTX GPU (Turing / Ampere / Ada / Blackwell).
//   * The NVIDIA Broadcast SDK redistributable installed. Download at
//     https://www.nvidia.com/en-us/geforce/broadcasting/broadcast-sdk/resources/
//     The installer lays down NVVideoEffects.dll plus the required model
//     files in `C:\Program Files\NVIDIA Corporation\NVIDIA Video Effects\`.
//   * A working CUDA runtime (cudart64_*.dll) — normally present with any
//     recent NVIDIA driver.
//
// The entire SDK is loaded dynamically. If anything is missing the class
// reports its status and the app keeps running without upscaling.

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <mutex>
#include <cstdint>

class UpscalerNV
{
public:
    enum class Status
    {
        NotInitialized,
        DllMissing,          // NVVideoEffects.dll not found on PATH
        CudaMissing,         // nvcuda / cudart not loadable
        RequiresRTX,         // SDK present but current GPU rejected
        EffectLoadFailed,    // Model directory / parameters missing
        Ready,
    };

    UpscalerNV();
    ~UpscalerNV();

    // Try to initialize. Call once after the Renderer is up.
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    Status       GetStatus()         const { return status_; }
    std::wstring StatusMessage()     const;
    bool         IsReady()           const { return status_ == Status::Ready; }

    // User-facing knobs.
    void  SetEnabled(bool on)     { enabled_ = on; }
    bool  IsEnabled()       const { return enabled_ && IsReady(); }

    // Allowed scales are 1.3, 1.5, 2.0, 3.0, 4.0.
    void  SetScale(float s)       { scale_ = s; dirty_ = true; }
    float GetScale()        const { return scale_; }

    // 0 = quality, 1 = performance.
    void  SetMode(int m)          { mode_ = m; dirty_ = true; }
    int   GetMode()         const { return mode_; }

    // Run super-resolution on src (BGRA). Returns an SRV for the upscaled
    // texture (owned by this class) or nullptr if the effect can't run
    // right now — caller falls back to the source SRV in that case.
    ID3D11ShaderResourceView* Process(ID3D11Texture2D* src, int srcW, int srcH);

    int  OutputWidth()  const { return outW_; }
    int  OutputHeight() const { return outH_; }

private:
    template <class T>
    using CPtr = Microsoft::WRL::ComPtr<T>;

    // --- SDK loading / resolution ----
    bool LoadDlls();
    void UnloadDlls();
    bool ResolveFunctions();

    // --- Effect lifecycle ----
    bool CreateEffectAndStream();
    void DestroyEffectAndStream();

    // --- Per-frame buffers ----
    bool EnsureBuffers(int srcW, int srcH);
    void ReleaseBuffers();

    // Status & log.
    void  SetStatus(Status s, const std::wstring& msg);

    ID3D11Device*          device_  = nullptr;
    ID3D11DeviceContext*   context_ = nullptr;

    HMODULE  vfxDll_  = nullptr;
    HMODULE  cvDll_   = nullptr; // NVCVImage.dll — holds the NvCVImage_* helpers
    HMODULE  cudaDll_ = nullptr;

    // NVIDIA handles — stored as void* so the header doesn't need SDK types.
    void*        effect_         = nullptr;
    void*        cudaStream_     = nullptr;

    // Opaque NvCVImage blocks (each ~64 bytes). Heap-allocated so the exact
    // struct size doesn't have to be declared in the header.
    void*        srcD3DImg_      = nullptr; // D3D11-backed wrapper
    void*        srcGpuImg_      = nullptr; // CUDA  BGR f32 planar (SR input)
    void*        dstGpuImg_      = nullptr; // CUDA  BGR f32 planar (SR output)
    void*        dstTmpImg_      = nullptr; // CUDA  BGRA u8 chunky (intermediate)
    void*        dstD3DImg_      = nullptr; // D3D11 BGRA u8 chunky (our outTex_)
    void*        stagingImg_     = nullptr; // NvCVImage_Transfer scratch

    CPtr<ID3D11Texture2D>          outTex_;
    CPtr<ID3D11ShaderResourceView> outSRV_;
    int  outW_ = 0, outH_ = 0;
    int  inW_  = 0, inH_  = 0;
    ID3D11Texture2D*               lastSrcTex_ = nullptr;

    bool   enabled_ = false;
    float  scale_   = 1.5f;
    int    mode_    = 1;
    bool   dirty_   = true; // parameters changed, need to re-apply to effect

    Status       status_ = Status::NotInitialized;
    std::wstring statusMsg_;
    mutable std::mutex statusMutex_;
};
