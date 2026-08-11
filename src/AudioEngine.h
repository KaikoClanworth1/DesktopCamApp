#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>

#include "FrameRing.h"

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
    // mic or USB headset genuinely reports L/R reversed.
    void  SetSwapLR(bool on) { swapLR_.store(on); }
    bool  GetSwapLR() const  { return swapLR_.load(); }

    // How much audio we deliberately keep queued between capture and render,
    // in milliseconds. Lower = less delay, more sensitive to scheduling
    // hiccups. The render thread trims anything above this every period, so
    // latency can't creep upwards over a long session.
    void  SetTargetLatencyMs(int ms);
    int   GetTargetLatencyMs() const { return targetLatencyMs_.load(); }

    // Use IAudioClient3's minimum engine period when the driver offers one
    // (typically ~3 ms instead of the default 10 ms). Applied on next Start.
    void  SetLowLatencyMode(bool on) { lowLatency_.store(on); }
    bool  GetLowLatencyMode() const  { return lowLatency_.load(); }

    // Realtime-ish metering. Updated from capture thread; readers may see
    // slightly stale values, which is fine for a VU meter.
    float CurrentPeak() const { return peak_.load(); }

    // Measured end-to-end passthrough delay: capture period + queued audio +
    // render period. 0 when not running.
    float MeasuredLatencyMs() const;
    float CapturePeriodMs()   const;
    float RenderPeriodMs()    const;
    uint32_t Underruns() const { return underruns_.load(); }
    uint32_t Overruns()  const { return overruns_.load();  }

    std::wstring LastError();

private:
    void CaptureThreadMain();
    void RenderThreadMain();
    void SetError(const std::wstring& msg);

    // Capture thread writes, render thread reads. See FrameRing.h for why
    // this counts frames rather than samples.
    FrameRing               ring_;

    std::atomic<bool>       running_{ false };
    std::thread             captureThread_;
    std::thread             renderThread_;

    std::wstring            captureId_;
    std::wstring            renderId_;

    std::atomic<float>      targetGain_{ 1.0f };
    float                   currentGain_ = 1.0f;
    std::atomic<bool>       swapLR_{ false };

    std::atomic<int>        targetLatencyMs_{ 25 };
    std::atomic<bool>       lowLatency_{ true };

    std::atomic<float>      peak_{ 0.0f };
    std::atomic<uint32_t>   capturePeriodFrames_{ 0 };
    std::atomic<uint32_t>   renderPeriodFrames_{ 0 };
    std::atomic<uint32_t>   underruns_{ 0 };
    std::atomic<uint32_t>   overruns_{ 0 };

    mutable std::mutex      errorMutex_;
    std::wstring            lastError_;

    // Common format negotiated between capture and render threads.
    uint32_t                sampleRate_ = 48000;
    uint16_t                channels_   = 2;
};
