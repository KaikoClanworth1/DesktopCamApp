#include "Updater.h"
#include "Version.h"

#include <winhttp.h>
#include <shlobj.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr const wchar_t* kUserAgent = L"DesktopCamApp-Updater/1.0";
constexpr const char*    kAssetName = "DesktopCamApp.exe";

// Second line of defence behind the CMake check: an empty version string
// parses as 0.0.0, which makes every release look newer forever.
static_assert(sizeof(DCA_VERSION_STRING) > 1,
              "DCA_VERSION_STRING is empty — such a build offers itself an endless update");

// A version we can actually compare needs at least one digit.
bool LooksLikeVersion(const std::string& v)
{
    for (char c : v)
        if (std::isdigit((unsigned char)c)) return true;
    return false;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool EndsWithNoCase(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower((unsigned char)s[s.size() - n + i]) != std::tolower((unsigned char)suffix[i]))
            return false;
    }
    return true;
}

// ---- Tiny JSON helpers -------------------------------------------------------
// The GitHub release payload is machine-generated and well-formed, so a
// key-directed string scan is enough — no need to drag in a JSON library.
std::string JsonUnescape(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\' || i + 1 >= in.size()) { out += in[i]; continue; }
        const char c = in[++i];
        switch (c) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': case 'f': break;
            case 'u': {
                // Keep it simple: decode BMP escapes to UTF-8, skip surrogates.
                if (i + 4 < in.size()) {
                    const std::string hex = in.substr(i + 1, 4);
                    const unsigned cp = (unsigned)std::strtoul(hex.c_str(), nullptr, 16);
                    i += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0xD800 || cp > 0xDFFF) {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                }
                break;
            }
            default: out += c; break;
        }
    }
    return out;
}

// Finds "key": "value" starting at `from`. Returns false if absent.
bool JsonString(const std::string& json, const char* key, size_t from,
                std::string* out, size_t* valueEnd = nullptr)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t k = json.find(needle, from);
    if (k == std::string::npos) return false;
    size_t p = json.find(':', k + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r' || json[p] == '\t')) ++p;
    if (p >= json.size() || json[p] != '"') return false;   // null or non-string
    ++p;
    const size_t start = p;
    while (p < json.size()) {
        if (json[p] == '\\') { p += 2; continue; }
        if (json[p] == '"')  break;
        ++p;
    }
    if (p > json.size()) return false;
    *out = JsonUnescape(json.substr(start, p - start));
    if (valueEnd) *valueEnd = p;
    return true;
}

// ---- HTTP --------------------------------------------------------------------
struct HttpResult
{
    bool        ok = false;
    DWORD       status = 0;
    std::string error;
};

HttpResult HttpGet(const std::wstring& url,
                   const std::wstring& extraHeaders,
                   std::string* outBody,
                   const std::wstring* outFilePath,
                   std::atomic<float>* progress,
                   const std::atomic<bool>* cancel)
{
    HttpResult res;

    URL_COMPONENTS uc{};
    uc.dwStructSize      = sizeof(uc);
    wchar_t host[256]{}, path[4096]{}, extra[2048]{};
    uc.lpszHostName      = host;  uc.dwHostNameLength  = (DWORD)std::size(host);
    uc.lpszUrlPath       = path;  uc.dwUrlPathLength   = (DWORD)std::size(path);
    uc.lpszExtraInfo     = extra; uc.dwExtraInfoLength = (DWORD)std::size(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        res.error = "Bad update URL";
        return res;
    }
    // Asset URLs carry a query string (signed S3 links) — it has to travel
    // with the path or the download 403s.
    std::wstring resource = path;
    resource += extra;
    const bool secure = (url.rfind(L"https://", 0) == 0);

    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { res.error = "WinHttpOpen failed"; return res; }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);

    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        res.error = "Could not reach github.com";
        return res;
    }

    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", resource.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        res.error = "WinHttpOpenRequest failed";
        return res;
    }

    const wchar_t* headers = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str();
    const DWORD headerLen  = extraHeaders.empty() ? 0 : (DWORD)-1L;

    bool ok = WinHttpSendRequest(req, headers, headerLen,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        const DWORD e = GetLastError();
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Network error (0x%08lX)", (unsigned long)e);
        res.error = buf;
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        return res;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    res.status = status;

    uint64_t total = 0;
    {
        DWORD len = 0, lsz = sizeof(len);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &len, &lsz, WINHTTP_NO_HEADER_INDEX))
            total = len;
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    if (outFilePath) {
        file = CreateFileW(outFilePath->c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            res.error = "Could not write the downloaded file";
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return res;
        }
    }

    std::vector<char> chunk(64 * 1024);
    uint64_t received = 0;
    for (;;) {
        if (cancel && cancel->load()) { res.error = "Cancelled"; break; }
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), (DWORD)chunk.size(), &read)) {
            res.error = "Download interrupted";
            break;
        }
        if (read == 0) { res.ok = true; break; }
        received += read;
        if (outBody) outBody->append(chunk.data(), read);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (!WriteFile(file, chunk.data(), read, &written, nullptr) || written != read) {
                res.error = "Disk write failed";
                break;
            }
        }
        if (progress && total > 0)
            progress->store((float)((double)received / (double)total));
    }

    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);

    if (res.ok && status != 200) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "GitHub returned HTTP %lu", (unsigned long)status);
        res.error = buf;
        res.ok = false;
    }
    return res;
}

// ---- Paths -------------------------------------------------------------------
std::wstring ExePath()
{
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return buf;
}

std::wstring ParentDir(const std::wstring& p)
{
    const size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
}

std::wstring UpdateCacheDir()
{
    PWSTR local = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
        dir = local;
        CoTaskMemFree(local);
        dir += L"\\DesktopCamApp";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\updates";
        CreateDirectoryW(dir.c_str(), nullptr);
    } else {
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW((DWORD)std::size(tmp), tmp);
        dir = tmp;
    }
    return dir;
}

// Can we replace the running executable without elevating?
bool AppDirWritable()
{
    const std::wstring probe = ParentDir(ExePath()) + L"\\.dca_write_test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

int ParseVersionPart(const std::string& s, size_t& pos)
{
    while (pos < s.size() && !std::isdigit((unsigned char)s[pos])) {
        if (s[pos] == '.') break;
        ++pos;
    }
    int v = 0;
    bool any = false;
    while (pos < s.size() && std::isdigit((unsigned char)s[pos])) {
        v = v * 10 + (s[pos] - '0');
        ++pos; any = true;
    }
    if (pos < s.size() && s[pos] == '.') ++pos;
    return any ? v : 0;
}

} // namespace

// ---- Updater -----------------------------------------------------------------
Updater::Updater() = default;

Updater::~Updater()
{
    cancel_.store(true);
    JoinWorker();
}

const char* Updater::CurrentVersion() { return DCA_VERSION_STRING; }
const char* Updater::ProjectUrl()     { return DCA_GITHUB_URL; }

bool Updater::Busy() const
{
    const State s = state_.load();
    return s == State::Checking || s == State::Downloading;
}

std::string Updater::Message() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return message_;
}

Updater::ReleaseInfo Updater::Latest() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return latest_;
}

void Updater::SetState(State s, const std::string& message)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        message_ = message;
    }
    state_.store(s);
}

void Updater::JoinWorker()
{
    if (worker_.joinable()) worker_.join();
}

bool Updater::IsNewer(const std::string& candidate, const std::string& current)
{
    // Never claim an update when either side isn't a comparable version —
    // "unknown" must not read as 0.0.0.
    if (!LooksLikeVersion(candidate) || !LooksLikeVersion(current)) return false;

    size_t a = 0, b = 0;
    for (int i = 0; i < 3; ++i) {
        const int ca = ParseVersionPart(candidate, a);
        const int cb = ParseVersionPart(current, b);
        if (ca != cb) return ca > cb;
    }
    return false;
}

void Updater::CheckAsync()
{
    if (running_.exchange(true)) return;
    JoinWorker();
    cancel_.store(false);
    progress_.store(0.0f);
    SetState(State::Checking, "Checking for updates\xE2\x80\xA6");
    worker_ = std::thread([this] {
        CheckWorker();
        running_.store(false);
    });
}

void Updater::CheckWorker()
{
    if (!LooksLikeVersion(CurrentVersion())) {
        SetState(State::Failed,
                 "This build has no version stamp, so updates can't be compared. "
                 "Download the latest build from the releases page.");
        return;
    }

    const std::wstring url = L"https://api.github.com/repos/"
                             + Widen(DCA_GITHUB_OWNER) + L"/" + Widen(DCA_GITHUB_REPO)
                             + L"/releases/latest";
    const std::wstring headers =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";

    std::string body;
    HttpResult r = HttpGet(url, headers, &body, nullptr, nullptr, &cancel_);
    if (!r.ok) {
        if (r.status == 404)
            SetState(State::Failed, "No releases published yet.");
        else
            SetState(State::Failed, r.error.empty() ? "Update check failed." : r.error);
        return;
    }

    ReleaseInfo info;
    if (!JsonString(body, "tag_name", 0, &info.tag)) {
        SetState(State::Failed, "Unexpected response from GitHub.");
        return;
    }
    info.version = info.tag;
    if (!info.version.empty() && (info.version[0] == 'v' || info.version[0] == 'V'))
        info.version.erase(0, 1);

    JsonString(body, "html_url", 0, &info.htmlUrl);
    if (JsonString(body, "body", 0, &info.notes) && info.notes.size() > 1200)
        info.notes = info.notes.substr(0, 1200) + "\n...";

    // Find the installable asset. Every "browser_download_url" in the payload
    // belongs to one asset; we want the raw .exe.
    for (size_t pos = 0;;) {
        std::string urlStr;
        size_t end = 0;
        if (!JsonString(body, "browser_download_url", pos, &urlStr, &end)) break;
        pos = end;
        if (EndsWithNoCase(urlStr, ".exe")) {
            info.assetUrl  = urlStr;
            const size_t slash = urlStr.find_last_of('/');
            info.assetName = (slash == std::string::npos) ? kAssetName : urlStr.substr(slash + 1);
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lk(mutex_);
        latest_ = info;
    }

    if (!IsNewer(info.version, CurrentVersion())) {
        SetState(State::UpToDate, std::string("You're on the latest version (")
                                  + CurrentVersion() + ").");
        return;
    }
    if (info.assetUrl.empty()) {
        SetState(State::Failed,
                 "Version " + info.version + " is available, but that release has no "
                 ".exe asset to install. Download it from the releases page.");
        return;
    }
    SetState(State::UpdateAvailable, "Version " + info.version + " is available.");
}

void Updater::DownloadAsync()
{
    if (state_.load() != State::UpdateAvailable) return;
    if (running_.exchange(true)) return;
    JoinWorker();
    cancel_.store(false);
    progress_.store(0.0f);
    SetState(State::Downloading, "Downloading update\xE2\x80\xA6");
    worker_ = std::thread([this] {
        DownloadWorker();
        running_.store(false);
    });
}

void Updater::DownloadWorker()
{
    ReleaseInfo info = Latest();
    if (info.assetUrl.empty()) {
        SetState(State::Failed, "Nothing to download.");
        return;
    }

    const std::wstring dir  = UpdateCacheDir();
    const std::wstring dest = dir + L"\\DesktopCamApp-" + Widen(info.version) + L".exe";

    HttpResult r = HttpGet(Widen(info.assetUrl), L"", nullptr, &dest, &progress_, &cancel_);
    if (!r.ok) {
        DeleteFileW(dest.c_str());
        SetState(State::Failed, r.error.empty() ? "Download failed." : r.error);
        return;
    }

    // Sanity check: a Windows executable starts with "MZ" and a truncated
    // download is the most likely failure we can actually detect.
    bool looksLikeExe = false;
    LARGE_INTEGER size{};
    HANDLE h = CreateFileW(dest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char magic[2] = {};
        DWORD got = 0;
        GetFileSizeEx(h, &size);
        ReadFile(h, magic, 2, &got, nullptr);
        CloseHandle(h);
        looksLikeExe = (got == 2 && magic[0] == 'M' && magic[1] == 'Z' && size.QuadPart > 256 * 1024);
    }
    if (!looksLikeExe) {
        DeleteFileW(dest.c_str());
        SetState(State::Failed, "The downloaded file doesn't look like a valid build.");
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mutex_);
        downloadedPath_ = dest;
    }
    progress_.store(1.0f);
    SetState(State::ReadyToInstall,
             "Version " + info.version + " downloaded. Restart to finish installing.");
}

bool Updater::InstallAndRestart()
{
    if (state_.load() != State::ReadyToInstall) return false;

    std::wstring newExe;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        newExe = downloadedPath_;
    }
    if (newExe.empty()) return false;

    const std::wstring target = ExePath();
    const std::wstring script = UpdateCacheDir() + L"\\apply-update.cmd";

    // Wait for this process to release its own binary, swap it, relaunch, and
    // delete ourselves. Keeps a .old copy until the copy succeeds so a failed
    // update can't leave the user with no app.
    std::wstring cmd;
    // Written without parenthesised blocks on purpose: %TRIES% inside a block
    // would need delayed expansion and silently loop forever.
    cmd += L"@echo off\r\n";
    cmd += L"set TRIES=0\r\n";
    cmd += L":retry\r\n";
    cmd += L"ping -n 2 127.0.0.1 >nul\r\n";
    cmd += L"move /y \"" + target + L"\" \"" + target + L".old\" >nul 2>&1\r\n";
    cmd += L"if not errorlevel 1 goto swap\r\n";
    cmd += L"set /a TRIES+=1\r\n";
    cmd += L"if %TRIES% lss 30 goto retry\r\n";
    cmd += L"exit /b 1\r\n";
    cmd += L":swap\r\n";
    cmd += L"copy /y \"" + newExe + L"\" \"" + target + L"\" >nul 2>&1\r\n";
    cmd += L"if not errorlevel 1 goto done\r\n";
    cmd += L"move /y \"" + target + L".old\" \"" + target + L"\" >nul 2>&1\r\n";
    cmd += L"start \"\" \"" + target + L"\"\r\n";
    cmd += L"exit /b 1\r\n";
    cmd += L":done\r\n";
    cmd += L"del \"" + target + L".old\" >nul 2>&1\r\n";
    cmd += L"del \"" + newExe + L"\" >nul 2>&1\r\n";
    cmd += L"start \"\" \"" + target + L"\"\r\n";
    cmd += L"(goto) 2>nul & del \"%~f0\"\r\n";

    const std::string ansi = [&] {
        int n = WideCharToMultiByte(CP_ACP, 0, cmd.c_str(), (int)cmd.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_ACP, 0, cmd.c_str(), (int)cmd.size(), s.data(), n, nullptr, nullptr);
        return s;
    }();

    HANDLE f = CreateFileW(script.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        SetState(State::Failed, "Could not write the update helper script.");
        return false;
    }
    DWORD written = 0;
    WriteFile(f, ansi.data(), (DWORD)ansi.size(), &written, nullptr);
    CloseHandle(f);

    const std::wstring args = L"/c \"" + script + L"\"";

    if (AppDirWritable()) {
        std::wstring line = L"cmd.exe " + args;
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags     = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            SetState(State::Failed, "Could not start the update helper.");
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    // Installed somewhere that needs admin rights (Program Files, etc.) —
    // ask for elevation instead of silently failing.
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        SetState(State::Failed, "Update needs administrator rights and the prompt was declined.");
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}
