#include "Settings.h"

#include <Windows.h>
#include <ShlObj.h>
#include <fstream>
#include <sstream>
#include <cstdlib>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

} // namespace

std::wstring Settings::DefaultPath()
{
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) || !appData) {
        return L"DesktopCamApp.ini";
    }
    std::wstring dir = appData;
    CoTaskMemFree(appData);
    dir += L"\\DesktopCamApp";
    CreateDirectoryW(dir.c_str(), nullptr); // no-op if exists
    return dir + L"\\settings.ini";
}

bool Settings::Load()
{
    const std::wstring path = DefaultPath();
    std::ifstream f(path.c_str());
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));

        if      (key == "camera_link")     cameraLink    = Utf8ToWide(val);
        else if (key == "camera_mode_key") cameraModeKey = val;
        else if (key == "mic_id")        micId        = Utf8ToWide(val);
        else if (key == "speaker_id")    speakerId    = Utf8ToWide(val);
        else if (key == "mic_volume")    micVolume    = (float)std::atof(val.c_str());
        else if (key == "audio_swap_lr") audioSwapLR  = (val == "1" || val == "true");
        else if (key == "audio_latency_ms")   audioLatencyMs  = std::atoi(val.c_str());
        else if (key == "audio_low_latency")  audioLowLatency = (val == "1" || val == "true");
        else if (key == "ui_hidden")     uiHidden     = (val == "1" || val == "true");
        else if (key == "debug_console") debugConsole = (val == "1" || val == "true");
        else if (key == "borderless")       borderless       = (val == "1" || val == "true");
        else if (key == "performance_mode") performanceMode  = (val == "1" || val == "true");
        else if (key == "mode_preference")  modePreference   = std::atoi(val.c_str());
        else if (key == "present_mode")     presentMode      = std::atoi(val.c_str());
        else if (key == "fps_limit")        fpsLimit         = std::atoi(val.c_str());
        else if (key == "use_nv12")         useNV12          = (val == "1" || val == "true");
        else if (key == "update_check_on_start") updateCheckOnStart = (val == "1" || val == "true");
        else if (key == "update_skip_version")   updateSkipVersion  = val;
        else if (key == "nvsr_enabled")     nvsrEnabled      = (val == "1" || val == "true");
        else if (key == "nvsr_scale")       nvsrScale        = (float)std::atof(val.c_str());
        else if (key == "nvsr_mode")        nvsrMode         = std::atoi(val.c_str());
        else if (key == "custom_title")     customTitle      = Utf8ToWide(val);
        else if (key == "window_x")      windowX      = std::atoi(val.c_str());
        else if (key == "window_y")      windowY      = std::atoi(val.c_str());
        else if (key == "window_w")      windowW      = std::atoi(val.c_str());
        else if (key == "window_h")      windowH      = std::atoi(val.c_str());
    }
    return true;
}

bool Settings::Save() const
{
    const std::wstring path = DefaultPath();
    std::ofstream f(path.c_str(), std::ios::out | std::ios::trunc);
    if (!f.is_open()) return false;

    f << "# Desktop Cam App settings (UTF-8)\n";
    f << "camera_link="     << WideToUtf8(cameraLink)  << "\n";
    f << "camera_mode_key=" << cameraModeKey           << "\n";
    f << "mic_id="        << WideToUtf8(micId)       << "\n";
    f << "speaker_id="    << WideToUtf8(speakerId)   << "\n";
    f << "mic_volume="    << micVolume               << "\n";
    f << "audio_swap_lr=" << (audioSwapLR ? 1 : 0)   << "\n";
    f << "audio_latency_ms="  << audioLatencyMs            << "\n";
    f << "audio_low_latency=" << (audioLowLatency ? 1 : 0) << "\n";
    f << "ui_hidden="     << (uiHidden ? 1 : 0)      << "\n";
    f << "debug_console=" << (debugConsole ? 1 : 0)  << "\n";
    f << "borderless="       << (borderless ? 1 : 0)       << "\n";
    f << "performance_mode=" << (performanceMode ? 1 : 0)  << "\n";
    f << "mode_preference="  << modePreference             << "\n";
    f << "present_mode="     << presentMode                << "\n";
    f << "fps_limit="        << fpsLimit                   << "\n";
    f << "use_nv12="         << (useNV12 ? 1 : 0)          << "\n";
    f << "update_check_on_start=" << (updateCheckOnStart ? 1 : 0) << "\n";
    f << "update_skip_version="   << updateSkipVersion            << "\n";
    f << "nvsr_enabled="     << (nvsrEnabled ? 1 : 0)      << "\n";
    f << "nvsr_scale="       << nvsrScale                  << "\n";
    f << "nvsr_mode="        << nvsrMode                   << "\n";
    f << "custom_title="     << WideToUtf8(customTitle)    << "\n";
    f << "window_x="      << windowX                 << "\n";
    f << "window_y="      << windowY                 << "\n";
    f << "window_w="      << windowW                 << "\n";
    f << "window_h="      << windowH                 << "\n";
    return static_cast<bool>(f);
}
