#include "UI.h"
#include "Application.h"
#include "DebugConsole.h"
#include "UpscalerNV.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <shellapi.h>
#include <string>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cmath>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---- utf-16 -> utf-8 for ImGui -----------------------------------------------
static std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Wide(const char* utf8)
{
    if (!utf8 || !*utf8) return {};
    const int len = (int)std::strlen(utf8);
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, len, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, len, w.data(), n);
    return w;
}

UI::UI() = default;
UI::~UI() { Shutdown(); }

// Shared theme colors.
static const ImVec4 kTeal        = ImVec4(0.10f, 0.85f, 0.76f, 1.0f);
static const ImVec4 kTealDim     = ImVec4(0.10f, 0.85f, 0.76f, 0.20f);
static const ImVec4 kTealHover   = ImVec4(0.10f, 0.85f, 0.76f, 0.35f);
static const ImVec4 kBg          = ImVec4(0.08f, 0.09f, 0.11f, 0.94f);
static const ImVec4 kPanel       = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
static const ImVec4 kPanelHover  = ImVec4(0.17f, 0.19f, 0.22f, 1.00f);
static const ImVec4 kPanelActive = ImVec4(0.10f, 0.11f, 0.14f, 1.00f);
static const ImVec4 kDim         = ImVec4(0.55f, 0.58f, 0.64f, 1.00f);
static const ImVec4 kGreen       = ImVec4(0.35f, 0.90f, 0.45f, 1.00f);

static void ApplyTheme()
{
    ImGui::StyleColorsDark();
    auto& s = ImGui::GetStyle();
    s.WindowRounding  = 12.0f;
    s.FrameRounding   = 7.0f;
    s.PopupRounding   = 7.0f;
    s.GrabRounding    = 7.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowPadding   = ImVec2(18, 16);
    s.FramePadding    = ImVec2(10, 7);
    s.ItemSpacing     = ImVec2(10, 10);
    s.ItemInnerSpacing= ImVec2(8, 6);
    s.IndentSpacing   = 12;
    s.ScrollbarSize   = 12;
    s.FrameBorderSize = 0;
    s.WindowBorderSize= 0;

    auto& c = s.Colors;
    c[ImGuiCol_WindowBg]         = kBg;
    c[ImGuiCol_ChildBg]          = ImVec4(0,0,0,0);
    c[ImGuiCol_PopupBg]          = ImVec4(0.10f, 0.11f, 0.14f, 0.98f);
    c[ImGuiCol_Text]             = ImVec4(0.92f, 0.94f, 0.97f, 1.0f);
    c[ImGuiCol_TextDisabled]     = kDim;
    c[ImGuiCol_FrameBg]          = kPanel;
    c[ImGuiCol_FrameBgHovered]   = kPanelHover;
    c[ImGuiCol_FrameBgActive]    = kPanelActive;
    c[ImGuiCol_Button]           = kPanel;
    c[ImGuiCol_ButtonHovered]    = kPanelHover;
    c[ImGuiCol_ButtonActive]     = kPanelActive;
    c[ImGuiCol_Header]           = kTealDim;
    c[ImGuiCol_HeaderHovered]    = kTealHover;
    c[ImGuiCol_HeaderActive]     = kTealHover;
    c[ImGuiCol_CheckMark]        = kTeal;
    c[ImGuiCol_SliderGrab]       = kTeal;
    c[ImGuiCol_SliderGrabActive] = kTeal;
    c[ImGuiCol_Separator]        = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);
    c[ImGuiCol_Border]           = ImVec4(0.20f, 0.22f, 0.26f, 0.6f);
    c[ImGuiCol_Tab]              = kPanel;
    c[ImGuiCol_TabHovered]       = kPanelHover;
    c[ImGuiCol_TabActive]        = kTealDim;
    c[ImGuiCol_NavHighlight]     = kTeal;
}

bool UI::Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyTheme();

    if (!ImGui_ImplWin32_Init(hwnd)) return false;
    if (!ImGui_ImplDX11_Init(device, ctx)) return false;
    initialized_ = true;
    return true;
}

void UI::Shutdown()
{
    if (!initialized_) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void UI::NewFrame()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

LRESULT UI::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool& handled)
{
    if (!initialized_) { handled = false; return 0; }
    LRESULT r = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
    // Only consider the event "handled" if ImGui wants to capture the
    // corresponding input — otherwise pass it through to the window.
    ImGuiIO& io = ImGui::GetIO();
    handled = false;
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
        handled = io.WantCaptureMouse;
        break;
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_CHAR:
        handled = io.WantCaptureKeyboard;
        break;
    }
    return r;
}

// ---- Custom widgets ---------------------------------------------------------
static void SectionLabel(const char* text)
{
    // Uppercase teal label with a little top spacing, no separator line.
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushStyleColor(ImGuiCol_Text, kTeal);
    std::string up = text;
    for (auto& ch : up) ch = (char)std::toupper((unsigned char)ch);
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 2));
}

// iOS-style toggle switch. Returns true when the value changed.
static bool ToggleSwitch(const char* id, bool* v)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 pos  = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight() * 0.9f;
    const float w = h * 1.85f;
    const ImVec2 end(pos.x + w, pos.y + h);

    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;

    const float t = *v ? 1.0f : 0.0f;
    const ImU32 offBg = IM_COL32(55, 60, 70, 255);
    const ImU32 onBg  = IM_COL32(26, 217, 195, 255);
    const ImU32 bg    = *v ? onBg : offBg;

    draw->AddRectFilled(pos, end, bg, h * 0.5f);
    const float knobR  = h * 0.42f;
    const float knobX  = *v ? (end.x - h * 0.5f) : (pos.x + h * 0.5f);
    const float knobY  = pos.y + h * 0.5f;
    draw->AddCircleFilled(ImVec2(knobX, knobY), knobR, IM_COL32(255, 255, 255, 255));
    (void)t;
    return clicked;
}

// Two-button segmented selector. Returns the (possibly updated) index.
static int Segmented(const char* idPrefix, int current, const char* a, const char* b)
{
    ImGuiStyle& s = ImGui::GetStyle();
    const float w = (ImGui::GetContentRegionAvail().x - s.ItemInnerSpacing.x) * 0.5f;
    const ImVec2 sz(w, 0);
    auto drawBtn = [&](const char* label, bool active, int idx) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        kTealDim);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kTealHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kTealHover);
            ImGui::PushStyleColor(ImGuiCol_Text,          kTeal);
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s##%s%d", label, idPrefix, idx);
        const bool clicked = ImGui::Button(buf, sz);
        if (active) ImGui::PopStyleColor(4);
        return clicked;
    };
    if (drawBtn(a, current == 0, 0)) current = 0;
    ImGui::SameLine(0, s.ItemInnerSpacing.x);
    if (drawBtn(b, current == 1, 1)) current = 1;
    return current;
}

// Rainbow segmented VU meter.
static void VuMeter(float peak)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w  = ImGui::GetContentRegionAvail().x;
    const float h  = 14.0f;
    const int   nBars = 34;
    const float barW  = w / nBars;
    const float gap   = 2.0f;
    const float pk = peak < 0.0f ? 0.0f : (peak > 1.0f ? 1.0f : peak);
    const int lit = (int)(pk * (nBars - 0.0001f) + 0.5f);

    for (int i = 0; i < nBars; ++i) {
        const float frac = (float)i / (float)(nBars - 1);
        ImU32 col;
        if (i < lit) {
            if      (frac < 0.60f) col = IM_COL32(    (int)(frac * 250),          240, 70,  255);
            else if (frac < 0.80f) col = IM_COL32(255,          240 - (int)((frac - 0.6f) * 300),  40,  255);
            else                    col = IM_COL32(255,           60 + (int)((1.0f - frac) * 100), 40,  255);
        } else {
            col = IM_COL32(38, 42, 50, 255);
        }
        ImVec2 a(pos.x + i * barW,          pos.y);
        ImVec2 b(pos.x + (i + 1) * barW - gap, pos.y + h);
        draw->AddRectFilled(a, b, col, 2.0f);
    }
    ImGui::Dummy(ImVec2(w, h));
}

// Live indicator: pulsing green dot + "LIVE" + optional HH:MM:SS counter.
static void LiveBadge(bool running, uint64_t elapsedMs)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float r    = 5.5f;
    const float yCenter = pos.y + ImGui::GetTextLineHeight() * 0.5f;
    const ImU32 dotCol = running
        ? IM_COL32(74, 230, 105, 255)
        : IM_COL32(90, 96, 106, 255);
    if (running) {
        const float t   = (float)(::GetTickCount64() % 1200) / 1200.0f;
        const float pul = 0.5f + 0.5f * std::sinf(t * 6.283185f);
        draw->AddCircleFilled(ImVec2(pos.x + r, yCenter), r + 5.0f * pul,
                              IM_COL32(74, 230, 105, (int)(80 * (1.0f - pul))));
    }
    draw->AddCircleFilled(ImVec2(pos.x + r, yCenter), r, dotCol);
    ImGui::Dummy(ImVec2(r * 2 + 6, 0));
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Text, running ? kGreen : kDim);
    ImGui::TextUnformatted(running ? "LIVE" : "IDLE");
    ImGui::PopStyleColor();

    if (running) {
        const uint64_t s  = elapsedMs / 1000;
        const uint32_t hh = (uint32_t)(s / 3600);
        const uint32_t mm = (uint32_t)((s / 60) % 60);
        const uint32_t ss = (uint32_t)(s % 60);
        ImGui::SameLine(0, 18);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::Text("%02u:%02u:%02u", hh, mm, ss);
        ImGui::PopStyleColor();
    }
}

// ---- Main control panel ------------------------------------------------------
void UI::Draw(Application& app)
{
    ImGui::SetNextWindowPos(ImVec2(12, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);

    // "Hidden" mode: render the full panel with global alpha=0 so the GPU
    // still has a consistent per-frame workload (prevents 30 fps drops due
    // to GPU clock-gating when there's nothing to do).
    const bool hidden = app.IsUIHidden();
    if (hidden) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);

    ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_AlwaysAutoResize;
    if (hidden) panelFlags |= ImGuiWindowFlags_NoInputs;

    ImGui::Begin("Desktop Cam App", nullptr, panelFlags);

    const bool running = app.IsRunning();

    // ---- Header row: title + LIVE badge
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.97f, 0.99f, 1.0f));
    ImGui::PushFont(nullptr);
    ImGui::TextUnformatted("Desktop Cam App");
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 16);
    LiveBadge(running, app.CaptureElapsedMs());

    // ---- Video ------------------------------------------------------------
    SectionLabel("Video");
    {
        const auto& devices = app.CameraDevices();
        int current = app.SelectedCameraIndex();
        std::string preview = (current >= 0 && current < (int)devices.size())
            ? Utf8(devices[current].friendlyName) : std::string("<none>");

        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("Camera");
        ImGui::PopStyleColor();

        ImGui::SetNextItemWidth(-120);
        if (ImGui::BeginCombo("##camera", preview.c_str()))
        {
            for (int i = 0; i < (int)devices.size(); ++i) {
                const bool selected = (i == current);
                std::string label = Utf8(devices[i].friendlyName);
                if (label.empty()) label = "Camera " + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), selected))
                    app.SetSelectedCameraIndex(i);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh##cam", ImVec2(110, 0))) app.RefreshCameraList();

        // --- Mode picker ---
        const auto& modes = app.CameraModes();
        const int   curMode = app.SelectedCameraModeIndex();
        std::string modePreview = "Auto (best available)";
        if (curMode >= 0 && curMode < (int)modes.size())
            modePreview = modes[curMode].label;

        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("Mode");
        ImGui::PopStyleColor();

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##cammode", modePreview.c_str()))
        {
            if (ImGui::Selectable("Auto (best available)", curMode < 0))
                app.SetSelectedCameraModeIndex(-1);
            if (curMode < 0) ImGui::SetItemDefaultFocus();

            if (!modes.empty()) ImGui::Separator();
            for (int i = 0; i < (int)modes.size(); ++i) {
                const bool sel = (i == curMode);
                if (ImGui::Selectable(modes[i].label.c_str(), sel))
                    app.SetSelectedCameraModeIndex(i);
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        // When the mode is left on Auto, let the user say what "best" means:
        // a 4K capture card can do 3840x2160@60 or 1920x1080@144, and which
        // one wins is a matter of taste.
        if (curMode < 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted("Auto picks");
            ImGui::PopStyleColor();
            const int curPref = (app.AutoModePreference() == ModePreference::Framerate) ? 1 : 0;
            const int pick    = Segmented("autopref", curPref, "Max resolution", "Max framerate");
            if (pick != curPref)
                app.SetAutoModePreference(pick == 1 ? ModePreference::Framerate
                                                    : ModePreference::Resolution);
        }

        if (!modes.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted("Change takes effect the next time you press Start.");
            ImGui::PopStyleColor();
        }
    }

    // ---- Audio ------------------------------------------------------------
    SectionLabel("Audio");
    {
        const auto& mics = app.MicrophoneDevices();
        int current = app.SelectedMicIndex();
        std::string preview = (current >= 0 && current < (int)mics.size())
            ? Utf8(mics[current].friendlyName) : std::string("<none>");

        ImGui::SetNextItemWidth(-120);
        if (ImGui::BeginCombo("Microphone", preview.c_str()))
        {
            for (int i = 0; i < (int)mics.size(); ++i) {
                const bool selected = (i == current);
                std::string label = Utf8(mics[i].friendlyName);
                if (label.empty()) label = "Mic " + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), selected))
                    app.SetSelectedMicIndex(i);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const auto& outs = app.SpeakerDevices();
        int curOut = app.SelectedSpeakerIndex();
        std::string outPreview = (curOut >= 0 && curOut < (int)outs.size())
            ? Utf8(outs[curOut].friendlyName) : std::string("<system default>");

        ImGui::SetNextItemWidth(-120);
        if (ImGui::BeginCombo("Output", outPreview.c_str()))
        {
            if (ImGui::Selectable("<system default>", curOut < 0))
                app.SetSelectedSpeakerIndex(-1);
            for (int i = 0; i < (int)outs.size(); ++i) {
                const bool selected = (i == curOut);
                std::string label = Utf8(outs[i].friendlyName);
                if (label.empty()) label = "Output " + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), selected))
                    app.SetSelectedSpeakerIndex(i);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        float vol = app.MicVolumePercent();
        ImGui::SetNextItemWidth(-120);
        if (ImGui::SliderFloat("Mic volume", &vol, 0.0f, 200.0f, "%.0f%%"))
            app.SetMicVolumePercent(vol);

        VuMeter(app.MicPeak());

        bool swap = app.GetAudioSwapLR();
        if (ImGui::Checkbox("Swap L / R", &swap)) app.SetAudioSwapLR(swap);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Flip left and right channels before passthrough.");
            ImGui::TextUnformatted("Use this if your mic reports L/R reversed.");
            ImGui::EndTooltip();
        }
    }

    // ---- Status -----------------------------------------------------------
    SectionLabel("Status");
    {
        LiveBadge(running, app.CaptureElapsedMs());
        int vw = 0, vh = 0;
        app.VideoSize(vw, vh);
        char stats[192];
        if (vw > 0 && vh > 0) std::snprintf(stats, sizeof(stats),
            "%s  \xE2\x80\xA2  capture %.0f fps  \xE2\x80\xA2  render %.0f fps",
            app.CaptureSummary().c_str(), app.CaptureFps(), app.Fps());
        else std::snprintf(stats, sizeof(stats),
            "Idle  \xE2\x80\xA2  render %.0f fps  \xE2\x80\xA2  DX11",
            app.Fps());
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(stats);

        const int hz = app.MonitorRefreshHz();
        if (hz > 0) {
            ImGui::Text("Display: %d Hz  \xE2\x80\xA2  %s", hz,
                        app.IsUncapped() ? "uncapped (tearing allowed)" : "v-sync");
            // Capturing faster than the screen can show is fine for OBS /
            // Discord (they pull frames from the swap chain), but say so.
            if (running && app.NegotiatedFps() > (float)hz + 1.0f && !app.IsUncapped()) {
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
                ImGui::TextWrapped("Capturing at %.0f fps on a %d Hz display \xE2\x80\x94 turn on "
                                   "Uncapped in Advanced to present every captured frame.",
                                   app.NegotiatedFps(), hz);
            }
        }
        ImGui::PopStyleColor();
    }

    // ---- Capture control --------------------------------------------------
    {
        const ImVec2 bigBtn(-FLT_MIN, 38);
        if (!running) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.55f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.68f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.12f, 0.45f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1,1,1,1));
            if (ImGui::Button("\xE2\x96\xB6  Start", bigBtn)) app.StartCapture();
            ImGui::PopStyleColor(4);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.80f, 0.25f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.30f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.65f, 0.18f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1,1,1,1));
            if (ImGui::Button("\xE2\x96\xA0  Stop", bigBtn)) app.StopCapture();
            ImGui::PopStyleColor(4);
        }
    }

    // ---- Window -----------------------------------------------------------
    SectionLabel("Window");
    {
        bool borderless = app.IsBorderless();
        if (ImGui::Checkbox("Borderless (no title bar)", &borderless))
            app.SetBorderless(borderless);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Hides the Windows title bar so it doesn't show up");
            ImGui::TextUnformatted("in Discord screen-share or OBS Window Capture.");
            ImGui::EndTooltip();
        }

        // App name (window title) — what Discord / OBS detect.
        static char titleBuf[128] = {0};
        static bool titleLoaded = false;
        if (!titleLoaded) {
            std::string cur = Utf8(app.CustomTitle());
            std::snprintf(titleBuf, sizeof(titleBuf), "%s", cur.c_str());
            titleLoaded = true;
        }
        ImGui::SetNextItemWidth(-140);
        const bool commit = ImGui::InputText("App name", titleBuf, sizeof(titleBuf),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Apply##title") || commit) {
            app.SetCustomTitle(Wide(titleBuf));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Discord / OBS Window Capture / Game Capture identify");
            ImGui::TextUnformatted("the app by its window title. Leave empty to reset to default.");
            ImGui::EndTooltip();
        }

        if (ImGui::Button("Hide UI", ImVec2(110, 0))) app.SetUIHidden(true);
        ImGui::SameLine();
        ImGui::TextDisabled("F1");
        ImGui::SameLine(0, 16);
        if (ImGui::Button("Fullscreen", ImVec2(110, 0))) app.ToggleFullscreen();
        ImGui::SameLine();
        ImGui::TextDisabled("Alt+Enter");

        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("If hiding the menu feels laggy, switch to Performance mode in Advanced - lighter pre-render queue.");
        ImGui::PopStyleColor();
    }

    // ---- Updates ----------------------------------------------------------
    SectionLabel("Updates");
    {
        Updater&   up      = app.GetUpdater();
        const auto state   = up.GetState();
        const auto release = up.Latest();
        const bool skipped = app.IsUpdateSkipped(release.version);

        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::Text("Installed version %s", Updater::CurrentVersion());
        ImGui::PopStyleColor();

        const std::string msg = up.Message();
        switch (state) {
            case Updater::State::Checking:
            case Updater::State::Downloading:
                ImGui::TextUnformatted(msg.c_str());
                break;
            case Updater::State::UpToDate:
                ImGui::TextColored(kGreen, "%s", msg.c_str());
                break;
            case Updater::State::UpdateAvailable:
            case Updater::State::ReadyToInstall:
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "%s", msg.c_str());
                break;
            case Updater::State::Failed:
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", msg.c_str());
                break;
            default:
                break;
        }

        if (state == Updater::State::Downloading) {
            ImGui::ProgressBar(up.Progress(), ImVec2(-FLT_MIN, 0));
        }

        if (state == Updater::State::UpdateAvailable && !release.notes.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped("%s", release.notes.c_str());
            ImGui::PopStyleColor();
        }

        if (state == Updater::State::UpdateAvailable && !skipped) {
            if (ImGui::Button("Download update", ImVec2(-FLT_MIN, 30)))
                up.DownloadAsync();
            if (ImGui::Button("Skip this version", ImVec2(150, 0)))
                app.SkipCurrentUpdate();
        } else if (state == Updater::State::ReadyToInstall) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.55f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.68f, 0.28f, 1.0f));
            if (ImGui::Button("Install and restart", ImVec2(-FLT_MIN, 30)))
                app.InstallUpdateAndRestart();
            ImGui::PopStyleColor(2);
        }

        ImGui::BeginDisabled(up.Busy());
        if (ImGui::Button("Check now", ImVec2(150, 0)))
            up.CheckAsync();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Releases page", ImVec2(150, 0))) {
            const std::string url = std::string(Updater::ProjectUrl()) + "/releases";
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        bool autoCheck = app.CheckUpdatesOnStart();
        if (ImGui::Checkbox("Check for updates on startup", &autoCheck))
            app.SetCheckUpdatesOnStart(autoCheck);
    }

    // ---- Advanced (collapsed by default) ----------------------------------
    if (ImGui::CollapsingHeader("Advanced")) {
        // Rendering mode — segmented selector.
        ImGui::TextUnformatted("Rendering mode");
        const bool perfWas = app.IsPerformanceMode();
        const int  cur     = perfWas ? 0 : 1;
        const int  chosen  = Segmented("mode", cur, "Performance", "Standard");
        if (chosen != cur) app.SetPerformanceMode(chosen == 0);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(app.IsPerformanceMode()
            ? "Lower latency, optimized for live capture."
            : "Smoother output \xE2\x80\x94 queues up to 2 frames ahead.");
        ImGui::PopStyleColor();

        // ---- Presentation ------------------------------------------------
        ImGui::Spacing();
        ImGui::TextUnformatted("Presentation");
        {
            const bool canTear = app.IsTearingSupported();
            if (!canTear) ImGui::BeginDisabled();
            const int curPresent = app.IsUncapped() ? 1 : 0;
            const int pick = Segmented("present", curPresent, "V-Sync", "Uncapped");
            if (pick != curPresent) app.SetUncapped(pick == 1);
            if (!canTear) ImGui::EndDisabled();

            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            if (!canTear) {
                ImGui::TextWrapped("Uncapped needs DXGI tearing support (Windows 10+ with a "
                                   "WDDM 2.0 driver) \xE2\x80\x94 not available here.");
            } else if (app.IsUncapped()) {
                ImGui::TextWrapped("Presents every frame as soon as it is drawn, so a 120/144 Hz "
                                   "capture isn't held down to the desktop refresh rate. Can tear.");
            } else {
                ImGui::TextWrapped("One frame per display refresh. Smoothest, but caps the app at "
                                   "your monitor's Hz.");
            }
            ImGui::PopStyleColor();

            if (app.IsUncapped()) {
                int limit = app.FpsLimit();
                ImGui::SetNextItemWidth(-120);
                if (ImGui::SliderInt("FPS limit", &limit, 0, 360,
                                     limit <= 0 ? "unlimited" : "%d fps"))
                    app.SetFpsLimit(limit);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("0 = unlimited. Set this to your capture's framerate");
                    ImGui::TextUnformatted("(e.g. 144) to stop the loop spinning faster than");
                    ImGui::TextUnformatted("frames actually arrive.");
                    ImGui::EndTooltip();
                }
            }
        }

        // ---- Capture pipeline --------------------------------------------
        ImGui::Spacing();
        bool nv12 = app.UseNV12();
        if (ImGui::Checkbox("NV12 direct capture (recommended)", &nv12)) app.SetUseNV12(nv12);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Takes frames from the card in their native NV12 layout and");
            ImGui::TextUnformatted("converts them on the GPU in our own shader. Skips Media");
            ImGui::TextUnformatted("Foundation's RGB32 conversion, which is what makes 4K60 and");
            ImGui::TextUnformatted("1080p144 sustainable. Turn off only if colors look wrong.");
            ImGui::TextUnformatted("Takes effect on the next Start.");
            ImGui::EndTooltip();
        }

        ImGui::Spacing();
        bool console = app.IsDebugConsoleOn();
        if (ImGui::Checkbox("Debug console", &console)) app.SetDebugConsoleOn(console);
        ImGui::SameLine();
        ImGui::TextDisabled("(MF negotiation + frame info)");

        auto verr = app.VideoError();
        auto aerr = app.AudioError();
        if (!verr.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Video error: %s", Utf8(verr).c_str());
        if (!aerr.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Audio error: %s", Utf8(aerr).c_str());

        // ---- NVIDIA Super Resolution (experimental) -----------------------
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                           "AI Upscaling (experimental \xE2\x80\x94 NVIDIA Broadcast SDK)");
        ImGui::TextDisabled("Needs RTX GPU + Broadcast SDK redistributable:");
        ImGui::TextDisabled("nvidia.com/en-us/geforce/broadcasting/broadcast-sdk/resources");

        auto& up = app.Upscaler();
        const auto status = up.GetStatus();

        const char* statusLabel = "Not initialized";
        ImVec4      statusColor(0.7f, 0.7f, 0.7f, 1.0f);
        switch (status) {
            case UpscalerNV::Status::Ready:
                statusLabel = "Ready \xE2\x80\x94 Broadcast SDK detected";
                statusColor = ImVec4(0.35f, 0.9f, 0.4f, 1.0f); break;
            case UpscalerNV::Status::DllMissing:
                statusLabel = "Not available \xE2\x80\x94 install the NVIDIA Broadcast SDK";
                statusColor = ImVec4(0.95f, 0.55f, 0.35f, 1.0f); break;
            case UpscalerNV::Status::CudaMissing:
                statusLabel = "Not available \xE2\x80\x94 CUDA runtime missing (update GPU driver)";
                statusColor = ImVec4(0.95f, 0.55f, 0.35f, 1.0f); break;
            case UpscalerNV::Status::RequiresRTX:
                statusLabel = "Not available \xE2\x80\x94 requires an NVIDIA RTX GPU";
                statusColor = ImVec4(0.95f, 0.55f, 0.35f, 1.0f); break;
            case UpscalerNV::Status::EffectLoadFailed:
                statusLabel = "SDK present but SR effect failed to load";
                statusColor = ImVec4(0.95f, 0.35f, 0.35f, 1.0f); break;
            default: break;
        }
        ImGui::TextColored(statusColor, "%s", statusLabel);
        const auto msg = up.StatusMessage();
        if (!msg.empty())
            ImGui::TextDisabled("%s", Utf8(msg).c_str());

        if (ImGui::Button("Retry init (prints diagnostics to debug console)"))
            app.RetryUpscalerInit();

        // Enable toggle — only allowed if SDK ready.
        const bool canToggle = (status == UpscalerNV::Status::Ready);
        if (!canToggle) ImGui::BeginDisabled();
        ImGui::TextUnformatted("NVIDIA RTX AI Upscaling");
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x - 60, 0));
        ImGui::SameLine();
        bool nvOn = up.IsEnabled();
        if (ToggleSwitch("##nvsrtoggle", &nvOn))
            app.SetUpscalerEnabled(nvOn);

        // Scale combo — the SR model tops out at a 4K output, so which scales
        // are offered depends on what the card is actually feeding us.
        int inW = 0, inH = 0;
        app.VideoSize(inW, inH);
        const char* allScaleLabels[] = { "1.3x", "1.5x", "2.0x" };
        const float allScaleValues[] = {  1.3f,  1.5f,  2.0f  };
        const char* scaleLabels[3];
        float       scaleValues[3];
        int         scaleCount = 0;
        for (int i = 0; i < 3; ++i) {
            const bool fits = (inW <= 0) ||
                              ((float)inW * allScaleValues[i] <= 3840.5f &&
                               (float)inH * allScaleValues[i] <= 2160.5f);
            if (fits) {
                scaleLabels[scaleCount] = allScaleLabels[i];
                scaleValues[scaleCount] = allScaleValues[i];
                ++scaleCount;
            }
        }

        if (scaleCount == 0) {
            ImGui::TextDisabled("Input is already %dx%d \xE2\x80\x94 nothing to upscale to.", inW, inH);
        } else {
            int curScale = 0;
            for (int i = 0; i < scaleCount; ++i)
                if (std::abs(up.GetScale() - scaleValues[i]) < 0.05f) { curScale = i; break; }
            ImGui::SetNextItemWidth(-120);
            if (ImGui::Combo("Scale", &curScale, scaleLabels, scaleCount))
                app.SetUpscalerScale(scaleValues[curScale]);
        }

        // Mode — segmented.
        ImGui::TextUnformatted("Mode");
        const int curSr = up.GetMode() == 0 ? 0 : 1;
        const int pickSr = Segmented("srmode", curSr, "Quality", "Performance");
        if (pickSr != curSr) app.SetUpscalerMode(pickSr);

        if (!canToggle) ImGui::EndDisabled();

        if (nvOn && canToggle && up.OutputWidth() > 0) {
            ImGui::TextDisabled("Output: %dx%d", up.OutputWidth(), up.OutputHeight());
        }
    }

    // ---- Exit -------------------------------------------------------------
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.35f, 0.10f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.14f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.28f, 0.07f, 0.07f, 1.0f));
    if (ImGui::Button("Exit Application", ImVec2(-FLT_MIN, 30))) app.Quit();
    ImGui::PopStyleColor(3);

    ImGui::End();
    if (hidden) ImGui::PopStyleVar();

    // Render the ImGui data now so the renderer only has to call DrawData.
    ImGui::Render();
}
