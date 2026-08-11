#pragma once

#include <Windows.h>
#include <vector>
#include <string>
#include <atomic>
#include <cstdint>

#include "Window.h"
#include "Renderer.h"
#include "VideoCapture.h"
#include "AudioEngine.h"
#include "UI.h"
#include "Settings.h"
#include "UpscalerNV.h"
#include "Updater.h"

class Application
{
public:
    Application();
    ~Application();

    bool Initialize(const wchar_t* title, int width, int height);
    int  Run();
    void Shutdown();

    // ---- Accessors used by the UI ----
    const std::vector<VideoDevice>& CameraDevices()     const { return cameras_;  }
    const std::vector<AudioDevice>& MicrophoneDevices() const { return mics_;     }
    const std::vector<AudioDevice>& SpeakerDevices()    const { return speakers_; }
    const std::vector<CameraMode>&  CameraModes()       const { return cameraModes_; }
    int  SelectedCameraModeIndex() const { return selCameraMode_; }
    void SetSelectedCameraModeIndex(int i);

    int  SelectedCameraIndex()   const { return selCamera_;   }
    int  SelectedMicIndex()      const { return selMic_;      }
    int  SelectedSpeakerIndex()  const { return selSpeaker_;  }

    void SetSelectedCameraIndex(int i);
    void SetSelectedMicIndex(int i);
    void SetSelectedSpeakerIndex(int i);

    void RefreshCameraList();
    void RefreshMicList();
    void RefreshSpeakerList();

    float MicVolumePercent() const { return micVolumePct_; }
    void  SetMicVolumePercent(float pct);

    float MicPeak() const { return audio_.CurrentPeak(); }

    bool GetAudioSwapLR() const { return audio_.GetSwapLR(); }
    void SetAudioSwapLR(bool on);

    bool IsRunning() const { return running_; }
    void StartCapture();
    void StopCapture();

    // ---- Capture mode / pacing ----
    ModePreference AutoModePreference() const { return (ModePreference)settings_.modePreference; }
    void           SetAutoModePreference(ModePreference p);

    bool IsTearingSupported() const { return renderer_.TearingSupported(); }
    bool IsUncapped()         const { return renderer_.GetPresentMode() == Renderer::PresentMode::Tearing; }
    void SetUncapped(bool on);

    int  FpsLimit() const { return renderer_.GetFpsLimit(); }
    void SetFpsLimit(int fps);

    bool UseNV12() const { return settings_.useNV12; }
    void SetUseNV12(bool on);

    int  MonitorRefreshHz() const { return renderer_.MonitorRefreshHz(); }

    float    Fps()             const { return renderer_.Fps(); }
    float    CaptureFps()      const { return video_.CaptureFps(); }
    float    NegotiatedFps()   const { return video_.NegotiatedFps(); }
    std::string CaptureSummary() const { return video_.NegotiatedSummary(); }
    void     VideoSize(int& w, int& h) const { w = video_.Width(); h = video_.Height(); }
    uint64_t CaptureElapsedMs() const { return running_ && captureStartMs_ ? (GetTickCount64() - captureStartMs_) : 0; }

    std::wstring VideoError() { return video_.LastError(); }
    std::wstring AudioError() { return audio_.LastError(); }

    void ToggleFullscreen() { window_.ToggleFullscreen(); }

    bool IsUIHidden() const { return uiHidden_; }
    void SetUIHidden(bool hidden);

    bool IsBorderless() const { return window_.IsBorderless(); }
    void SetBorderless(bool b);

    bool IsPerformanceMode() const { return performanceMode_; }
    void SetPerformanceMode(bool p);

    // NVIDIA Super Resolution (experimental).
    UpscalerNV& Upscaler() { return upscaler_; }
    const UpscalerNV& Upscaler() const { return upscaler_; }
    void SetUpscalerEnabled(bool on);
    void SetUpscalerScale(float s);
    void SetUpscalerMode(int m);
    void RetryUpscalerInit();

    bool IsDebugConsoleOn() const;
    void SetDebugConsoleOn(bool on);

    // ---- Updates ----
    Updater&    GetUpdater() { return updater_; }
    bool        CheckUpdatesOnStart() const { return settings_.updateCheckOnStart; }
    void        SetCheckUpdatesOnStart(bool on);
    void        SkipCurrentUpdate();
    bool        IsUpdateSkipped(const std::string& version) const
                { return !version.empty() && settings_.updateSkipVersion == version; }
    void        InstallUpdateAndRestart();

    void Quit();

    // Window title — Discord / OBS / Window Capture use this to identify
    // the app. Empty string restores the built-in default.
    std::wstring CustomTitle() const { return settings_.customTitle; }
    void         SetCustomTitle(const std::wstring& title);

private:
    Window        window_;
    Renderer      renderer_;
    VideoCapture  video_;
    AudioEngine   audio_;
    UI            ui_;
    UpscalerNV    upscaler_;
    Updater       updater_;

    std::vector<VideoDevice> cameras_;
    std::vector<AudioDevice> mics_;
    std::vector<AudioDevice> speakers_;
    std::vector<CameraMode>  cameraModes_;   // modes for the currently-selected camera

    int   selCamera_     = -1;
    int   selCameraMode_ = -1;              // -1 = Auto
    int   selMic_        = -1;
    int   selSpeaker_    = -1;

    void  RefreshCameraModes();

    float micVolumePct_ = 100.0f;
    bool  running_      = false;
    bool  uiHidden_     = false;
    bool  performanceMode_ = false;

    uint64_t lastEnumTickMs_ = 0;
    uint64_t captureStartMs_ = 0;

    Settings     settings_;
    std::wstring defaultTitle_;
    bool     settingsDirty_       = false;
    uint64_t lastSettingsSaveMs_  = 0;
    void     MarkSettingsDirty()  { settingsDirty_ = true; }
    void     FlushSettingsMaybe();
    void     CaptureCurrentSelectionIntoSettings();
    void     ApplyLoadedSettings();
};
