#include "Application.h"
#include "DebugConsole.h"

Application::Application() = default;

Application::~Application() { Shutdown(); }

bool Application::Initialize(const wchar_t* title, int width, int height)
{
    defaultTitle_ = title ? title : L"Desktop Cam App";

    // Load saved settings first so we can create the window at the stored
    // size/position instead of the caller's default.
    settings_.Load();

    // Attach the debug console NOW (before anything else can log) so the
    // NVIDIA upscaler's DLL-search diagnostics are visible when we try to
    // initialize it below. Idempotent — ShowDebugConsole(true) is a no-op
    // if the console is already attached.
    if (settings_.debugConsole)
        ShowDebugConsole(true);

    const int startW = (settings_.windowW > 0) ? settings_.windowW : width;
    const int startH = (settings_.windowH > 0) ? settings_.windowH : height;

    if (!window_.Create(title, startW, startH))
        return false;

    // Restore saved window position (if any).
    if (settings_.windowX != (int)0x80000000 && settings_.windowY != (int)0x80000000) {
        SetWindowPos(window_.Hwnd(), nullptr, settings_.windowX, settings_.windowY,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    if (!renderer_.Initialize(window_.Hwnd(), startW, startH))
        return false;

    if (!ui_.Init(window_.Hwnd(), renderer_.Device(), renderer_.Context()))
        return false;

    // The NVIDIA Broadcast SDK is NOT loaded here. Initializing it pulls in
    // TensorRT (~210 MB of module footprint), the CUDA runtime and a CUDA
    // context — several hundred MB of RAM and VRAM — for a feature that is
    // off by default. EnsureUpscalerLoaded() brings it up the moment the
    // user turns AI upscaling on; ApplyLoadedSettings does it at startup for
    // users who left it on.
    renderer_.SetUpscaler(&upscaler_);

    // Route window messages through F1-handling first, then ImGui. When the
    // UI is "hidden" we keep the pipeline rendering (alpha=0) but swallow
    // input here so invisible widgets can't be clicked.
    window_.ExternalWndProc = [this](HWND hwnd, UINT m, WPARAM wp, LPARAM lp, bool& handled) -> LRESULT {
        if (m == WM_KEYDOWN && wp == VK_F1) {
            SetUIHidden(!uiHidden_);
            handled = true;
            return 0;
        }
        if (uiHidden_) { handled = false; return 0; }
        return ui_.WndProc(hwnd, m, wp, lp, handled);
    };

    RefreshCameraList();
    RefreshMicList();
    RefreshSpeakerList();

    ApplyLoadedSettings();

    if (settings_.updateCheckOnStart)
        updater_.CheckAsync();

    return true;
}

void Application::ApplyLoadedSettings()
{
    // Match saved device identifiers back to indices. Fall back to the
    // first available if the saved device is gone (unplugged).
    selCamera_ = cameras_.empty() ? -1 : 0;
    if (!settings_.cameraLink.empty()) {
        for (size_t i = 0; i < cameras_.size(); ++i) {
            if (cameras_[i].symbolicLink == settings_.cameraLink) {
                selCamera_ = (int)i;
                break;
            }
        }
    }
    RefreshCameraModes(); // fills cameraModes_ and resolves settings_.cameraModeKey

    selMic_ = mics_.empty() ? -1 : 0;
    if (!settings_.micId.empty()) {
        for (size_t i = 0; i < mics_.size(); ++i) {
            if (mics_[i].id == settings_.micId) { selMic_ = (int)i; break; }
        }
    }

    selSpeaker_ = -1;
    if (!settings_.speakerId.empty()) {
        for (size_t i = 0; i < speakers_.size(); ++i) {
            if (speakers_[i].id == settings_.speakerId) { selSpeaker_ = (int)i; break; }
        }
    }

    micVolumePct_ = settings_.micVolume;
    audio_.SetGain(micVolumePct_ / 100.0f);
    audio_.SetSwapLR(settings_.audioSwapLR);
    audio_.SetTargetLatencyMs(settings_.audioLatencyMs);
    audio_.SetLowLatencyMode(settings_.audioLowLatency);

    // Always start with the UI visible — F1 hides it for the rest of the
    // session but the state isn't remembered across launches.
    uiHidden_ = false;

    if (settings_.debugConsole)
        ShowDebugConsole(true);

    if (settings_.borderless)
        window_.SetBorderless(true);

    performanceMode_ = settings_.performanceMode;
    renderer_.SetFrameLatency(performanceMode_ ? 1 : 2);

    // Presentation pacing. Tearing mode silently degrades to V-Sync when the
    // GPU/DXGI can't do it.
    //
    // One-time repair: "Uncapped" with a limit at or below the display's
    // refresh rate is strictly worse than V-Sync — it throws away vblank
    // alignment to gain nothing, and the judder that produces was the whole
    // reason this setting existed. Move those users to Auto.
    if (settings_.presentMode == 1 && settings_.fpsLimit > 0) {
        const int hz = renderer_.MonitorRefreshHz();
        if (hz > 0 && settings_.fpsLimit <= hz) {
            wprintf(L"[dx] Uncapped @ %d fps on a %d Hz display buys nothing — switching to Auto\n",
                    settings_.fpsLimit, hz);
            settings_.presentMode = 2;
            MarkSettingsDirty();
        }
    }
    if (settings_.presentMode < 0 || settings_.presentMode > 2) settings_.presentMode = 2;
    renderer_.SetPresentMode((Renderer::PresentMode)settings_.presentMode);
    renderer_.SetFpsLimit(settings_.fpsLimit);

    if (window_.Hwnd()) {
        const wchar_t* t = settings_.customTitle.empty()
            ? defaultTitle_.c_str()
            : settings_.customTitle.c_str();
        SetWindowTextW(window_.Hwnd(), t);
    }

    // Apply saved upscaler settings to whatever upscaler state we could
    // initialize. If the SDK is missing the knobs still get stored — we
    // just don't do anything with them until the DLL shows up. Cap scale
    // at 2.0x to match the UI (1080p input can't produce 3x/4x without
    // hitting the SR model's output-size limit).
    float savedScale = settings_.nvsrScale;
    if (savedScale > 2.01f) savedScale = 2.0f;
    if (savedScale < 1.29f) savedScale = 1.3f;
    upscaler_.SetScale(savedScale);
    upscaler_.SetMode(settings_.nvsrMode);
    if (settings_.nvsrEnabled)
        SetUpscalerEnabled(true);   // loads the SDK on demand
}

namespace {

std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace

void Application::RebuildDeviceNameCache()
{
    cameraNames_.clear();
    cameraNames_.reserve(cameras_.size());
    for (size_t i = 0; i < cameras_.size(); ++i) {
        std::string n = ToUtf8(cameras_[i].friendlyName);
        cameraNames_.push_back(n.empty() ? ("Camera " + std::to_string(i)) : std::move(n));
    }

    micNames_.clear();
    micNames_.reserve(mics_.size());
    for (size_t i = 0; i < mics_.size(); ++i) {
        std::string n = ToUtf8(mics_[i].friendlyName);
        micNames_.push_back(n.empty() ? ("Mic " + std::to_string(i)) : std::move(n));
    }

    speakerNames_.clear();
    speakerNames_.reserve(speakers_.size());
    for (size_t i = 0; i < speakers_.size(); ++i) {
        std::string n = ToUtf8(speakers_[i].friendlyName);
        speakerNames_.push_back(n.empty() ? ("Output " + std::to_string(i)) : std::move(n));
    }
}

// Re-enumerate and adopt the result only when something actually changed.
// Selections are re-resolved by device identity, so unplugging one device no
// longer silently shifts the user onto a different one.
void Application::RefreshDeviceListsIfChanged()
{
    auto newCams = VideoCapture::EnumerateDevices();
    auto newMics = AudioEngine::EnumerateCaptureDevices();
    auto newOuts = AudioEngine::EnumerateRenderDevices();

    auto sameVideo = [](const std::vector<VideoDevice>& a, const std::vector<VideoDevice>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].symbolicLink != b[i].symbolicLink) return false;
        return true;
    };
    auto sameAudio = [](const std::vector<AudioDevice>& a, const std::vector<AudioDevice>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].id != b[i].id) return false;
        return true;
    };

    bool changed = false;

    if (!sameVideo(newCams, cameras_)) {
        const std::wstring keep = (selCamera_ >= 0 && selCamera_ < (int)cameras_.size())
            ? cameras_[selCamera_].symbolicLink : std::wstring();
        cameras_ = std::move(newCams);
        selCamera_ = cameras_.empty() ? -1 : 0;
        for (size_t i = 0; i < cameras_.size(); ++i)
            if (cameras_[i].symbolicLink == keep) { selCamera_ = (int)i; break; }
        changed = true;
    }

    if (!sameAudio(newMics, mics_)) {
        const std::wstring keep = (selMic_ >= 0 && selMic_ < (int)mics_.size())
            ? mics_[selMic_].id : std::wstring();
        mics_ = std::move(newMics);
        selMic_ = mics_.empty() ? -1 : 0;
        for (size_t i = 0; i < mics_.size(); ++i)
            if (mics_[i].id == keep) { selMic_ = (int)i; break; }
        changed = true;
    }

    if (!sameAudio(newOuts, speakers_)) {
        const std::wstring keep = (selSpeaker_ >= 0 && selSpeaker_ < (int)speakers_.size())
            ? speakers_[selSpeaker_].id : std::wstring();
        speakers_ = std::move(newOuts);
        selSpeaker_ = -1;   // -1 == system default
        if (!keep.empty()) {
            for (size_t i = 0; i < speakers_.size(); ++i)
                if (speakers_[i].id == keep) { selSpeaker_ = (int)i; break; }
        }
        changed = true;
    }

    if (changed) RebuildDeviceNameCache();
}

void Application::CaptureCurrentSelectionIntoSettings()
{
    settings_.cameraLink = (selCamera_ >= 0 && selCamera_ < (int)cameras_.size())
        ? cameras_[selCamera_].symbolicLink : std::wstring();
    settings_.cameraModeKey = (selCameraMode_ >= 0 && selCameraMode_ < (int)cameraModes_.size())
        ? cameraModes_[selCameraMode_].key : std::string();
    settings_.micId = (selMic_ >= 0 && selMic_ < (int)mics_.size())
        ? mics_[selMic_].id : std::wstring();
    settings_.speakerId = (selSpeaker_ >= 0 && selSpeaker_ < (int)speakers_.size())
        ? speakers_[selSpeaker_].id : std::wstring();
    settings_.micVolume    = micVolumePct_;
    settings_.audioLatencyMs  = audio_.GetTargetLatencyMs();
    settings_.audioLowLatency = audio_.GetLowLatencyMode();
    settings_.uiHidden     = uiHidden_;
    settings_.debugConsole = IsDebugConsoleShown();
    settings_.borderless      = window_.IsBorderless();
    settings_.performanceMode = performanceMode_;
    settings_.presentMode     = (int)renderer_.GetPresentMode();
    settings_.fpsLimit        = renderer_.GetFpsLimit();
    settings_.nvsrEnabled     = upscaler_.IsEnabled();
    settings_.nvsrScale       = upscaler_.GetScale();
    settings_.nvsrMode        = upscaler_.GetMode();
}

void Application::FlushSettingsMaybe()
{
    if (!settingsDirty_) return;
    const uint64_t now = GetTickCount64();
    if (now - lastSettingsSaveMs_ < 500) return;
    lastSettingsSaveMs_ = now;
    CaptureCurrentSelectionIntoSettings();
    settings_.Save();
    settingsDirty_ = false;
}

void Application::SetSelectedCameraIndex(int i)
{
    selCamera_ = i;
    RefreshCameraModes();
    MarkSettingsDirty();
}

void Application::SetSelectedCameraModeIndex(int i)
{
    selCameraMode_ = i;
    MarkSettingsDirty();
}

void Application::RefreshCameraModes()
{
    cameraModes_.clear();
    if (selCamera_ >= 0 && selCamera_ < (int)cameras_.size())
        cameraModes_ = VideoCapture::EnumerateModes(cameras_[selCamera_].symbolicLink);

    // Try to keep the previous selection if it exists in the new list
    // (match by key). Otherwise fall back to Auto.
    const std::string prevKey = settings_.cameraModeKey;
    selCameraMode_ = -1;
    if (!prevKey.empty()) {
        for (size_t i = 0; i < cameraModes_.size(); ++i) {
            if (cameraModes_[i].key == prevKey) { selCameraMode_ = (int)i; break; }
        }
    }
}
void Application::SetSelectedMicIndex(int i) { selMic_ = i; MarkSettingsDirty(); }
void Application::SetSelectedSpeakerIndex(int i) { selSpeaker_ = i; MarkSettingsDirty(); }

void Application::SetUIHidden(bool hidden)
{
    uiHidden_ = hidden;
    MarkSettingsDirty();
}

bool Application::IsDebugConsoleOn() const      { return IsDebugConsoleShown(); }
void Application::SetDebugConsoleOn(bool on)
{
    ShowDebugConsole(on);
    MarkSettingsDirty();
}

void Application::SetBorderless(bool b)
{
    window_.SetBorderless(b);
    MarkSettingsDirty();
}

void Application::SetPerformanceMode(bool p)
{
    performanceMode_ = p;
    renderer_.SetFrameLatency(p ? 1 : 3);
    MarkSettingsDirty();
}

void Application::SetAutoModePreference(ModePreference p)
{
    settings_.modePreference = (int)p;
    MarkSettingsDirty();
}

void Application::SetPresentModeIndex(int idx)
{
    if (idx < 0 || idx > 2) idx = 2;
    renderer_.SetPresentMode((Renderer::PresentMode)idx);
    settings_.presentMode = idx;
    MarkSettingsDirty();
}

// Resolves "as reported by the device" against the user's overrides and hands
// the result to the shader.
void Application::ApplyColorSettings()
{
    int  matrix = video_.ColorMatrix();
    bool full   = video_.FullRange();

    switch (settings_.colorMatrix) {
        case 1: matrix = 0; break;   // BT.601
        case 2: matrix = 1; break;   // BT.709
        case 3: matrix = 2; break;   // BT.2020
        default: break;              // as reported
    }
    if (settings_.colorRange == 1)      full = false;
    else if (settings_.colorRange == 2) full = true;

    renderer_.SetVideoColorSpace(matrix, full);
}

void Application::SetColorRangeSetting(int r)
{
    settings_.colorRange = (r < 0 || r > 2) ? 0 : r;
    ApplyColorSettings();
    MarkSettingsDirty();
}

void Application::SetColorMatrixSetting(int m)
{
    settings_.colorMatrix = (m < 0 || m > 3) ? 0 : m;
    ApplyColorSettings();
    MarkSettingsDirty();
}

void Application::SetFpsLimit(int fps)
{
    renderer_.SetFpsLimit(fps);
    MarkSettingsDirty();
}

void Application::SetUseNV12(bool on)
{
    settings_.useNV12 = on;
    MarkSettingsDirty();
}

void Application::SetCheckUpdatesOnStart(bool on)
{
    settings_.updateCheckOnStart = on;
    MarkSettingsDirty();
}

void Application::SkipCurrentUpdate()
{
    settings_.updateSkipVersion = updater_.Latest().version;
    MarkSettingsDirty();
}

void Application::InstallUpdateAndRestart()
{
    // Persist everything first — the helper script kills and replaces us.
    CaptureCurrentSelectionIntoSettings();
    settings_.Save();
    settingsDirty_ = false;

    if (updater_.InstallAndRestart()) {
        StopCapture();
        Quit();
    }
}

void Application::Quit()
{
    PostQuitMessage(0);
}

void Application::SetCustomTitle(const std::wstring& title)
{
    settings_.customTitle = title;
    MarkSettingsDirty();
    if (window_.Hwnd()) {
        const wchar_t* t = title.empty() ? defaultTitle_.c_str() : title.c_str();
        SetWindowTextW(window_.Hwnd(), t);
    }
}

bool Application::EnsureUpscalerLoaded()
{
    if (upscaler_.IsReady()) return true;
    return upscaler_.Initialize(renderer_.Device(), renderer_.Context());
}

void Application::SetUpscalerEnabled(bool on)
{
    if (on) {
        // Pay for the SDK only when it is actually wanted. If it can't load,
        // leave the toggle off so the UI reflects reality.
        if (!EnsureUpscalerLoaded()) {
            upscaler_.SetEnabled(false);
            settings_.nvsrEnabled = false;
            MarkSettingsDirty();
            return;
        }
        upscaler_.SetEnabled(true);
    } else {
        // Hand back the CUDA/TensorRT allocations and unload the DLLs rather
        // than sitting on hundreds of MB for a feature that is switched off.
        upscaler_.SetEnabled(false);
        upscaler_.Shutdown();
    }
    settings_.nvsrEnabled = upscaler_.IsEnabled();
    MarkSettingsDirty();
}

void Application::SetUpscalerScale(float s)
{
    upscaler_.SetScale(s);
    MarkSettingsDirty();
}

void Application::SetUpscalerMode(int m)
{
    upscaler_.SetMode(m);
    MarkSettingsDirty();
}

void Application::RetryUpscalerInit()
{
    upscalerProbeOk_ = false;
    upscaler_.Shutdown();
    const bool ok = upscaler_.Initialize(renderer_.Device(), renderer_.Context());

    if (ok && !upscaler_.IsEnabled()) {
        // Diagnostics only. Loading the SDK costs ~250 MB of VRAM, so hand it
        // straight back rather than holding it for a feature that is off; the
        // toggle will load it again for real.
        upscaler_.Shutdown();
        upscalerProbeOk_ = true;
    }
    if (!upscaler_.IsReady()) upscaler_.SetEnabled(false);
}

int Application::Run()
{
    while (window_.PumpMessages())
    {
        // Handle resizes.
        if (window_.ConsumeResized())
            renderer_.Resize(window_.Width(), window_.Height());

        // Device lists refresh when Windows reports a device-tree change,
        // with a slow safety net. The old code re-enumerated every 2 s from
        // this loop: MFEnumDeviceSources plus two COM endpoint enumerations
        // costs several milliseconds — a dropped frame at 144 Hz, every two
        // seconds — and the freshly allocated lists were thrown away unless
        // the device *count* happened to change.
        const uint64_t now = GetTickCount64();
        if (window_.ConsumeDeviceChanged() || now - lastEnumTickMs_ > 30000) {
            lastEnumTickMs_ = now;
            RefreshDeviceListsIfChanged();
        }

        // Gate on DWM's waitable so we only produce a frame when the swap
        // chain is actually ready for one — this is what holds overall
        // window latency down to ~1 composition interval.
        renderer_.WaitForFrame();

        // Always run the ImGui pipeline each frame, even when "hidden" — it
        // keeps the GPU clocked up with a consistent per-frame workload so
        // Present(1, 0) lands on every vblank. Without it, skipping the UI
        // work lets the GPU power-gate and Present starts catching only
        // every other vblank (renders drop to 30 fps).
        ui_.NewFrame();
        ui_.Draw(*this);

        renderer_.BeginFrame();
        renderer_.DrawVideo();
        renderer_.DrawUI();
        renderer_.EndFrame();

        FlushSettingsMaybe();
    }
    return 0;
}

void Application::Shutdown()
{
    // Snapshot current window placement before tearing everything down.
    if (window_.Hwnd()) {
        WINDOWPLACEMENT wp{ sizeof(wp) };
        if (GetWindowPlacement(window_.Hwnd(), &wp)) {
            settings_.windowX = wp.rcNormalPosition.left;
            settings_.windowY = wp.rcNormalPosition.top;
            settings_.windowW = wp.rcNormalPosition.right  - wp.rcNormalPosition.left;
            settings_.windowH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        }
    }
    CaptureCurrentSelectionIntoSettings();
    settings_.Save();

    StopCapture();
    upscaler_.Shutdown();
    ui_.Shutdown();
    renderer_.Shutdown();
    window_.Destroy();
}

// ---- Device lists ------------------------------------------------------------
void Application::RefreshCameraList()
{
    cameras_ = VideoCapture::EnumerateDevices();
    if (selCamera_ >= (int)cameras_.size())
        selCamera_ = cameras_.empty() ? -1 : 0;
    RebuildDeviceNameCache();
}

void Application::RefreshMicList()
{
    mics_ = AudioEngine::EnumerateCaptureDevices();
    if (selMic_ >= (int)mics_.size())
        selMic_ = mics_.empty() ? -1 : 0;
    RebuildDeviceNameCache();
}

void Application::RefreshSpeakerList()
{
    speakers_ = AudioEngine::EnumerateRenderDevices();
    if (selSpeaker_ >= (int)speakers_.size())
        selSpeaker_ = -1;
    RebuildDeviceNameCache();
}

void Application::SetMicVolumePercent(float pct)
{
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 200.0f) pct = 200.0f;
    micVolumePct_ = pct;
    audio_.SetGain(pct / 100.0f);
    MarkSettingsDirty();
}

void Application::SetAudioSwapLR(bool on)
{
    audio_.SetSwapLR(on);
    settings_.audioSwapLR = on;
    MarkSettingsDirty();
}

void Application::SetAudioTargetLatencyMs(int ms)
{
    audio_.SetTargetLatencyMs(ms);
    settings_.audioLatencyMs = audio_.GetTargetLatencyMs();
    MarkSettingsDirty();
}

void Application::SetAudioLowLatencyMode(bool on)
{
    audio_.SetLowLatencyMode(on);
    settings_.audioLowLatency = on;
    MarkSettingsDirty();
}

// ---- Start/Stop --------------------------------------------------------------
void Application::StartCapture()
{
    if (running_) return;

    // Video — prefer the GPU-backed DXGI path, fall back to CPU bytes if
    // the driver can't do MF's advanced video processing.
    if (selCamera_ >= 0 && selCamera_ < (int)cameras_.size())
    {
        const auto& dev = cameras_[selCamera_];

        VideoCapture::StartOptions opts;
        if (selCameraMode_ >= 0 && selCameraMode_ < (int)cameraModes_.size())
            opts.preferred = &cameraModes_[selCameraMode_];
        opts.preference = (ModePreference)settings_.modePreference;
        // NV12 needs both the user's opt-in and a GPU that can sample it.
        opts.allowNV12  = settings_.useNV12 && renderer_.SupportsNV12();

        video_.Start(renderer_.Device(), dev.symbolicLink,
            [this](ID3D11Texture2D* tex, UINT sub, int w, int h) {
                renderer_.SubmitVideoTexture(tex, sub, w, h);
            },
            [this](int w, int h, int stride, const uint8_t* bgra) {
                renderer_.SubmitVideoFrame(w, h, stride, bgra);
            },
            opts);

        // The shader needs to know which YUV matrix MF negotiated before the
        // first NV12 frame lands, and Auto needs the capture rate to decide
        // whether tearing would buy anything.
        ApplyColorSettings();
        renderer_.SetSourceFps(video_.NegotiatedFps());
    }

    // Audio
    std::wstring capId, renderId;
    if (selMic_     >= 0 && selMic_     < (int)mics_.size())     capId     = mics_[selMic_].id;
    if (selSpeaker_ >= 0 && selSpeaker_ < (int)speakers_.size()) renderId  = speakers_[selSpeaker_].id;
    audio_.Start(capId, renderId);

    running_ = true;
    captureStartMs_ = GetTickCount64();
}

void Application::StopCapture()
{
    if (!running_) return;
    video_.Stop();
    audio_.Stop();
    renderer_.SetSourceFps(0.0f);
    // video_.Stop() has joined the MF pipeline, so nothing can submit a new
    // frame from here on and the frame buffers can go back to the driver.
    renderer_.ReleaseVideoResources();
    running_ = false;
    captureStartMs_ = 0;
}
