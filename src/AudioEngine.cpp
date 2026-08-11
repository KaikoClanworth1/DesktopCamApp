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

    // 100 ms ring buffer is plenty for passthrough.
    capacity_ = (size_t)sampleRate_ * channels_ / 10;
    ring_.assign(capacity_, 0.0f);
    writeIdx_ = 0;
    readIdx_  = 0;
    currentGain_ = targetGain_.load();

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
    ring_.clear();
    capacity_ = 0;
    writeIdx_ = 0;
    readIdx_  = 0;
    peak_.store(0.0f);
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

// ---- Ring buffer -------------------------------------------------------------
size_t AudioEngine::RingWrite(const float* src, size_t n)
{
    if (capacity_ == 0) return 0;
    const size_t r = readIdx_.load(std::memory_order_acquire);
    const size_t w = writeIdx_.load(std::memory_order_relaxed);
    const size_t free = (r + capacity_ - w - 1) % capacity_;
    const size_t toWrite = std::min(n, free);
    for (size_t i = 0; i < toWrite; ++i)
        ring_[(w + i) % capacity_] = src[i];
    writeIdx_.store((w + toWrite) % capacity_, std::memory_order_release);
    return toWrite;
}

size_t AudioEngine::RingRead(float* dst, size_t n)
{
    if (capacity_ == 0) return 0;
    const size_t w = writeIdx_.load(std::memory_order_acquire);
    const size_t r = readIdx_.load(std::memory_order_relaxed);
    const size_t avail = (w + capacity_ - r) % capacity_;
    const size_t toRead = std::min(n, avail);
    for (size_t i = 0; i < toRead; ++i)
        dst[i] = ring_[(r + i) % capacity_];
    readIdx_.store((r + toRead) % capacity_, std::memory_order_release);
    return toRead;
}

void AudioEngine::RingClear()
{
    writeIdx_.store(0);
    readIdx_.store(0);
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
    if (FAILED(dev->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr, (void**)client.GetAddressOf()))) {
        SetError(L"Activate(capture AudioClient) failed");
        running_.store(false);
        cleanup();
        return;
    }

    WAVEFORMATEXTENSIBLE wfx;
    MakeTargetFormat(wfx, sampleRate_, channels_);

    const REFERENCE_TIME bufDuration = 200000; // 20 ms
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                      | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                      | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                    bufDuration, 0, (WAVEFORMATEX*)&wfx, nullptr);
    if (FAILED(hr)) {
        SetError(L"AudioClient::Initialize(capture) failed");
        running_.store(false);
        cleanup();
        return;
    }
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
            const float target = targetGain_.load();
            float gain = currentGain_;
            const float smooth = 0.002f;
            float peak = 0.0f;
            for (size_t i = 0; i < samples; ++i) {
                gain += (target - gain) * smooth;
                float v = work[i] * gain;
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                work[i] = v;
                const float a = v < 0 ? -v : v;
                if (a > peak) peak = a;
            }
            currentGain_ = gain;

            // Optional L/R swap for mics that report channels reversed.
            if (swapLR_.load() && channels_ == 2) {
                for (size_t i = 0; i + 1 < samples; i += 2)
                    std::swap(work[i], work[i + 1]);
            }

            // Update peak meter with a slight decay.
            float oldPeak = peak_.load();
            float newPeak = std::max(peak, oldPeak * 0.85f);
            peak_.store(newPeak);

            RingWrite(work.data(), samples);
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
    if (FAILED(dev->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr, (void**)client.GetAddressOf()))) {
        SetError(L"Activate(render AudioClient) failed");
        running_.store(false);
        cleanup();
        return;
    }

    WAVEFORMATEXTENSIBLE wfx;
    MakeTargetFormat(wfx, sampleRate_, channels_);

    const REFERENCE_TIME bufDuration = 300000; // 30 ms
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                      | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                      | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                    bufDuration, 0, (WAVEFORMATEX*)&wfx, nullptr);
    if (FAILED(hr)) {
        SetError(L"AudioClient::Initialize(render) failed");
        running_.store(false);
        cleanup();
        return;
    }
    client->SetEventHandle(evt);

    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);

    if (FAILED(client->GetService(IID_IAudioRenderClient_, (void**)render.GetAddressOf()))) {
        SetError(L"GetService(render) failed");
        running_.store(false);
        cleanup();
        return;
    }

    // Pre-fill with silence so the render pump starts smoothly.
    {
        BYTE* buf = nullptr;
        if (SUCCEEDED(render->GetBuffer(bufferFrames, &buf))) {
            std::memset(buf, 0, (size_t)bufferFrames * wfx.Format.nBlockAlign);
            render->ReleaseBuffer(bufferFrames, 0);
        }
    }

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
        if (framesToFill == 0) continue;

        const size_t sampleCount = (size_t)framesToFill * channels_;
        chunk.resize(sampleCount);

        const size_t got = RingRead(chunk.data(), sampleCount);
        if (got < sampleCount) {
            // Underrun — pad with silence.
            std::memset(chunk.data() + got, 0, (sampleCount - got) * sizeof(float));
        }

        BYTE* out = nullptr;
        if (SUCCEEDED(render->GetBuffer(framesToFill, &out))) {
            std::memcpy(out, chunk.data(), sampleCount * sizeof(float));
            render->ReleaseBuffer(framesToFill, 0);
        }
    }

    cleanup();
}
