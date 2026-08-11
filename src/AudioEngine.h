#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>

struct AudioDevice
{
    std::wstring friendlyName;
    std::wstring id; // IMMDevice::GetId
};

class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    static std::vector<AudioDevice> EnumerateCaptureDevices();
    static std::vector<AudioDevice> EnumerateRenderDevices();

    // captureId / renderId may be empty — empty means "system default".
    bool Start(const std::wstring& captureId, const std::wstring& renderId);
    void Stop();

    bool IsRunning() const { return running_.load(); }

    // Volume gain in [0, 2]. 1.0 = unity.
    void  SetGain(float gain) { targetGain_.store(gain); }
    float GetGain() const     { return targetGain_.load(); }

    // Swap the left/right channels on the way in — useful when a camera
    // mic or USB headset reports L/R reversed.
    void  SetSwapLR(bool on) { swapLR_.store(on); }
    bool  GetSwapLR() const  { return swapLR_.load(); }

    // Realtime-ish metering. Updated from capture thread; readers may see
    // slightly stale values, which is fine for a VU meter.
    float CurrentPeak() const { return peak_.load(); }

    std::wstring LastError();

private:
    void CaptureThreadMain();
    void RenderThreadMain();
    void SetError(const std::wstring& msg);

    // --- Ring buffer (SPSC, float samples, interleaved) ---
    std::vector<float>      ring_;
    size_t                  capacity_ = 0;
    std::atomic<size_t>     writeIdx_{ 0 };
    std::atomic<size_t>     readIdx_{ 0 };
    size_t RingWrite(const float* src, size_t n);
    size_t RingRead(float* dst, size_t n);
    void   RingClear();

    std::atomic<bool>       running_{ false };
    std::thread             captureThread_;
    std::thread             renderThread_;

    std::wstring            captureId_;
    std::wstring            renderId_;

    std::atomic<float>      targetGain_{ 1.0f };
    float                   currentGain_ = 1.0f;
    std::atomic<bool>       swapLR_{ false };

    std::atomic<float>      peak_{ 0.0f };

    mutable std::mutex      errorMutex_;
    std::wstring            lastError_;

    // Common format negotiated between capture and render threads.
    uint32_t                sampleRate_ = 48000;
    uint16_t                channels_   = 2;
};
