#pragma once

#include <string>

// Persisted user settings. Stored as a small UTF-8 INI file at
// %APPDATA%\DesktopCamApp\settings.ini — safe on any user account, roams
// with the user profile.
struct Settings
{
    // Device identities are stable across runs (unlike indices into the
    // enumeration), so we persist those and look up the index at load time.
    std::wstring cameraLink;   // MF symbolic link
    std::string  cameraModeKey;// stable descriptor (e.g. "1920x1080_60_3231564E")
    std::wstring micId;        // IMMDevice::GetId
    std::wstring speakerId;    // IMMDevice::GetId, empty == system default

    float        micVolume     = 100.0f;
    bool         audioSwapLR   = false;
    bool         uiHidden      = false;
    bool         debugConsole  = false;
    bool         borderless    = false;

    // true  = Performance Mode (max frame latency 1, lower lag)
    // false = Standard Mode    (max frame latency 3, smoother)
    bool         performanceMode = false;

    // How "Auto" picks a capture mode when none is pinned.
    // 0 = highest resolution first (4K60), 1 = highest framerate first (1080p144).
    int          modePreference = 0;

    // 0 = V-Sync (paced by the compositor), 1 = allow tearing (uncapped —
    // lets a 144 Hz capture through on a 60 Hz desktop).
    int          presentMode = 0;

    // Render-loop cap in FPS for the tearing path. 0 = unlimited.
    int          fpsLimit = 0;

    // Ask Media Foundation for NV12 instead of RGB32 (no per-frame color
    // conversion — needed to sustain 4K60 / 1080p144).
    bool         useNV12 = true;

    // NVIDIA Super Resolution (experimental).
    bool         nvsrEnabled = false;
    float        nvsrScale   = 1.5f;  // 1.3, 1.5, 2.0
    int          nvsrMode    = 1;     // 0 = quality, 1 = performance

    // User-chosen window title — what Discord / OBS see for Window Capture
    // and "Playing" detection. Empty means use the built-in default.
    std::wstring customTitle;

    // Auto-update (GitHub releases).
    bool         updateCheckOnStart = true;
    std::string  updateSkipVersion;   // "1.2.0" — don't nag about this one again

    // Window placement. INT32_MIN means "no saved value; use default".
    int          windowX = 0x80000000;
    int          windowY = 0x80000000;
    int          windowW = 1280;
    int          windowH = 720;

    bool Load();
    bool Save() const;

    static std::wstring DefaultPath();
};
