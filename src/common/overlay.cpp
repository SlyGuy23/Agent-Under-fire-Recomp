// The settings panel on F10, or both sticks clicked on a controller. Its rows and look follow the F10 panel of
// SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa), cut down to the settings this SDK has.
#include "common/overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "common/sticks.h"
#include "core/input.h"

namespace overlay {

namespace {

std::atomic<bool> g_open{false};
std::atomic<int64_t> g_closed_at{0};  // ms; the button that closed the panel stays away from the game a moment
bool g_chord_held = false;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

bool Open() { return g_open.load(std::memory_order_relaxed); }

void ApplyVolume() { audio::g_master_volume.store(std::min<uint32_t>(g_master_volume, 100) / 100.0f); }

void Toggle() {
    bool open = !g_open.load(std::memory_order_relaxed);
    g_open.store(open, std::memory_order_relaxed);
    if (!open) g_closed_at.store(NowMs(), std::memory_order_relaxed);
}

void Pad(int port, input::Pad& pad) {
    sticks::Apply(pad);
    constexpr uint16_t kChord = 0x40 | 0x80;  // left and right thumb clicks
    if (port == 0) {
        bool held = (pad.buttons & kChord) == kChord;
        if (held && !g_chord_held) Toggle();
        g_chord_held = held;
    }
    bool settling = NowMs() - g_closed_at.load(std::memory_order_relaxed) < 300;
    if (Open() || settling || g_chord_held) {
        bool connected = pad.connected;
        pad = {};
        pad.connected = connected;
    }
}

}  // namespace overlay

#ifndef _WIN32
// the headless harness has no window or ImGui
namespace overlay {
void Draw(double) { g_frames++; }
bool Message(void*, uint32_t, uintptr_t, intptr_t, intptr_t*) { return false; }
}  // namespace overlay
#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <imgui.h>

namespace overlay {

namespace {

using app::DisplayMode;
using app::Settings;

// MI6 navy and steel, from the game's own menus
constexpr ImVec4 kPanelBg{0.03f, 0.05f, 0.10f, 0.94f};
constexpr ImVec4 kBorder{0.45f, 0.62f, 0.86f, 0.85f};
constexpr ImVec4 kText{0.88f, 0.91f, 0.96f, 1.0f};
constexpr ImVec4 kHighlight{0.14f, 0.25f, 0.45f, 1.0f};
constexpr ImVec4 kHeading{0.55f, 0.72f, 0.95f, 1.0f};
constexpr ImVec4 kDim{0.55f, 0.60f, 0.68f, 1.0f};
constexpr ImVec4 kButton{0.09f, 0.14f, 0.25f, 1.0f};
constexpr ImVec4 kButtonHovered{0.22f, 0.36f, 0.62f, 1.0f};
constexpr ImVec4 kLine{0.55f, 0.72f, 0.95f, 0.35f};
constexpr std::pair<ImGuiCol, ImVec4> kColors[] = {
    {ImGuiCol_WindowBg, kPanelBg}, {ImGuiCol_Border, kBorder}, {ImGuiCol_Text, kText},
    {ImGuiCol_Header, kHighlight}, {ImGuiCol_HeaderHovered, kHighlight}, {ImGuiCol_HeaderActive, kHighlight},
    {ImGuiCol_Button, kButton}, {ImGuiCol_ButtonHovered, kButtonHovered}, {ImGuiCol_ButtonActive, kButtonHovered},
    {ImGuiCol_ScrollbarGrab, kHighlight}, {ImGuiCol_ScrollbarGrabHovered, kButtonHovered},
    {ImGuiCol_ScrollbarGrabActive, kHeading}, {ImGuiCol_Separator, kLine}, {ImGuiCol_NavHighlight, kHeading},
};

int Step(int index, int step, int count) {
    if (count <= 0) return 0;
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

template <size_t N>
int Nearest(const uint32_t (&values)[N], uint32_t value) {
    int best = 0;
    for (int i = 1; i < int(N); i++) {
        uint32_t d = values[i] > value ? values[i] - value : value - values[i];
        uint32_t bd = values[best] > value ? values[best] - value : value - values[best];
        if (d < bd) best = i;
    }
    return best;
}

// stepped first when step is not zero
template <size_t N>
int StepTable(const uint32_t (&values)[N], uint32_t& value, int step) {
    int i = Nearest(values, value);
    if (step) value = values[i = Step(i, step, int(N))];
    return i;
}

struct Mode {
    uint32_t w, h;
};

// the primary monitor's modes, smallest first, so Left steps down; 0 x 0 first is the desktop's
const std::vector<Mode>& Modes() {
    static std::vector<Mode> modes = [] {
        std::vector<Mode> m{{0, 0}};
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
            if (dm.dmBitsPerPel < 32 || dm.dmPelsWidth < 640) continue;
            Mode mode{uint32_t(dm.dmPelsWidth), uint32_t(dm.dmPelsHeight)};
            if (std::none_of(m.begin(), m.end(), [&](const Mode& o) { return o.w == mode.w && o.h == mode.h; }))
                m.push_back(mode);
        }
        std::sort(m.begin() + 1, m.end(), [](const Mode& a, const Mode& b) { return a.w != b.w ? a.w < b.w : a.h < b.h; });
        return m;
    }();
    return modes;
}

void Section(const char* title) {
    float em = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, em * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, em * 0.2f));
}

// label left, an arrow each side of the value right; the row is the only nav stop. Left / Right (keys or D-pad)
// or the arrows step it: -1, 0 or 1.
int OptionRow(const char* label, const std::string& value, bool enabled = true) {
    const ImGuiStyle& style = ImGui::GetStyle();
    float em = ImGui::GetFontSize(), row_w = ImGui::GetContentRegionAvail().x, row_h = ImGui::GetFrameHeight();
    float value_w = em * 12, block_w = row_h * 2 + value_w + style.ItemSpacing.x * 2;
    float x = ImGui::GetCursorPosX(), y = ImGui::GetCursorPosY();
    ImGui::PushID(label);
    ImGui::BeginDisabled(!enabled);
    int step = 0;
    ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(row_w, row_h));
    if (ImGui::IsItemFocused()) {
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(ImGuiCol_Header));
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) step = 1;
    }
    ImGui::SetCursorPos(ImVec2(x + style.ItemInnerSpacing.x, y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);
    float block_x = x + row_w - block_w;
    ImGui::SetCursorPos(ImVec2(block_x, y));
    if (ImGui::ArrowButton("##dec", ImGuiDir_Left)) step = -1;
    float value_x = block_x + row_h + style.ItemSpacing.x;
    float pad = (value_w - ImGui::CalcTextSize(value.c_str()).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(value_x + std::max(pad, 0.0f), y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(value.c_str());
    ImGui::SetCursorPos(ImVec2(value_x + value_w + style.ItemSpacing.x, y));
    if (ImGui::ArrowButton("##inc", ImGuiDir_Right)) step = 1;
    ImGui::PopItemFlag();
    ImGui::EndDisabled();
    ImGui::PopID();
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::Dummy(ImVec2(row_w, row_h));
    return enabled ? step : 0;
}

const char* OnOff(bool on) { return on ? "ON" : "OFF"; }

bool FlipRow(const char* label, bool& on, bool enabled = true) {
    if (!OptionRow(label, OnOff(on), enabled)) return false;
    on = !on;
    return true;
}

constexpr uint32_t kRenderPercents[] = {50, 75, 100, 125, 150, 200};
constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, app::kFpsCapRefresh, 0};
constexpr uint32_t kAniso[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "16x"};
const char* const kModeNames[] = {"FULLSCREEN", "BORDERLESS", "WINDOWED"};
const char* const kDumpNames[] = {"PNG", "DDS"};

void Changed(bool display) {
    if (display) app::ApplyDisplay();
    app::Save();
}

void Rows() {
    Settings& s = app::Get();
    Section("DISPLAY");
    if (int st = OptionRow("DISPLAY MODE", kModeNames[std::min<uint32_t>(uint32_t(s.display_mode), 2)])) {
        s.display_mode = DisplayMode(Step(int(s.display_mode), st, 3));
        Changed(true);
    }
    {
        const std::vector<Mode>& modes = Modes();
        int i = 0;
        for (size_t k = 0; k < modes.size(); k++)
            if (modes[k].w == s.width && modes[k].h == s.height) i = int(k);
        char text[48];
        if (modes[size_t(i)].w) std::snprintf(text, sizeof(text), "%u x %u", modes[size_t(i)].w, modes[size_t(i)].h);
        else std::snprintf(text, sizeof(text), "DESKTOP");
        // borderless always covers the monitor at its desktop mode
        if (int st = OptionRow("RESOLUTION", text, s.display_mode != DisplayMode::Borderless)) {
            const Mode& m = modes[size_t(Step(i, st, int(modes.size())))];
            s.width = m.w;
            s.height = m.h;
            Changed(true);
        }
    }
    {
        char text[16];
        std::snprintf(text, sizeof(text), "%u%%", kRenderPercents[Nearest(kRenderPercents, s.render_percent)]);
        if (int st = OptionRow("RENDER RESOLUTION", text)) {
            StepTable(kRenderPercents, s.render_percent, st);
            Changed(false);
        }
    }
    if (FlipRow("VERTICAL SYNC", s.vsync)) Changed(false);
    {
        char text[24];
        uint32_t cap = kFpsCaps[Nearest(kFpsCaps, s.fps_cap)];
        if (cap == app::kFpsCapRefresh) std::snprintf(text, sizeof(text), "MONITOR REFRESH");
        else if (cap == 0) std::snprintf(text, sizeof(text), "UNLIMITED");
        else std::snprintf(text, sizeof(text), "%u", cap);
        if (int st = OptionRow("MAX FRAME RATE", text)) {
            StepTable(kFpsCaps, s.fps_cap, st);
            Changed(false);
        }
    }
    if (FlipRow("FPS COUNTER", g_fps_counter)) Changed(false);

    Section("GRAPHICS");
    if (int st = OptionRow("ANISOTROPIC FILTERING", kAnisoNames[Nearest(kAniso, s.aniso)])) {
        StepTable(kAniso, s.aniso, st);
        Changed(false);
    }

    Section("GENERAL");
    if (FlipRow("SKIP INTROS", g_skip_intros)) Changed(false);
    {
        char text[16];
        std::snprintf(text, sizeof(text), "%u%%", g_master_volume);
        if (int st = OptionRow("MASTER VOLUME", text)) {
            g_master_volume = uint32_t(std::clamp(int(g_master_volume) + st * 5, 0, 100));
            ApplyVolume();
            Changed(false);
        }
    }

    Section("CONTROLS");
    if (FlipRow("MODERN STICK RESPONSE", sticks::g_modern)) Changed(false);
    {
        char text[16];
        std::snprintf(text, sizeof(text), "%u%%", sticks::g_deadzone);
        if (int st = OptionRow("STICK DEADZONE", text, sticks::g_modern)) {
            sticks::g_deadzone = uint32_t(std::clamp(int(sticks::g_deadzone) + st * 5, 0, 30));
            Changed(false);
        }
    }

    Section("TEXTURES");
    if (FlipRow("TEXTURE REPLACEMENT", s.custom_textures)) Changed(false);
    if (FlipRow("TEXTURE DUMPING", s.dump_textures)) Changed(false);
    if (int st = OptionRow("DUMP FORMAT", kDumpNames[std::min<uint32_t>(s.texture_dump_format, 1)], s.dump_textures)) {
        s.texture_dump_format = uint32_t(Step(int(s.texture_dump_format), st, 2));
        Changed(false);
    }
}

void Defaults() {
    Settings& s = app::Get();
    Settings d;
    d.join_ip = s.join_ip;
    bool display = d.display_mode != s.display_mode || d.width != s.width || d.height != s.height;
    s = d;
    g_skip_intros = false;
    g_fps_counter = false;
    g_master_volume = 100;
    ApplyVolume();
    sticks::g_modern = true;
    sticks::g_deadzone = 10;
    Changed(display);
}

void Panel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * 42, em * 30), ImGuiCond_Always);
    for (const auto& [col, color] : kColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.6f, em * 1.2f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("##settings", nullptr, kFlags)) {
        const char* title = "SETTINGS";
        float title_pad = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title).x) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(title_pad, 0.0f));
        ImGui::TextUnformatted(title);
        float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + em;
        ImGui::BeginChild("##rows", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        Rows();
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("Left and Right change a setting. B, Escape or F10 closes.");
        ImGui::PopStyleColor();
        float bw = em * 8;
        float buttons_pad = ImGui::GetContentRegionAvail().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(buttons_pad, 0.0f));
        if (ImGui::Button("Back", ImVec2(bw, 0))) Toggle();
        ImGui::SameLine();
        if (ImGui::Button("Defaults", ImVec2(bw, 0))) Defaults();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(int(std::size(kColors)));
}

void FpsCounter(double fps) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f FPS", fps);
    float em = ImGui::GetFontSize();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 at(em * 0.6f, em * 0.4f);
    dl->AddText(ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(at, IM_COL32(255, 255, 255, 230), text);
}

}  // namespace

void Draw(double fps) {
    g_frames++;
    ImGuiIO& io = ImGui::GetIO();
    io.KeyRepeatRate = 0.1f;  // held arrows step ten times a second
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    if (Open()) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        io.MouseDrawCursor = false;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) Toggle();
        else Panel();
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }
    if (g_fps_counter) FpsCounter(fps);
}

bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t, intptr_t* result) {
    if (msg == WM_KEYDOWN && wp == VK_F10) {
        Toggle();
        *result = 0;
        return true;
    }
    // F10 is a system key in Windows: without this it also opens the window menu
    if ((msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && wp == VK_F10) {
        if (msg == WM_SYSKEYDOWN) Toggle();
        *result = 0;
        return true;
    }
    if (msg == WM_SETCURSOR && Open()) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        *result = TRUE;
        return true;
    }
    return false;
}

}  // namespace overlay
#endif
