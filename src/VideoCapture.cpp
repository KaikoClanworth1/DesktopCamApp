#include "VideoCapture.h"

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <Mferror.h>
#include <mfobjects.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

template <class T> using CPtr = ComPtr<T>;

namespace {

struct MFGlobalInit
{
    MFGlobalInit()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION);
    }
    ~MFGlobalInit()
    {
        MFShutdown();
        CoUninitialize();
    }
};

MFGlobalInit& EnsureMFInit()
{
    static MFGlobalInit s;
    return s;
}

const char* FourccName(uint32_t fourcc)
{
    switch (fourcc) {
        case 0x3231564E: return "NV12";
        case 0x32595559: return "YUY2";
        case 0x30323449: return "I420";
        case 0x47504A4D: return "MJPG";
        case 0x31435657: return "WVC1";
        case 0x34363248: return "H264";
        case 0x59565955: return "UYVY";
        case 20:         return "RGB32";
        case 22:         return "RGB24";
        default:         return "?";
    }
}

// A short tag for common resolutions so a 40-entry combo stays readable.
const char* ResolutionTag(uint32_t w, uint32_t h)
{
    if (w >= 3800 && h >= 2100) return "  (4K)";
    if (w >= 2500 && h >= 1400) return "  (1440p)";
    if (w >= 1900 && h >= 1000) return "  (1080p)";
    if (w >= 1200 && h >=  700) return "  (720p)";
    return "";
}

// Preference tier for the pixel format the device delivers. NV12 is what
// every modern capture card streams natively and what our shader consumes
// with no conversion; MJPG needs a software decode and is a last resort.
// MF video subtype GUIDs are "<fourcc>-0000-0010-8000-00AA00389B71", so the
// first 32 bits identify the format on their own.
uint64_t SubtypeTier(uint32_t fourcc)
{
    switch (fourcc) {
        case 0x3231564E: return 5; // NV12
        case 0x32595559: return 4; // YUY2
        case 0x59565955: return 3; // UYVY
        case 0x30323449: return 2; // I420
        case 0x47504A4D: return 1; // MJPG
        default:         return 0;
    }
}

uint64_t SubtypeTier(const GUID& sub) { return SubtypeTier((uint32_t)sub.Data1); }

// Rank a mode so the best one has the highest value. Both orderings keep a
// >=24 fps mode ahead of anything slower, so Auto never lands on a 4K@10
// "photo" mode just because it has the most pixels.
uint64_t ScoreMode(uint32_t w, uint32_t h, uint32_t fps, uint64_t subTier, ModePreference pref)
{
    const uint64_t area     = (uint64_t)w * (uint64_t)h;
    const uint64_t usable   = (fps >= 24) ? 1ULL : 0ULL;
    const uint64_t fpsClamp = (fps > 0xFFFF) ? 0xFFFF : fps;

    if (pref == ModePreference::Framerate)
        return (usable << 63) | (fpsClamp << 40) | (area << 8) | subTier;

    // Resolution first, then framerate, then pixel format.
    return (usable << 63) | (area << 24) | (fpsClamp << 8) | subTier;
}

} // namespace

// ---- IMFSourceReaderCallback impl --------------------------------------------
class VideoCapture::ReaderCallback
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFSourceReaderCallback>
{
public:
    explicit ReaderCallback(VideoCapture* p) : parent_(p) {}
    void Detach() { parent_ = nullptr; }

    STDMETHODIMP OnReadSample(HRESULT hr,
                              DWORD /*streamIndex*/,
                              DWORD streamFlags,
                              LONGLONG /*timestamp*/,
                              IMFSample* sample) override
    {
        VideoCapture* p = parent_;
        if (!p || !p->running_.load()) return S_OK;

        if (FAILED(hr)) {
            p->OnReadError(hr);
            return S_OK;
        }

        if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            p->running_.store(false);
            return S_OK;
        }

        if (sample) p->OnSample(sample);
        p->RequestNextSample();
        return S_OK;
    }

    STDMETHODIMP OnFlush(DWORD /*streamIndex*/) override
    {
        if (parent_ && parent_->flushDoneEvent_)
            SetEvent(parent_->flushDoneEvent_);
        return S_OK;
    }

    STDMETHODIMP OnEvent(DWORD /*streamIndex*/, IMFMediaEvent* /*e*/) override
    {
        return S_OK;
    }

private:
    VideoCapture* parent_;
};

// ---- Enumerate devices -------------------------------------------------------
std::vector<VideoDevice> VideoCapture::EnumerateDevices()
{
    EnsureMFInit();

    std::vector<VideoDevice> out;

    CPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(attrs.GetAddressOf(), 1))) return out;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &activates, &count))) return out;

    for (UINT32 i = 0; i < count; ++i) {
        VideoDevice d;
        WCHAR* name = nullptr; UINT32 nameLen = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen)) && name) {
            d.friendlyName = name;
            CoTaskMemFree(name);
        }
        WCHAR* sym = nullptr; UINT32 symLen = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &sym, &symLen)) && sym) {
            d.symbolicLink = sym;
            CoTaskMemFree(sym);
        }
        if (!d.symbolicLink.empty()) out.push_back(std::move(d));
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return out;
}

// ---- Lifecycle ---------------------------------------------------------------
VideoCapture::VideoCapture() { EnsureMFInit(); }

VideoCapture::~VideoCapture() { Stop(); }

void VideoCapture::SetError(const std::wstring& msg)
{
    std::lock_guard<std::mutex> lk(errorMutex_);
    lastError_ = msg;
}

std::wstring VideoCapture::LastError()
{
    std::lock_guard<std::mutex> lk(errorMutex_);
    return lastError_;
}

std::string VideoCapture::NegotiatedSummary() const
{
    if (width_ <= 0 || height_ <= 0) return "Idle";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%dx%d @ %.0f  %s  %s",
                  width_, height_, negotiatedFps_,
                  format_ == CaptureFormat::NV12 ? "NV12" : "BGRA",
                  gpuPath_ ? "GPU" : "CPU");
    return buf;
}

// Build a CameraMode descriptor from an IMFMediaType (shared with the
// auto-scoring path in Start()).
static CameraMode ModeFromMediaType(IMFMediaType* t)
{
    CameraMode m;
    UINT32 w = 0, h = 0;
    MFGetAttributeSize (t, MF_MT_FRAME_SIZE, &w, &h);
    m.width = w; m.height = h;

    UINT32 nN = 0, nD = 0;
    MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &nN, &nD);
    m.fpsN = nN; m.fpsD = nD ? nD : 1;

    GUID sub{};
    t->GetGUID(MF_MT_SUBTYPE, &sub);
    m.subtypeFourcc = (uint32_t)sub.Data1;

    // Round rather than truncate: 60000/1001 is 60 fps, not 59.
    const uint32_t fps = m.fpsRounded();

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%ux%u @ %u fps  %s%s",
                  m.width, m.height, fps, FourccName(m.subtypeFourcc),
                  ResolutionTag(m.width, m.height));
    m.label = buf;
    std::snprintf(buf, sizeof(buf), "%ux%u_%u_%08X",
                  m.width, m.height, fps, (unsigned)m.subtypeFourcc);
    m.key = buf;
    return m;
}

std::vector<CameraMode> VideoCapture::EnumerateModes(const std::wstring& symbolicLink)
{
    EnsureMFInit();
    std::vector<CameraMode> out;
    if (symbolicLink.empty()) return out;

    CPtr<IMFAttributes> devAttrs;
    if (FAILED(MFCreateAttributes(devAttrs.GetAddressOf(), 2))) return out;
    devAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    devAttrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                        symbolicLink.c_str());

    CPtr<IMFMediaSource> source;
    if (FAILED(MFCreateDeviceSource(devAttrs.Get(), source.GetAddressOf()))) return out;

    CPtr<IMFPresentationDescriptor> pd;
    CPtr<IMFStreamDescriptor>       sd;
    CPtr<IMFMediaTypeHandler>       mth;
    BOOL sel = FALSE;
    if (SUCCEEDED(source->CreatePresentationDescriptor(pd.GetAddressOf())) &&
        SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &sel, sd.GetAddressOf())) &&
        SUCCEEDED(sd->GetMediaTypeHandler(mth.GetAddressOf())))
    {
        DWORD n = 0;
        mth->GetMediaTypeCount(&n);
        out.reserve(n);
        for (DWORD i = 0; i < n; ++i) {
            CPtr<IMFMediaType> t;
            if (SUCCEEDED(mth->GetMediaTypeByIndex(i, t.GetAddressOf())) && t) {
                CameraMode m = ModeFromMediaType(t.Get());
                if (m.valid()) out.push_back(std::move(m));
            }
        }
    }
    source->Shutdown();

    // Cards that advertise 4K normally list 60+ entries, many of them exact
    // duplicates (same size/fps/format at different numerator/denominator
    // spellings). Collapse them and put the biggest/fastest first so the
    // combo is usable.
    std::sort(out.begin(), out.end(), [](const CameraMode& a, const CameraMode& b) {
        if (a.width  != b.width)  return a.width  > b.width;
        if (a.height != b.height) return a.height > b.height;
        const uint32_t fa = a.fpsRounded(), fb = b.fpsRounded();
        if (fa != fb) return fa > fb;
        return SubtypeTier(a.subtypeFourcc) > SubtypeTier(b.subtypeFourcc);
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const CameraMode& a, const CameraMode& b) { return a.key == b.key; }),
              out.end());
    return out;
}

bool VideoCapture::Start(ID3D11Device* device, const std::wstring& symbolicLink,
                         TextureCallback textureCb, BytesCallback bytesCb,
                         const StartOptions& options)
{
    Stop();
    if (symbolicLink.empty() || !device) {
        SetError(L"Invalid device");
        return false;
    }
    texCb_   = std::move(textureCb);
    bytesCb_ = std::move(bytesCb);
    gpuPath_ = true; // we'll flip to false if we fall through to the CPU reader
    format_  = CaptureFormat::BGRA;
    negotiatedFps_ = 0.0f;

    const CameraMode* preferred = options.preferred;

    if (!flushDoneEvent_)
        flushDoneEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // Activate media source for the chosen camera.
    CPtr<IMFAttributes> devAttrs;
    if (FAILED(MFCreateAttributes(devAttrs.GetAddressOf(), 2))) {
        SetError(L"MFCreateAttributes (source) failed");
        return false;
    }
    devAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    devAttrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, symbolicLink.c_str());

    HRESULT hr = MFCreateDeviceSource(devAttrs.Get(), source_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SetError(L"MFCreateDeviceSource failed");
        return false;
    }

    // ---- Pin the source's native type BEFORE creating the reader --------
    // Otherwise the reader picks the first native it can convert to our
    // output format (typically YUY2 1080p@25 — the first entry), regardless
    // of the framerate we put on the output type.
    UINT32 pinnedW = 0, pinnedH = 0, pinnedFpsN = 30, pinnedFpsD = 1;
    GUID   pinnedSubtype = MFVideoFormat_NV12;
    {
        CPtr<IMFPresentationDescriptor> pd;
        CPtr<IMFStreamDescriptor>       sd;
        CPtr<IMFMediaTypeHandler>       mth;
        BOOL selected = FALSE;
        if (SUCCEEDED(source_->CreatePresentationDescriptor(pd.GetAddressOf())) &&
            SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &selected, sd.GetAddressOf())) &&
            SUCCEEDED(sd->GetMediaTypeHandler(mth.GetAddressOf())))
        {
            DWORD count = 0;
            mth->GetMediaTypeCount(&count);

            wprintf(L"[mf] native types available (%lu):\n", (unsigned long)count);

            CPtr<IMFMediaType> chosen;
            uint64_t bestScore = 0;
            bool     lockedIn  = false;
            for (DWORD i = 0; i < count; ++i) {
                CPtr<IMFMediaType> t;
                if (FAILED(mth->GetMediaTypeByIndex(i, t.GetAddressOf()))) continue;

                UINT32 w = 0, h = 0, fpsN = 0, fpsD = 0;
                MFGetAttributeSize (t.Get(), MF_MT_FRAME_SIZE, &w, &h);
                MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &fpsN, &fpsD);
                const uint32_t fps = (fpsD > 0) ? (uint32_t)((double)fpsN / (double)fpsD + 0.5) : 0;

                GUID sub{};
                t->GetGUID(MF_MT_SUBTYPE, &sub);
                wprintf(L"  [%lu] %ux%u @ %u fps (num=%u den=%u) subtype=%08lX\n",
                        (unsigned long)i, w, h, fps, fpsN, fpsD, (unsigned long)sub.Data1);

                if (w == 0 || h == 0) continue;

                const uint64_t score = ScoreMode(w, h, fps, SubtypeTier(sub), options.preference);

                const bool matchesPreferred =
                    preferred && preferred->valid() &&
                    preferred->width  == w  && preferred->height == h &&
                    preferred->fpsRounded() == fps &&
                    preferred->subtypeFourcc == (uint32_t)sub.Data1;

                if (matchesPreferred && !lockedIn) {
                    // Exact match on a user-picked mode — lock it in.
                    chosen        = t;
                    pinnedW       = w;
                    pinnedH       = h;
                    pinnedFpsN    = fpsN ? fpsN : 30;
                    pinnedFpsD    = fpsD ? fpsD : 1;
                    pinnedSubtype = sub;
                    lockedIn      = true;
                } else if (!lockedIn && score > bestScore) {
                    bestScore     = score;
                    chosen        = t;
                    pinnedW       = w;
                    pinnedH       = h;
                    pinnedFpsN    = fpsN ? fpsN : 30;
                    pinnedFpsD    = fpsD ? fpsD : 1;
                    pinnedSubtype = sub;
                }
            }

            wprintf(L"[mf] chose %ux%u @ %u/%u fps subtype=%08lX (%s, prefer=%s)\n",
                    pinnedW, pinnedH, pinnedFpsN, pinnedFpsD,
                    (unsigned long)pinnedSubtype.Data1,
                    lockedIn ? L"user-picked" : L"auto",
                    options.preference == ModePreference::Framerate ? L"fps" : L"resolution");

            if (chosen) {
                HRESULT ph = mth->SetCurrentMediaType(chosen.Get());
                if (FAILED(ph)) {
                    wprintf(L"[mf] source SetCurrentMediaType failed: 0x%08lX — reader will pick its own\n",
                            (unsigned long)ph);
                } else {
                    wprintf(L"[mf] pinned native type on source\n");
                }
            }
        }
    }

    // Build an MF DXGI device manager wrapping the renderer's ID3D11Device.
    // This is what lets the pipeline keep frames on the GPU and hand us D3D
    // textures we can CopySubresourceRegion out of — zero CPU round-trip.
    // At 4K144 the CPU path would need ~4.8 GB/s over PCIe, so this is the
    // only path that can actually sustain it.
    CPtr<IMFDXGIDeviceManager> dxgiMgr;
    UINT resetToken = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&resetToken, dxgiMgr.GetAddressOf()))) {
        SetError(L"MFCreateDXGIDeviceManager failed");
        return false;
    }
    if (FAILED(dxgiMgr->ResetDevice(device, resetToken))) {
        SetError(L"DXGI manager ResetDevice failed");
        return false;
    }

    // Source reader attributes — async callback, D3D manager, low latency.
    CPtr<IMFAttributes> readerAttrs;
    if (FAILED(MFCreateAttributes(readerAttrs.GetAddressOf(), 8))) {
        SetError(L"MFCreateAttributes (reader) failed");
        return false;
    }
    callback_ = Microsoft::WRL::Make<ReaderCallback>(this);
    readerAttrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get());
    readerAttrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgiMgr.Get());
    // ADVANCED_VIDEO_PROCESSING and VIDEO_PROCESSING are mutually exclusive
    // — only ADVANCED works with a D3D manager.
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    readerAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    // Ask MF's allocator for textures we can bind directly — saves the
    // driver a shadow copy when we CopySubresourceRegion out of them.
    readerAttrs->SetUINT32(MF_SA_D3D11_BINDFLAGS,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);

    hr = MFCreateSourceReaderFromMediaSource(source_.Get(), readerAttrs.Get(), reader_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        // Retry with plain CPU pipeline if the hardware/driver can't do the
        // GPU-accelerated advanced video processor. Slower but always works.
        CPtr<IMFAttributes> cpuAttrs;
        MFCreateAttributes(cpuAttrs.GetAddressOf(), 3);
        cpuAttrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get());
        cpuAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        cpuAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        HRESULT hr2 = MFCreateSourceReaderFromMediaSource(source_.Get(), cpuAttrs.Get(), reader_.ReleaseAndGetAddressOf());
        if (FAILED(hr2)) {
            wchar_t buf[128];
            swprintf_s(buf, L"MFCreateSourceReaderFromMediaSource failed: 0x%08lX / 0x%08lX", (unsigned long)hr, (unsigned long)hr2);
            SetError(buf);
            source_->Shutdown();
            source_.Reset();
            callback_.Reset();
            return false;
        }
        gpuPath_ = false;
    }

    // The source's native type is already pinned. Ask the reader for an
    // output format at the same dimensions/fps.
    //
    // NV12 first when we're on the GPU path: for a card already streaming
    // NV12 that is a straight pass-through (no video-processor pass at all),
    // and our pixel shader does the YUV->RGB. RGB32 would burn a full 4K
    // conversion per frame — the thing that makes 4K60/1080p144 stutter.
    UINT32 frameW = pinnedW ? pinnedW : 1280;
    UINT32 frameH = pinnedH ? pinnedH : 720;

    auto makeOutputType = [&](const GUID& subtype, IMFMediaType** out) -> HRESULT {
        CPtr<IMFMediaType> t;
        HRESULT h2 = MFCreateMediaType(t.GetAddressOf());
        if (FAILED(h2)) return h2;
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE,    subtype);
        t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize (t.Get(), MF_MT_FRAME_SIZE, frameW, frameH);
        MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, pinnedFpsN, pinnedFpsD);
        *out = t.Detach();
        return S_OK;
    };

    hr = E_FAIL;
    if (gpuPath_ && options.allowNV12) {
        CPtr<IMFMediaType> nv12;
        if (SUCCEEDED(makeOutputType(MFVideoFormat_NV12, nv12.GetAddressOf()))) {
            hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nv12.Get());
            if (SUCCEEDED(hr)) {
                format_ = CaptureFormat::NV12;
                wprintf(L"[mf] output format = NV12 (zero-conversion path)\n");
            } else {
                wprintf(L"[mf] NV12 output rejected (0x%08lX) — falling back to RGB32\n",
                        (unsigned long)hr);
            }
        }
    }

    if (FAILED(hr)) {
        CPtr<IMFMediaType> rgbType;
        if (SUCCEEDED(makeOutputType(MFVideoFormat_RGB32, rgbType.GetAddressOf())))
            hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, rgbType.Get());
        if (FAILED(hr)) {
            SetError(L"SetCurrentMediaType(NV12/RGB32) failed");
            source_->Shutdown();
            source_.Reset();
            reader_.Reset();
            return false;
        }
        format_ = CaptureFormat::BGRA;
        wprintf(L"[mf] output format = RGB32\n");
    }

    // Re-read negotiated type for actual dimensions, framerate and the
    // colorimetry we need for the YUV->RGB shader constants.
    colorMatrix_ = (frameH >= 720) ? 1 : 0;  // BT.709 for HD, BT.601 for SD
    fullRange_   = false;

    CPtr<IMFMediaType> actual;
    reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, actual.GetAddressOf());
    if (actual) {
        UINT32 aw = 0, ah = 0;
        MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &aw, &ah);
        if (aw && ah) { frameW = aw; frameH = ah; }

        UINT32 fn = 0, fd = 0;
        if (SUCCEEDED(MFGetAttributeRatio(actual.Get(), MF_MT_FRAME_RATE, &fn, &fd)) && fd)
            negotiatedFps_ = (float)fn / (float)fd;

        UINT32 matrix = 0;
        if (SUCCEEDED(actual->GetUINT32(MF_MT_YUV_MATRIX, &matrix))) {
            // MFVideoTransferMatrix: 1 = BT709, 2 = BT601, 3 = SMPTE240M,
            // 4/5 = BT2020.
            if      (matrix == MFVideoTransferMatrix_BT601) colorMatrix_ = 0;
            else if (matrix == MFVideoTransferMatrix_BT709) colorMatrix_ = 1;
            else if (matrix >= 4)                           colorMatrix_ = 2;
        }
        UINT32 range = 0;
        if (SUCCEEDED(actual->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range)))
            fullRange_ = (range == MFNominalRange_0_255);
    }
    if (negotiatedFps_ <= 0.0f && pinnedFpsD)
        negotiatedFps_ = (float)pinnedFpsN / (float)pinnedFpsD;

    width_  = (int)frameW;
    height_ = (int)frameH;

    wprintf(L"[mf] path=%s  negotiated=%dx%d @ %.2f fps  matrix=%d fullRange=%d\n",
                 gpuPath_ ? L"GPU/D3D-manager" : L"CPU/bytes",
                 width_, height_, negotiatedFps_, colorMatrix_, (int)fullRange_);

    // Query the source directly to see which native input type MF picked —
    // this tells us whether we're coming in through NV12 (fast), MJPG
    // (software decode, slow), etc.
    {
        CPtr<IMFPresentationDescriptor> pd;
        if (SUCCEEDED(source_->CreatePresentationDescriptor(pd.GetAddressOf()))) {
            BOOL selected = FALSE;
            CPtr<IMFStreamDescriptor> sd;
            if (SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &selected, sd.GetAddressOf()))) {
                CPtr<IMFMediaTypeHandler> mth;
                if (SUCCEEDED(sd->GetMediaTypeHandler(mth.GetAddressOf()))) {
                    CPtr<IMFMediaType> currentNative;
                    if (SUCCEEDED(mth->GetCurrentMediaType(currentNative.GetAddressOf()))) {
                        GUID sub{};
                        currentNative->GetGUID(MF_MT_SUBTYPE, &sub);
                        UINT32 nw = 0, nh = 0, nN = 0, nD = 0;
                        MFGetAttributeSize (currentNative.Get(), MF_MT_FRAME_SIZE, &nw, &nh);
                        MFGetAttributeRatio(currentNative.Get(), MF_MT_FRAME_RATE, &nN, &nD);
                        wprintf(L"[mf] native input in use: %ux%u @ %u/%u fps subtype=%08lX (NV12=0x3231564E MJPG=0x47504A4D YUY2=0x32595559)\n",
                                nw, nh, nN, nD, (unsigned long)sub.Data1);
                    }
                }
            }
        }
    }

    running_.store(true);
    framesSinceTick_.store(0);
    lastFpsTickMs_.store(GetTickCount64());
    captureFps_.store(0.0f);

    // Kick off the async pipeline.
    RequestNextSample();
    return true;
}

void VideoCapture::Stop()
{
    if (!running_.load() && !reader_ && !source_) return;
    running_.store(false);

    if (reader_ && flushDoneEvent_) {
        ResetEvent(flushDoneEvent_);
        if (SUCCEEDED(reader_->Flush(MF_SOURCE_READER_ALL_STREAMS)))
            WaitForSingleObject(flushDoneEvent_, 2000);
    }

    if (source_) {
        source_->Shutdown();
        source_.Reset();
    }
    reader_.Reset();

    if (callback_) {
        callback_->Detach();
        callback_.Reset();
    }

    if (flushDoneEvent_) {
        CloseHandle(flushDoneEvent_);
        flushDoneEvent_ = nullptr;
    }

    texCb_   = nullptr;
    bytesCb_ = nullptr;
    width_ = height_ = 0;
    negotiatedFps_ = 0.0f;
    captureFps_.store(0.0f);
}

void VideoCapture::RequestNextSample()
{
    if (!running_.load() || !reader_) return;
    HRESULT hr = reader_->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) {
        SetError(L"ReadSample (async) request failed");
        running_.store(false);
    }
}

void VideoCapture::OnReadError(HRESULT /*hr*/)
{
    SetError(L"Async read sample returned an error");
    running_.store(false);
}

void VideoCapture::UpdateCaptureFps()
{
    framesSinceTick_.fetch_add(1);
    const uint64_t now = GetTickCount64();
    uint64_t last = lastFpsTickMs_.load();
    if (now - last >= 1000) {
        if (lastFpsTickMs_.compare_exchange_strong(last, now)) {
            const uint32_t frames = framesSinceTick_.exchange(0);
            const float fps = (float)frames * 1000.0f / (float)(now - last);
            captureFps_.store(fps);
            wprintf(L"[mf] capture fps = %.2f (%u frames in %llu ms)\n",
                    fps, frames, (unsigned long long)(now - last));
        }
    }
}

void VideoCapture::OnSample(IMFSample* sample)
{
    if (!sample) return;

    UpdateCaptureFps();

    static bool firstSampleLogged = false;

    CPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->GetBufferByIndex(0, buf.GetAddressOf()))) return;

    // Fast path: GPU texture via MF DXGI buffer.
    if (gpuPath_ && texCb_) {
        CPtr<IMFDXGIBuffer> dxgiBuf;
        if (SUCCEEDED(buf.As(&dxgiBuf))) {
            CPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(dxgiBuf->GetResource(IID_PPV_ARGS(tex.GetAddressOf())))) {
                UINT subIdx = 0;
                dxgiBuf->GetSubresourceIndex(&subIdx);
                if (!firstSampleLogged) {
                    D3D11_TEXTURE2D_DESC d{};
                    tex->GetDesc(&d);
                    wprintf(L"[mf] first GPU sample: %ux%u fmt=%d arr=%u sub=%u\n",
                                 d.Width, d.Height, (int)d.Format, d.ArraySize, subIdx);
                    firstSampleLogged = true;
                }
                texCb_(tex.Get(), subIdx, width_, height_);
                return;
            }
        }
        // If we thought we were on the GPU path but didn't get a DXGI buffer,
        // fall through to the CPU path for this sample.
    }

    if (!firstSampleLogged) {
        wprintf(L"[mf] first CPU sample (no DXGI buffer path)\n");
        firstSampleLogged = true;
    }

    // CPU path: IMF2DBuffer is preferred (gives us the proper pitch), but
    // older drivers may only expose IMFMediaBuffer::Lock.
    // Only ever reached with RGB32 output — the NV12 request above is gated
    // on gpuPath_.
    if (!bytesCb_ || format_ != CaptureFormat::BGRA) return;

    CPtr<IMF2DBuffer> buf2d;
    if (SUCCEEDED(buf.As(&buf2d))) {
        BYTE* row0 = nullptr;
        LONG  pitch = 0;
        if (SUCCEEDED(buf2d->Lock2D(&row0, &pitch))) {
            bytesCb_(width_, height_, (int)pitch, row0);
            buf2d->Unlock2D();
            return;
        }
    }

    BYTE* data = nullptr;
    DWORD maxLen = 0, curLen = 0;
    if (SUCCEEDED(buf->Lock(&data, &maxLen, &curLen)) && data) {
        const int assumedStride = width_ * 4; // RGB32, top-down
        bytesCb_(width_, height_, assumedStride, data);
        buf->Unlock();
    }
}
