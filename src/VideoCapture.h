#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdint>

struct VideoDevice
{
    std::wstring friendlyName;
    std::wstring symbolicLink;
};

// One native capture mode exposed by a camera — resolution + framerate +
// pixel format. `key` is a stable identifier safe to persist in settings.
struct CameraMode
{
    uint32_t    width          = 0;
    uint32_t    height         = 0;
    uint32_t    fpsN           = 0;
    uint32_t    fpsD           = 1;
    uint32_t    subtypeFourcc  = 0; // low 32 bits of MF_MT_SUBTYPE GUID
    std::string label;              // "3840x2160 @ 60 fps  NV12  (4K)"
    std::string key;                // "3840x2160_60_3231564E"

    bool  valid() const { return width > 0 && height > 0; }
    float fps()   const { return fpsD ? (float)fpsN / (float)fpsD : 0.0f; }
    // Rounded framerate — 60000/1001 (59.94) reads as 60, 144000/1001 as 144.
    uint32_t fpsRounded() const { return (uint32_t)(fps() + 0.5f); }
};

// How Auto picks a mode when the user hasn't pinned one.
enum class ModePreference
{
    Resolution = 0, // highest resolution first  (4K60 over 1080p144)
    Framerate  = 1, // highest framerate first   (1080p144 over 4K60)
};

// Pixel layout of the frames the reader hands us.
enum class CaptureFormat
{
    BGRA,  // RGB32 — converted by MF's video processor
    NV12,  // native 4:2:0 — no conversion, shader does YUV->RGB
};

class VideoCapture
{
public:
    // Both callbacks fire on an MF worker thread.
    // textureCb is used when MF delivers a GPU texture (fast path).
    // bytesCb   is used when MF falls back to a CPU buffer.
    using TextureCallback = std::function<void(ID3D11Texture2D* src, UINT subresource, int w, int h)>;
    using BytesCallback   = std::function<void(int w, int h, int stride, const uint8_t* bgra)>;

    // Everything Start() needs beyond the device + callbacks.
    struct StartOptions
    {
        const CameraMode* preferred  = nullptr;                  // pinned mode, or null for Auto
        ModePreference    preference = ModePreference::Resolution;
        // Ask the reader for NV12 instead of RGB32 when we're on the GPU
        // path. Skips a full-frame color conversion per frame — the
        // difference between "4K144 works" and "4K144 drops frames".
        bool              allowNV12  = true;
    };

    VideoCapture();
    ~VideoCapture();

    static std::vector<VideoDevice> EnumerateDevices();

    // List every native media type exposed by the device at symbolicLink,
    // de-duplicated and sorted (biggest/fastest first).
    // Runs in a few ms; safe to call whenever the user picks a camera.
    static std::vector<CameraMode> EnumerateModes(const std::wstring& symbolicLink);

    // device — the renderer's ID3D11Device; used to bind MF's DXGI manager.
    bool Start(ID3D11Device* device, const std::wstring& symbolicLink,
               TextureCallback textureCb, BytesCallback bytesCb,
               const StartOptions& options = {});
    void Stop();
    bool IsRunning() const { return running_.load(); }

    int Width()  const { return width_;  }
    int Height() const { return height_; }

    // Frames-per-second delivered by MF to our callback (updated ~2 Hz).
    float CaptureFps() const { return captureFps_.load(); }

    // Framerate MF negotiated with the device, e.g. 144.0. 0 if unknown.
    float NegotiatedFps() const { return negotiatedFps_; }

    CaptureFormat Format()      const { return format_; }
    bool          UsingGpuPath() const { return gpuPath_; }

    // YUV->RGB conversion parameters for the NV12 path.
    // 0 = BT.601, 1 = BT.709, 2 = BT.2020.
    int  ColorMatrix() const { return colorMatrix_; }
    bool FullRange()   const { return fullRange_;   }

    // Human-readable summary of what we negotiated, for the UI/status line.
    std::string NegotiatedSummary() const;

    std::wstring LastError();

private:
    class ReaderCallback; // IMFSourceReaderCallback impl
    friend class ReaderCallback;

    void OnSample(IMFSample* sample);
    void OnReadError(HRESULT hr);
    void RequestNextSample();
    void UpdateCaptureFps();
    void SetError(const std::wstring& msg);

    // COM/MF state
    Microsoft::WRL::ComPtr<IMFMediaSource>  source_;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    Microsoft::WRL::ComPtr<ReaderCallback>  callback_;
    HANDLE                                  flushDoneEvent_ = nullptr;

    TextureCallback  texCb_;
    BytesCallback    bytesCb_;
    bool             gpuPath_ = false;
    CaptureFormat    format_  = CaptureFormat::BGRA;
    int              colorMatrix_ = 1;      // BT.709 by default (HD)
    bool             fullRange_   = false;  // studio/limited by default
    float            negotiatedFps_ = 0.0f;
    int              width_  = 0;
    int              height_ = 0;

    std::atomic<bool>     running_{ false };
    std::atomic<float>    captureFps_{ 0.0f };
    std::atomic<uint32_t> framesSinceTick_{ 0 };
    std::atomic<uint64_t> lastFpsTickMs_{ 0 };

    mutable std::mutex  errorMutex_;
    std::wstring        lastError_;
};
