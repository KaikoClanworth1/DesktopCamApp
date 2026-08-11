#include "AudioEngine.h"

#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <wrl/client.h>
#include <ksmedia.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

template <class T> using CPtr = Microsoft::WRL::ComPtr<T>;

static const CLSID CLSID_MMDeviceEnumerator_ = __uuidof(MMDeviceEnumerator);
static const IID   IID_IMMDeviceEnumerator_  = __uuidof(IMMDeviceEnumerator);
static const IID   IID_IAudioClient_         = __uuidof(IAudioClient);
static const IID   IID_IAudioCaptureClient_  = __uuidof(IAudioCaptureClient);
static const IID   IID_IAudioRenderClient_   = __uuidof(IAudioRenderClient);

namespace {

struct ComApartment
{
    bool ok = false;
    ComApartment() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComApartment() { if (ok) CoUninitialize(); }
};

// Build a canonical target format: 48 kHz / N-channel float32.
static void MakeTargetFormat(WAVEFORMATEXTENSIBLE& wfx, uint32_t sampleRate, uint16_t channels)
{
    wfx = {};
    wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels       = channels;
    wfx.Format.nSamplesPerSec  = sampleRate;
    wfx.Format.wBitsPerSample  = 32;
    wfx.Format.nBlockAlign     = wfx.Format.nChannels * wfx.Format.wBitsPerSample / 8;
    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
    wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = (channels == 1)
        ? (DWORD)SPEAKER_FRONT_CENTER
        : (DWORD)(SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

static std::wstring GetDeviceFriendlyName(IMMDevice* device)
{
    std::wstring out;
    CPtr<IPropertyStore> ps;
    if (FAILED(device->OpenPropertyStore(STGM_READ, ps.GetAddressOf()))) return out;
    PROPVARIANT pv; PropVariantInit(&pv);
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
        out = pv.pwszVal;
    PropVariantClear(&pv);
    return out;
}

static std::vector<AudioDevice> EnumerateFlow(EDataFlow flow)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::vector<AudioDevice> out;
    CPtr<IMMDeviceEnumerator> enumer;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator_, (void**)enumer.GetAddressOf())))
        return out;

    CPtr<IMMDeviceCollection> coll;
    if (FAILED(enumer->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, coll.GetAddressOf())))
        return out;

    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i)
    {
        CPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, dev.GetAddressOf()))) continue;
        AudioDevice d;
        d.friendlyName = GetDeviceFriendlyName(dev.Get());
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
            d.id = id;
            CoTaskMemFree(id);
        }
        if (!d.id.empty()) out.push_back(std::move(d));
    }
    return out;
}

} // namespace

// ---- Public statics ----------------------------------------------------------
std::vector<AudioDevice> AudioEngine::EnumerateCaptureDevices() { return EnumerateFlow(eCapture); }
std::vector<AudioDevice> AudioEngine::EnumerateRenderDevices()  { return EnumerateFlow(eRender);  }

// ---- Ctor/dtor ---------------------------------------------------------------
AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { Stop(); }

// ---- Lifecycle ---------------------------------------------------------------
bool AudioEngine::Start(const std::wstring& captureId, const std::wstring& renderId)
{
    Stop();

    captureId_ = captureId;
    renderId_  = renderId;

    // The ring only ever holds `targetLatencyMs_` worth of audio in steady
    // state — the render thread trims the rest. 250 ms of capacity just gives
    // a scheduling hiccup somewhere to land without dropping frames.
    ring_.Reset((size_t)sampleRate_ / 4, channels_);
    currentGain_ = targetGain_.load();
    underruns_.store(0);
    overruns_.store(0);
    capturePeriodFrames_.store(0);
    renderPeriodFrames_.store(0);

    running_.store(true);
    captureThread_ = std::thread(&AudioEngine::CaptureThreadMain, this);
    renderThread_  = std::thread(&AudioEngine::RenderThreadMain,  this);
    return true;
}

void AudioEngine::Stop()
{
    running_.store(false);
    if (captureThread_.joinable()) captureThread_.join();
    if (renderThread_.joinable())  renderThread_.join();
    ring_.Release();
    peak_.store(0.0f);
    capturePeriodFrames_.store(0);
    renderPeriodFrames_.store(0);
}

void AudioEngine::SetTargetLatencyMs(int ms)
{
    if (ms < 5)   ms = 5;
    if (ms > 200) ms = 200;
    targetLatencyMs_.store(ms);
}

float AudioEngine::CapturePeriodMs() const
{
    return running_.load()
        ? (float)capturePeriodFrames_.load() * 1000.0f / (float)sampleRate_ : 0.0f;
}

float AudioEngine::RenderPeriodMs() const
{
    return running_.load()
        ? (float)renderPeriodFrames_.load() * 1000.0f / (float)sampleRate_ : 0.0f;
}

float AudioEngine::MeasuredLatencyMs() const
{
    if (!running_.load() || ring_.Capacity() == 0) return 0.0f;
    const size_t queued = ring_.Available();
    const float frames = (float)(queued + capturePeriodFrames_.load() + renderPeriodFrames_.load());
    return frames * 1000.0f / (float)sampleRate_;
}

void AudioEngine::SetError(const std::wstring& msg)
{
    std::lock_guard<std::mutex> lk(errorMutex_);
    lastError_ = msg;
}

std::wstring AudioEngine::LastError()
{
    std::lock_guard<std::mutex> lk(errorMutex_);
    return lastError_;
}

// ---- Client initialisation ---------------------------------------------------
// Activates an IAudioClient and initialises it for the lowest latency the
// endpoint will give us:
//
//   1. IAudioClient3 at the driver's *minimum* engine period (commonly ~3 ms
//      instead of the 10 ms default) when low-latency mode is on.
//   2. Otherwise IAudioClient::Initialize with a buffer duration of 0, which
//      means "the engine's default period" — the old code asked for 20/30 ms
//      buffers explicitly and paid for them on every packet.
//
// periodFrames comes back as the size of one engine period so callers can
// avoid ever queueing more than a single period ahead.
static HRESULT InitAudioClient(IMMDevice* dev, bool lowLatency,
                               uint32_t sampleRate, uint16_t channels,
                               CPtr<IAudioClient>& outClient,
                               UINT32& periodFrames, bool& usedLowLatency)
{
    usedLowLatency = false;
    periodFrames   = 0;

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                      | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                      | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    WAVEFORMATEXTENSIBLE wfx;
    MakeTargetFormat(wfx, sampleRate, channels);

    if (lowLatency) {
        CPtr<IAudioClient> client;
        if (SUCCEEDED(dev->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr,
                                    (void**)client.GetAddressOf()))) {
            CPtr<IAudioClient3> ac3;
            if (SUCCEEDED(client.As(&ac3))) {
                UINT32 defP = 0, fundP = 0, minP = 0, maxP = 0;
                if (SUCCEEDED(ac3->GetSharedModeEnginePeriod((WAVEFORMATEX*)&wfx,
                                                             &defP, &fundP, &minP, &maxP)) &&
                    minP > 0)
                {
                    if (SUCCEEDED(ac3->InitializeSharedAudioStream(flags, minP,
                                                                   (WAVEFORMATEX*)&wfx, nullptr)))
                    {
                        outClient      = client;
                        periodFrames   = minP;
                        usedLowLatency = true;
                        return S_OK;
                    }
                }
            }
        }
        // Anything above failing just means this endpoint won't do the
        // low-latency path — fall through with a fresh client, since a failed
        // Initialize leaves the old one in an undefined state.
    }

    CPtr<IAudioClient> client;
    HRESULT hr = dev->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr, (void**)client.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, (WAVEFORMATEX*)&wfx, nullptr);
    if (FAILED(hr)) return hr;

    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    if (SUCCEEDED(client->GetDevicePeriod(&defPeriod, &minPeriod)) && defPeriod > 0)
        periodFrames = (UINT32)((double)defPeriod * (double)sampleRate / 10000000.0 + 0.5);
    if (periodFrames == 0) periodFrames = sampleRate / 100; // 10 ms fallback

    outClient = client;
    return S_OK;
}

// ---- Helpers to open endpoints -----------------------------------------------
static HRESULT OpenDevice(const std::wstring& id, EDataFlow flow, IMMDevice** out)
{
    CPtr<IMMDeviceEnumerator> enumer;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_ALL,
                                   IID_IMMDeviceEnumerator_, (void**)enumer.GetAddressOf());
    if (FAILED(hr)) return hr;
    if (id.empty()) {
        return enumer->GetDefaultAudioEndpoint(flow, eConsole, out);
    }
    return enumer->GetDevice(id.c_str(), out);
}

// ---- Capture thread ----------------------------------------------------------
void AudioEngine::CaptureThreadMain()
{
    ComApartment apt;
    DWORD taskIdx = 0;
    HANDLE avTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);

    CPtr<IMMDevice>   dev;
    CPtr<IAudioClient> client;
    CPtr<IAudioCaptureClient> capture;
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto cleanup = [&]() {
        if (client) client->Stop();
        if (evt) CloseHandle(evt);
        if (avTask) AvRevertMmThreadCharacteristics(avTask);
    };

    if (FAILED(OpenDevice(captureId_, eCapture, dev.GetAddressOf()))) {
        SetError(L"OpenDevice(capture) failed");
        running_.store(false);
        cleanup();
        return;
    }

    UINT32 periodFrames = 0;
    bool   lowLatencyUsed = false;
    HRESULT hr = InitAudioClient(dev.Get(), lowLatency_.load(), sampleRate_, channels_,
                                 client, periodFrames, lowLatencyUsed);
    if (FAILED(hr) || !client) {
        SetError(L"AudioClient::Initialize(capture) failed");
        running_.store(false);
        cleanup();
        return;
    }
    capturePeriodFrames_.store(periodFrames);
    wprintf(L"[audio] capture period = %u frames (%.2f ms)%s\n",
            periodFrames, (float)periodFrames * 1000.0f / (float)sampleRate_,
            lowLatencyUsed ? L" [IAudioClient3]" : L"");
    client->SetEventHandle(evt);
    if (FAILED(client->GetService(IID_IAudioCaptureClient_, (void**)capture.GetAddressOf()))) {
        SetError(L"GetService(capture) failed");
        running_.store(false);
        cleanup();
        return;
    }

    if (FAILED(client->Start())) {
        SetError(L"AudioClient::Start(capture) failed");
        running_.store(false);
        cleanup();
        return;
    }

    const size_t chunkCap = (size_t)sampleRate_ / 10 * channels_;
    std::vector<float> work;
    work.reserve(chunkCap);

    while (running_.load())
    {
        DWORD waitRc = WaitForSingleObject(evt, 200);
        if (!running_.load()) break;
        if (waitRc != WAIT_OBJECT_0) continue;

        UINT32 packetSize = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packetSize)) && packetSize > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD  f = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &f, nullptr, nullptr))) break;

            const size_t samples = (size_t)frames * channels_;
            work.resize(samples);

            if (f & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::memset(work.data(), 0, samples * sizeof(float));
            } else if (data) {
                std::memcpy(work.data(), data, samples * sizeof(float));
            }

            // Apply gain (smoothed), clip to [-1, 1], update peak.
            // The gain ramp advances once per FRAME, not once per sample —
            // stepping it per sample gave L and R slightly different gains
            // while the slider was moving.
            const float target = targetGain_.load();
            float gain = currentGain_;
            const float smooth = 0.002f;
            float peak = 0.0f;
            for (UINT32 fr = 0; fr < frames; ++fr) {
                gain += (target - gain) * smooth;
                for (uint16_t c = 0; c < channels_; ++c) {
                    float v = work[(size_t)fr * channels_ + c] * gain;
                    if (v >  1.0f) v =  1.0f;
                    if (v < -1.0f) v = -1.0f;
                    work[(size_t)fr * channels_ + c] = v;
                    const float a = v < 0 ? -v : v;
                    if (a > peak) peak = a;
                }
            }
            currentGain_ = gain;

            // Optional L/R swap for mics that genuinely report channels
            // reversed. Operates on whole frames.
            if (swapLR_.load() && channels_ == 2) {
                for (UINT32 fr = 0; fr < frames; ++fr)
                    std::swap(work[(size_t)fr * 2], work[(size_t)fr * 2 + 1]);
            }

            // Update peak meter with a slight decay.
            float oldPeak = peak_.load();
            float newPeak = std::max(peak, oldPeak * 0.85f);
            peak_.store(newPeak);

            if (ring_.Write(work.data(), frames) < frames)
                overruns_.fetch_add(1);
            capture->ReleaseBuffer(frames);
            capture->GetNextPacketSize(&packetSize);
        }
    }

    cleanup();
}

// ---- Render thread -----------------------------------------------------------
void AudioEngine::RenderThreadMain()
{
    ComApartment apt;
    DWORD taskIdx = 0;
    HANDLE avTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);

    CPtr<IMMDevice>   dev;
    CPtr<IAudioClient> client;
    CPtr<IAudioRenderClient> render;
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto cleanup = [&]() {
        if (client) client->Stop();
        if (evt) CloseHandle(evt);
        if (avTask) AvRevertMmThreadCharacteristics(avTask);
    };

    if (FAILED(OpenDevice(renderId_, eRender, dev.GetAddressOf()))) {
        SetError(L"OpenDevice(render) failed");
        running_.store(false);
        cleanup();
        return;
    }
    UINT32 periodFrames = 0;
    bool   lowLatencyUsed = false;
    HRESULT hr = InitAudioClient(dev.Get(), lowLatency_.load(), sampleRate_, channels_,
                                 client, periodFrames, lowLatencyUsed);
    if (FAILED(hr) || !client) {
        SetError(L"AudioClient::Initialize(render) failed");
        running_.store(false);
        cleanup();
        return;
    }
    renderPeriodFrames_.store(periodFrames);
    wprintf(L"[audio] render period = %u frames (%.2f ms)%s\n",
            periodFrames, (float)periodFrames * 1000.0f / (float)sampleRate_,
            lowLatencyUsed ? L" [IAudioClient3]" : L"");
    client->SetEventHandle(evt);

    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);
    if (periodFrames == 0 || periodFrames > bufferFrames) periodFrames = bufferFrames;

    if (FAILED(client->GetService(IID_IAudioRenderClient_, (void**)render.GetAddressOf()))) {
        SetError(L"GetService(render) failed");
        running_.store(false);
        cleanup();
        return;
    }

    // Deliberately no silence pre-fill. The old code filled the entire render
    // buffer with silence before starting, and that silence never drains — it
    // sits permanently in front of the live audio, adding a whole buffer of
    // delay for the rest of the session.

    if (FAILED(client->Start())) {
        SetError(L"AudioClient::Start(render) failed");
        running_.store(false);
        cleanup();
        return;
    }

    std::vector<float> chunk;
    chunk.reserve((size_t)bufferFrames * channels_);

    while (running_.load())
    {
        DWORD waitRc = WaitForSingleObject(evt, 200);
        if (!running_.load()) break;
        if (waitRc != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        UINT32 framesToFill = bufferFrames - padding;
        // Never queue more than one engine period ahead. Topping the whole
        // buffer up every wake-up is what pinned latency at the full buffer
        // size — the endpoint only needs one period to stay fed.
        if (framesToFill > periodFrames) framesToFill = periodFrames;
        if (framesToFill == 0) continue;

        // Capture and render endpoints run off independent clocks, so the
        // queue drifts. Without trimming, any hiccup permanently adds delay
        // and the ring eventually sits full for the rest of the session.
        // Trim from the oldest end, in whole frames.
        size_t targetFrames = (size_t)targetLatencyMs_.load() * sampleRate_ / 1000;
        if (targetFrames < framesToFill) targetFrames = framesToFill;
        const size_t avail = ring_.Available();
        if (avail > targetFrames)
            ring_.DropOldest(avail - targetFrames);

        const size_t sampleCount = (size_t)framesToFill * channels_;
        chunk.resize(sampleCount);

        const size_t gotFrames = ring_.Read(chunk.data(), framesToFill);
        if (gotFrames < framesToFill) {
            // Underrun — pad the tail with silence, on a frame boundary.
            std::memset(chunk.data() + gotFrames * channels_, 0,
                        (sampleCount - gotFrames * channels_) * sizeof(float));
            if (gotFrames == 0) underruns_.fetch_add(1);
        }

        BYTE* out = nullptr;
        if (SUCCEEDED(render->GetBuffer(framesToFill, &out))) {
            std::memcpy(out, chunk.data(), sampleCount * sizeof(float));
            render->ReleaseBuffer(framesToFill, 0);
        }
    }

    cleanup();
}
