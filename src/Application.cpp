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

    // Best-effort NVIDIA SR init. Renderer will simply not call through if
    // the upscaler isn't Ready / Enabled.
    upscaler_.Initialize(renderer_.Device(), renderer_.Context());
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
    renderer_.SetPresentMode(settings_.presentMode == 1
        ? Renderer::PresentMode::Tearing
        : Renderer::PresentMode::VSync);
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
    upscaler_.SetEnabled(settings_.nvsrEnabled);
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
    settings_.presentMode     = (renderer_.GetPresentMode() == Renderer::PresentMode::Tearing) ? 1 : 0;
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

void Application::SetUncapped(bool on)
{
    renderer_.SetPresentMode(on ? Renderer::PresentMode::Tearing
                                : Renderer::PresentMode::VSync);
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

void Application::SetUpscalerEnabled(bool on)
{
    upscaler_.SetEnabled(on);
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
    upscaler_.Shutdown();
    upscaler_.Initialize(renderer_.Device(), renderer_.Context());
}

int Application::Run()
{
    while (window_.PumpMessages())
    {
        // Handle resizes.
        if (window_.ConsumeResized())
            renderer_.Resize(window_.Width(), window_.Height());

        // Hot-reload device lists every 2s.
        const uint64_t now = GetTickCount64();
        if (now - lastEnumTickMs_ > 2000) {
            lastEnumTickMs_ = now;
            // Light-weight refresh — safe to call any time.
            auto newCams = VideoCapture::EnumerateDevices();
            auto newMics = AudioEngine::EnumerateCaptureDevices();
            auto newOuts = AudioEngine::EnumerateRenderDevices();
            if (newCams.size() != cameras_.size()) cameras_ = std::move(newCams);
            if (newMics.size() != mics_.size())    mics_    = std::move(newMics);
            if (newOuts.size() != speakers_.size()) speakers_ = std::move(newOuts);
            if (selCamera_  >= (int)cameras_.size())  selCamera_  = cameras_.empty()  ? -1 : 0;
            if (selMic_     >= (int)mics_.size())     selMic_     = mics_.empty()     ? -1 : 0;
            if (selSpeaker_ >= (int)speakers_.size()) selSpeaker_ = -1;
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
}

void Application::RefreshMicList()
{
    mics_ = AudioEngine::EnumerateCaptureDevices();
    if (selMic_ >= (int)mics_.size())
        selMic_ = mics_.empty() ? -1 : 0;
}

void Application::RefreshSpeakerList()
{
    speakers_ = AudioEngine::EnumerateRenderDevices();
    if (selSpeaker_ >= (int)speakers_.size())
        selSpeaker_ = -1;
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
        // first NV12 frame lands.
        renderer_.SetVideoColorSpace(video_.ColorMatrix(), video_.FullRange());
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
    running_ = false;
    captureStartMs_ = 0;
}
