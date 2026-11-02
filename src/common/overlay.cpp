// The settings panel on F10, or both sticks clicked on a controller. Its rows and look follow the F10 panel of
// SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa), cut down to the settings this SDK has.
#include "common/overlay.h"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "common/controls.h"
#include "common/damage_fx.h"
#include "common/fov.h"
#include "common/menus.h"
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

std::string FovDesc() { return "How much you see across the screen, in degrees on foot: zooms keep their power"; }

std::string DamageEffectsDesc() {
    constexpr const char* kDescs[damage_fx::kLevels] = {
        "The game's white flash and view kick when you are hit",
        "Half the view kick and a dimmer flash when you are hit",
        "A quarter of the view kick and a faint flash when you are hit",
        "No flash or view kick when you are hit",
    };
    return kDescs[damage_fx::Current()];
}

std::string DamageFlashDesc() {
    if (damage_fx::Current() == damage_fx::kOff) return "No flash while Damage Effects is Off";
    return damage_fx::g_flash_black ? "The flash when you are hit is dark, at the strength Damage Effects sets"
                                    : "The flash when you are hit is white, as the game draws it";
}

bool PackFolderOff(const std::wstring& folder) {
    auto is = [&](const wchar_t* want) {
        std::wstring w = want;
        return folder.size() == w.size() && std::equal(folder.begin(), folder.end(), w.begin(), [](wchar_t a, wchar_t b) {
                   return std::towlower(a) == std::towlower(b);
               });
    };
    if (is(kCharacterPack)) return !g_hd_characters;
    if (is(kEvilSummitPack)) return !g_hd_characters || !g_mission || g_mission() != kEvilSummit;
    if (is(kWeaponPack)) return !g_hd_weapons;
    for (const wchar_t* m : kMenuPacks)
        if (is(m)) return !g_hd_menus;
    return false;
}

void Toggle() {
    bool open = !g_open.load(std::memory_order_relaxed);
    g_open.store(open, std::memory_order_relaxed);
    if (!open) g_closed_at.store(NowMs(), std::memory_order_relaxed);
}

void Pad(int port, input::Pad& pad) {
    constexpr uint16_t kChord = 0x40 | 0x80;  // left and right thumb clicks, on the controller as it is
    bool chord = port == 0 && (pad.buttons & kChord) == kChord;
    // in place of sticks::Apply: remapping, the sticks, the keyboard and mouse (a neutral pad while this panel is open)
    controls::Pad(port, pad);
    if (!Open()) menus::Pad(port, pad);
    if (port == 0) {
        if (chord && !g_chord_held) Toggle();
        g_chord_held = chord;
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
std::vector<SettingRow> SettingRows() { return {}; }
void StepSetting(int, int) {}
void ResetSettings() {}
void ResetSettings(const std::vector<std::string>&) {}
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

constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, app::kFpsCapRefresh, 0};
constexpr uint32_t kAniso[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "16x"};
const char* const kAntiAliasingNames[] = {"OFF", "FXAA", "2x SSAA", "4x SSAA"};
const char* const kModeNames[] = {"FULLSCREEN", "BORDERLESS", "WINDOWED"};
const char* const kDumpNames[] = {"PNG", "DDS"};

void Changed(bool display) {
    if (display) app::ApplyDisplay();
    app::Save();
}

// one part of the texture pack on or off: the pack is listed again at the next frame
void TogglePack(bool& on) {
    on = !on;
    app::g_pack_folders++;
    Changed(false);
}

std::string PackDesc() {
    return app::Get().custom_textures ? "The upscaled textures replace the game's, in the parts turned on below"
                                      : "The game's own textures everywhere";
}
std::string CharacterPackDesc() {
    return g_hd_characters ? "Upscaled characters, and the keycards and business card they handle"
                           : "The game's own character textures";
}
std::string WeaponPackDesc() {
    return g_hd_weapons ? "Upscaled weapons and gadgets" : "The game's own weapon and gadget textures";
}
std::string MenuPackDesc() {
    return g_hd_menus ? "Upscaled menus, HUD, medals and loading screens" : "The game's own menu, HUD and loading screen art";
}

std::string FpsCapDesc() {
    uint32_t cap = kFpsCaps[Nearest(kFpsCaps, app::Get().fps_cap)];
    if (cap == app::kFpsCapRefresh) return "The game runs at most as many frames a second as the monitor shows";
    if (cap == 0) return "No limit: the game runs as many frames a second as the PC can draw";
    return "The game runs at most " + std::to_string(cap) + " frames a second";
}
std::string FpsCounterDesc() {
    return g_fps_counter ? "The frames a second shown in the top left corner" : "No frame counter on the screen";
}
std::string AnisoDesc() {
    uint32_t a = kAniso[Nearest(kAniso, app::Get().aniso)];
    if (a == 0) return "Textures filtered as the game asks";
    return "Floors and walls seen at an angle stay sharp further away (" + std::to_string(a) + "x)";
}
std::string AntiAliasingDesc() {
    constexpr const char* kDescs[] = {
        "No smoothing of jagged edges",
        "Jagged edges smoothed by a quick filter over the finished picture",
        "Drawn with twice the pixels and scaled down: smoother edges, more work for the GPU",
        "Drawn with four times the pixels and scaled down: the smoothest edges, the most work for the GPU",
    };
    return kDescs[std::min<uint32_t>(uint32_t(app::Get().anti_aliasing), 3)];
}

// one setting back to its default (the panel's Defaults does them all)
template <typename T>
void Default(T& value, T def) {
    value = def;
    Changed(false);
}
void DefaultPack(bool& on) {
    if (!on) TogglePack(on);
}

// one setting: shown by the F10 panel and the PC Settings page alike
struct Entry {
    const char* section;
    const char* label;
    std::string (*value)();
    void (*step)(int);  // -1 or 1
    bool (*enabled)();
    // optional: a line saying what the current value does, shown under the row on the PC Settings page while it is
    // selected
    std::string (*desc)() = nullptr;
    // optional: the setting alone back to its default (the pause menu's PC Settings page resets only its own rows)
    void (*reset)() = nullptr;
};

std::string Percent(uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof(text), "%u%%", v);
    return text;
}

int ModeIndex() {
    const Settings& s = app::Get();
    const std::vector<Mode>& modes = Modes();
    int i = 0;
    for (size_t k = 0; k < modes.size(); k++)
        if (modes[k].w == s.width && modes[k].h == s.height) i = int(k);
    return i;
}

const Entry kEntries[] = {
    {"DISPLAY", "DISPLAY MODE",
     [] { return std::string(kModeNames[std::min<uint32_t>(uint32_t(app::Get().display_mode), 2)]); },
     [](int st) {
         Settings& s = app::Get();
         s.display_mode = DisplayMode(Step(int(s.display_mode), st, 3));
         Changed(true);
     },
     [] { return true; }},
    {"DISPLAY", "RESOLUTION",
     [] {
         const Mode& m = Modes()[size_t(ModeIndex())];
         char text[48];
         if (m.w) std::snprintf(text, sizeof(text), "%u x %u", m.w, m.h);
         else std::snprintf(text, sizeof(text), "DESKTOP");
         return std::string(text);
     },
     [](int st) {
         Settings& s = app::Get();
         const std::vector<Mode>& modes = Modes();
         const Mode& m = modes[size_t(Step(ModeIndex(), st, int(modes.size())))];
         s.width = m.w;
         s.height = m.h;
         Changed(true);
     },
     // borderless always covers the monitor at its desktop mode
     [] { return app::Get().display_mode != DisplayMode::Borderless; }},
    {"DISPLAY", "VERTICAL SYNC", [] { return std::string(OnOff(app::Get().vsync)); },
     [](int) {
         app::Get().vsync = !app::Get().vsync;
         Changed(false);
     },
     [] { return true; }},
    {"DISPLAY", "MAX FRAME RATE",
     [] {
         uint32_t cap = kFpsCaps[Nearest(kFpsCaps, app::Get().fps_cap)];
         char text[24];
         if (cap == app::kFpsCapRefresh) std::snprintf(text, sizeof(text), "MONITOR REFRESH");
         else if (cap == 0) std::snprintf(text, sizeof(text), "UNLIMITED");
         else std::snprintf(text, sizeof(text), "%u", cap);
         return std::string(text);
     },
     [](int st) {
         StepTable(kFpsCaps, app::Get().fps_cap, st);
         Changed(false);
     },
     [] { return true; }, FpsCapDesc, [] { Default(app::Get().fps_cap, Settings{}.fps_cap); }},
    {"DISPLAY", "FPS COUNTER", [] { return std::string(OnOff(g_fps_counter)); },
     [](int) {
         g_fps_counter = !g_fps_counter;
         Changed(false);
     },
     [] { return true; }, FpsCounterDesc, [] { Default(g_fps_counter, false); }},
    {"GRAPHICS", "MENUS IN 4:3", [] { return std::string(OnOff(app::g_menus_4x3)); },
     [](int) {
         app::g_menus_4x3 = !app::g_menus_4x3;
         Changed(false);
     },
     [] { return true; }},
    {"GRAPHICS", "ANISOTROPIC FILTERING", [] { return std::string(kAnisoNames[Nearest(kAniso, app::Get().aniso)]); },
     [](int st) {
         StepTable(kAniso, app::Get().aniso, st);
         Changed(false);
     },
     [] { return true; }, AnisoDesc, [] { Default(app::Get().aniso, Settings{}.aniso); }},
    {"GRAPHICS", "ANTI-ALIASING",
     [] { return std::string(kAntiAliasingNames[std::min<uint32_t>(uint32_t(app::Get().anti_aliasing), 3)]); },
     [](int st) {
         Settings& s = app::Get();
         s.anti_aliasing = app::AntiAliasing(Step(int(s.anti_aliasing), st, 4));
         Changed(false);
     },
     [] { return true; }, AntiAliasingDesc, [] { Default(app::Get().anti_aliasing, Settings{}.anti_aliasing); }},
    {"GENERAL", "SKIP INTROS", [] { return std::string(OnOff(g_skip_intros)); },
     [](int) {
         g_skip_intros = !g_skip_intros;
         Changed(false);
     },
     [] { return true; }},
    {"GENERAL", "MASTER VOLUME", [] { return Percent(g_master_volume); },
     [](int st) {
         g_master_volume = uint32_t(std::clamp(int(g_master_volume) + st * 5, 0, 100));
         ApplyVolume();
         Changed(false);
     },
     [] { return true; }},
    // degrees added across the screen (the on-foot view's shown in brackets), applied at once on foot and in the
    // driving and on-rails missions; zooms and scopes keep their magnification (src/common/fov.h)
    {"GENERAL", "FIELD OF VIEW", [] { return fov::Label(); },
     [](int st) {
         fov::Step(st);
         Changed(false);
     },
     [] { return true; }, FovDesc, [] {
         fov::g_offset = 0;
         Changed(false);
     }},
    // every upscaled texture in the pack; the three rows under it each leave one part of the pack out
    {"TEXTURES", "UPSCALED TEXTURE REPLACEMENT", [] { return std::string(OnOff(app::Get().custom_textures)); },
     [](int) {
         app::Get().custom_textures = !app::Get().custom_textures;
         Changed(false);
     },
     [] { return true; }, PackDesc},
    {"TEXTURES", "CHARACTER TEXTURES", [] { return std::string(OnOff(g_hd_characters)); },
     [](int) { TogglePack(g_hd_characters); }, [] { return app::Get().custom_textures; }, CharacterPackDesc,
     [] { DefaultPack(g_hd_characters); }},
    {"TEXTURES", "WEAPON TEXTURES", [] { return std::string(OnOff(g_hd_weapons)); },
     [](int) { TogglePack(g_hd_weapons); }, [] { return app::Get().custom_textures; }, WeaponPackDesc,
     [] { DefaultPack(g_hd_weapons); }},
    {"TEXTURES", "MENU AND HUD TEXTURES", [] { return std::string(OnOff(g_hd_menus)); },
     [](int) { TogglePack(g_hd_menus); }, [] { return app::Get().custom_textures; }, MenuPackDesc,
     [] { DefaultPack(g_hd_menus); }},
    {"TEXTURES", "TEXTURE DUMPING", [] { return std::string(OnOff(app::Get().dump_textures)); },
     [](int) {
         app::Get().dump_textures = !app::Get().dump_textures;
         Changed(false);
     },
     [] { return true; }},
    {"TEXTURES", "DUMP FORMAT", [] { return std::string(kDumpNames[std::min<uint32_t>(app::Get().texture_dump_format, 1)]); },
     [](int st) {
         Settings& s = app::Get();
         s.texture_dump_format = uint32_t(Step(int(s.texture_dump_format), st, 2));
         Changed(false);
     },
     [] { return app::Get().dump_textures; }},
    // what being hit on foot does to the screen (src/common/damage_fx.h), applied at once
    {"ACCESSIBILITY", "DAMAGE EFFECTS", [] { return std::string(damage_fx::Name()); },
     [](int st) {
         damage_fx::g_level = uint32_t(Step(int(damage_fx::Current()), st, int(damage_fx::kLevels)));
         Changed(false);
     },
     [] { return true; }, DamageEffectsDesc, [] { Default(damage_fx::g_level, uint32_t(damage_fx::kDefault)); }},
    {"ACCESSIBILITY", "DAMAGE FLASH COLOUR", [] { return std::string(damage_fx::g_flash_black ? "BLACK" : "WHITE"); },
     [](int) {
         damage_fx::g_flash_black = !damage_fx::g_flash_black;
         Changed(false);
     },
     [] { return damage_fx::Current() != damage_fx::kOff; }, DamageFlashDesc, [] { Default(damage_fx::g_flash_black, false); }},
};

void Rows() {
    const char* section = nullptr;
    for (const Entry& e : kEntries) {
        if (!section || std::strcmp(section, e.section) != 0) Section(section = e.section);
        if (int st = OptionRow(e.label, e.value(), e.enabled())) e.step(st);
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
    app::g_menus_4x3 = true;
    if (!g_hd_characters || !g_hd_weapons || !g_hd_menus) app::g_pack_folders++;
    g_hd_characters = g_hd_weapons = g_hd_menus = true;
    g_master_volume = 100;
    fov::g_offset = 0;
    damage_fx::g_level = damage_fx::kDefault;
    damage_fx::g_flash_black = false;
    ApplyVolume();
    Changed(display);
}

}  // namespace

std::vector<SettingRow> SettingRows() {
    std::vector<SettingRow> rows;
    for (const Entry& e : kEntries) rows.push_back({e.section, e.label, e.value(), e.enabled(), e.desc ? e.desc() : std::string()});
    return rows;
}

void StepSetting(int index, int step) {
    if (index < 0 || index >= int(std::size(kEntries)) || !step || !kEntries[index].enabled()) return;
    kEntries[index].step(step);
}

void ResetSettings() { Defaults(); }

void ResetSettings(const std::vector<std::string>& labels) {
    for (const Entry& e : kEntries)
        if (e.reset && std::find(labels.begin(), labels.end(), e.label) != labels.end()) e.reset();
}

namespace {

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
    // the mission-only pack folder follows the mission played: the pack is listed again when it changes
    if (g_mission) {
        static uint32_t last = ~0u;
        uint32_t now = g_mission();
        if (now != last) {
            last = now;
            app::g_pack_folders++;
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    io.KeyRepeatRate = 0.1f;  // held arrows step ten times a second
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    if (Open()) {
        // the cursor stays hidden while the controller is in use (controls.cpp)
        if (controls::ActiveDevice() == controls::Device::KeyboardMouse) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        io.MouseDrawCursor = false;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) Toggle();
        else Panel();
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }
    if (!Open()) menus::Ui();
    if (g_fps_counter) FpsCounter(fps);
}

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    if (!Open() && menus::Message(hwnd, msg, wp, lp, result)) return true;
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
