#pragma once

// Auto-update against GitHub Releases.
//
// Check  : GET https://api.github.com/repos/<owner>/<repo>/releases/latest
// Install: download the release's DesktopCamApp.exe asset next to the app,
//          then hand off to a tiny .cmd that waits for this process to exit,
//          swaps the binary and relaunches it.
//
// Everything runs on a worker thread; the UI only reads atomics + a couple of
// mutex-guarded strings, so a slow or unreachable network never stalls the
// render loop.

#include <Windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class Updater
{
public:
    enum class State
    {
        Idle,
        Checking,
        UpToDate,
        UpdateAvailable,
        Downloading,
        ReadyToInstall,
        Failed,
    };

    struct ReleaseInfo
    {
        std::string version;    // "1.2.0" (tag with any leading 'v' stripped)
        std::string tag;        // "v1.2.0"
        std::string notes;      // release body, trimmed
        std::string assetUrl;   // browser_download_url of the .exe asset
        std::string assetName;  // "DesktopCamApp.exe"
        std::string htmlUrl;    // release page, for the "View release" button
    };

    Updater();
    ~Updater();

    // Current build's version, e.g. "1.2.0".
    static const char* CurrentVersion();
    // "https://github.com/owner/repo"
    static const char* ProjectUrl();

    // Kick off a background check. No-op while another operation is running.
    void CheckAsync();

    // Download the pending update. Call only in State::UpdateAvailable.
    void DownloadAsync();

    // Swap in the downloaded build and relaunch. Returns true if the handoff
    // started — the caller should quit the app immediately afterwards.
    bool InstallAndRestart();

    State       GetState()  const { return state_.load(); }
    float       Progress()  const { return progress_.load(); }
    bool        Busy()      const;
    std::string Message()   const;
    ReleaseInfo Latest()    const;

    // "1.3.0" is newer than "1.2.9"; malformed strings compare as older.
    static bool IsNewer(const std::string& candidate, const std::string& current);

private:
    void SetState(State s, const std::string& message);
    void JoinWorker();
    void CheckWorker();
    void DownloadWorker();

    std::atomic<State> state_{ State::Idle };
    std::atomic<float> progress_{ 0.0f };
    std::atomic<bool>  running_{ false };
    std::atomic<bool>  cancel_{ false };

    mutable std::mutex mutex_;
    std::string        message_;
    ReleaseInfo        latest_;
    std::wstring       downloadedPath_;

    std::thread        worker_;
};
