#ifdef _WIN32
#include "common/menus_pages.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "common/overlay.h"

namespace menus {

namespace {

// the options list's places, in console pixels
// (the screen's title, OPTIONS at y 78, reads PC SETTINGS or CONTROLLER SETUP while a page is up: menus.cpp)
constexpr float kTitleY = 112, kTopY = 116, kRowStep = 22, kLabelX = 96, kValueX = 352, kTextSize = 16;
constexpr int kVisibleRows = 12;

// labels as the options list writes them, "Controller Setup": each word's first letter a capital, the rest small,
// short all-capital names (HUD, FPS, LT) kept
std::string Titled(const std::string& in) {
    std::string out = in;
    size_t i = 0;
    while (i < out.size()) {
        size_t j = out.find(' ', i);
        if (j == std::string::npos) j = out.size();
        std::string w = out.substr(i, j - i);
        static const char* kKeep[] = {"HUD", "HD", "FPS", "LT", "RT", "PC", "D-PAD", "4:3", "A", "B", "X", "Y", "UI"};
        bool keep = false;
        for (const char* k : kKeep) keep |= w == k;
        if (!keep) {
            bool first = true;
            for (size_t k = i; k < j; k++) {
                char& ch = out[k];
                if (std::isalpha(static_cast<unsigned char>(ch))) {
                    ch = first ? char(std::toupper(static_cast<unsigned char>(ch))) : char(std::tolower(static_cast<unsigned char>(ch)));
                    first = false;
                }
            }
        }
        i = j + 1;
    }
    return out;
}

struct Row {
    std::string label, value;
    bool enabled = true;
    bool heading = false;  // a section's name, not selectable
    std::string desc;      // a line saying what it does, shown under the row while it is selected
};

// what the mouse did to the list this frame
struct MouseHit {
    int row = -1;       // hovered row
    int step = 0;       // left click 1, right click -1, on the hovered row
    float wheel = 0;
};

ImU32 Dim(ImU32 c) { return (c & 0x00FFFFFF) | (uint32_t(0x78) << 24); }

// a list in the options screen's style: title, rows with values, scrolled to keep sel in view
MouseHit DrawList(const PageContext& c, const char* title, const std::vector<Row>& rows, int sel, int& scroll,
                  const char* hint) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    (void)title;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + kVisibleRows) scroll = sel - kVisibleRows + 1;
    scroll = std::clamp(scroll, 0, std::max(0, int(rows.size()) - kVisibleRows));
    MouseHit hit;
    ImGuiIO& io = ImGui::GetIO();
    float extra = 0;  // the selected row's description pushes the rows after it down, as on CONTROLLER SETUP
    for (int i = scroll; i < int(rows.size()) && i < scroll + kVisibleRows; i++) {
        const Row& r = rows[size_t(i)];
        float y = kTopY + kRowStep * float(i - scroll) + extra;
        if (i == sel && !r.desc.empty()) {
            c.say(dl, kLabelX + 8, y + 16, 11, Dim(c.lit), r.desc.c_str(), 0);
            extra += 13;
        }
        if (r.heading) {
            c.title(dl, kLabelX - 10, y + 5, 11, Dim(c.lit), r.label.c_str(), 0);
            continue;
        }
        ImU32 col = i == sel ? c.lit : (r.enabled ? c.unlit : Dim(c.unlit));
        c.say(dl, kLabelX, y, kTextSize, col, Titled(r.label).c_str(), 0);
        if (!r.value.empty()) c.say(dl, kValueX, y, kTextSize, i == sel ? c.lit : (r.enabled ? c.value : Dim(c.value)), r.value.c_str(), 0);
        ImVec2 a = c.guest(kLabelX - 12, y - 2), b = c.guest(600, y + kRowStep - 2);
        if (ImGui::IsMouseHoveringRect(a, b, false)) {
            hit.row = i;
            if (ImGui::IsMouseClicked(0)) hit.step = 1;
            if (ImGui::IsMouseClicked(1)) hit.step = -1;
        }
    }
    if (scroll > 0) c.title(dl, 320, kTopY - 12, 11, c.unlit, "^", 0.5f);
    if (scroll + kVisibleRows < int(rows.size())) c.title(dl, 320, kTopY + kRowStep * kVisibleRows - 2 + extra, 11, c.unlit, "v", 0.5f);
    if (hint) c.say(dl, 320, kTopY + kRowStep * kVisibleRows + 6 + extra, kTextSize - 4, Dim(c.unlit), hint, 0.5f);
    hit.wheel = io.MouseWheel;
    return hit;
}

// the next selectable row from sel in direction d
int NextRow(const std::vector<Row>& rows, int sel, int d) {
    int n = int(rows.size());
    for (int k = 0; k < n; k++) {
        sel = (sel + d + n) % n;
        if (!rows[size_t(sel)].heading) return sel;
    }
    return sel;
}

// ---------------------------------------------------------------------------------------------------------------
// PC SETTINGS: the F10 panel's rows, then RESET TO DEFAULTS

int g_pc_sel = -1, g_pc_scroll = 0;
ImVec2 g_last_mouse{-1, -1};

}  // namespace

bool PcSettingsPage(const PageContext& c, const std::vector<PageNav>& nav) {
    std::vector<overlay::SettingRow> settings = overlay::SettingRows();
    // rows: headings and settings, then reset; map rows to settings indexes
    std::vector<Row> rows;
    std::vector<int> index;
    std::string section;
    for (size_t i = 0; i < settings.size(); i++) {
        if (settings[i].section != section) {
            section = settings[i].section;
            rows.push_back({section, "", true, true});
            index.push_back(-1);
        }
        rows.push_back({settings[i].label, settings[i].value, settings[i].enabled, false, settings[i].desc});
        index.push_back(int(i));
    }
    rows.push_back({"RESET TO DEFAULTS", "", true, false});
    index.push_back(-2);
    if (g_pc_sel < 0 || g_pc_sel >= int(rows.size()) || rows[size_t(g_pc_sel)].heading) g_pc_sel = NextRow(rows, -1, 1);

    for (PageNav n : nav) {
        switch (n) {
            case PageNav::Up: g_pc_sel = NextRow(rows, g_pc_sel, -1); break;
            case PageNav::Down: g_pc_sel = NextRow(rows, g_pc_sel, 1); break;
            case PageNav::Left:
            case PageNav::Right:
            case PageNav::Accept: {
                int step = n == PageNav::Left ? -1 : 1;
                int k = index[size_t(g_pc_sel)];
                if (k >= 0) overlay::StepSetting(k, step);
                else if (k == -2 && n == PageNav::Accept) overlay::ResetSettings();
                break;
            }
            case PageNav::Back: g_pc_sel = -1; return false;
            default: break;
        }
    }
    MouseHit hit = DrawList(c, "PC SETTINGS", rows, g_pc_sel, g_pc_scroll, nullptr);
    ImVec2 m = ImGui::GetIO().MousePos;
    bool moved = m.x != g_last_mouse.x || m.y != g_last_mouse.y;
    g_last_mouse = m;
    if (hit.row >= 0 && !rows[size_t(hit.row)].heading) {
        if (moved) g_pc_sel = hit.row;
        if (hit.step) {
            int k = index[size_t(hit.row)];
            if (k >= 0) overlay::StepSetting(k, hit.step);
            else if (k == -2 && hit.step > 0) overlay::ResetSettings();
        }
    }
    if (hit.wheel != 0) g_pc_scroll = std::max(0, g_pc_scroll - int(hit.wheel));
    return true;
}

}  // namespace menus
#endif


#ifdef _WIN32
// ---------------------------------------------------------------------------------------------------------------
// CONTROLLER SETUP: four tabs (the controller and the keyboard and mouse, on foot and driving), each the actions with
// what gives them, then the device's options and a reset. Accept on an action waits for the input to give it.
#include <chrono>
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "common/controls.h"
#include "common/damage_fx.h"
#include "common/fov.h"

namespace menus {

namespace {

using controls::Action;
using controls::Context;
using controls::Device;

struct Tab {
    Context context;
    Device device;
};
// the tabs in LT / RT order: each device, on foot, driving and on rails
constexpr Tab kTabs[] = {{Context::OnFoot, Device::Controller},        {Context::Driving, Device::Controller},
                         {Context::OnRails, Device::Controller},       {Context::OnFoot, Device::KeyboardMouse},
                         {Context::Driving, Device::KeyboardMouse},    {Context::OnRails, Device::KeyboardMouse}};
constexpr int kTabCount = int(sizeof(kTabs) / sizeof(kTabs[0]));
constexpr const char* kDeviceNames[] = {"CONTROLLER", "KEYBOARD & MOUSE"};
constexpr const char* kContextNames[] = {"ON FOOT", "DRIVING", "ON RAILS"};

int TabOf(Context c, Device d) {
    for (int i = 0; i < kTabCount; i++)
        if (kTabs[i].context == c && kTabs[i].device == d) return i;
    return 0;
}

// Reset: the pause page's whole OPTIONS tab; ResetControls / ResetOptions: a Controller Setup tab's bindings or its
// options. Note: small text under the row before it
enum class RowKind { Action, Option, Reset, ResetControls, ResetOptions, Heading, Note };
enum class Opt { LeftSens, LeftDead, LeftOuter, RightSens, RightDead, RightOuter, MouseSens, MouseInvert, MouseLook,
                 CrouchToggle, AimToggle, SmartFire, KbmAutoAim, Fov, DamageFx, DamageFlash };

struct CRow {
    RowKind kind;
    Action action = Action::None;
    Opt opt = Opt::LeftSens;
    std::string label, value;
    std::string desc;  // a line saying what it does, shown under the row while it is selected
};

int g_tab = 0, g_sel = -1, g_scroll = 0, g_slot = 0;
bool g_capturing = false;
int64_t g_capture_at = 0;
int g_ended_frame = -10;  // the frame a capture ended: the input that ended it does nothing more
ImVec2 g_mouse_last{-1, -1};

int64_t Ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string Pct(uint32_t v) { return std::to_string(v) + "%"; }

// what the current value of an option does, for the line under its row
std::string SmartFireDesc(const controls::Options& o) {
    return o.smart_fire ? "With a gadget out RT uses it, rather than switching back to your weapon"
                        : "With a gadget out RT switches back to your weapon, as in the game";
}
std::string AutoAimDesc(const controls::Options& o) {
    return o.kbm_auto_aim ? "The game's aim assist pulls your aim onto enemies, on foot and on rails"
                          : "No aim assist: your aim stays where the mouse puts it";
}

// the mouse looks around where no key is bound to looking: on foot turning and looking up and down, on rails aiming
// driving looks nowhere with the mouse (its motion goes to no stick unless the settings file names one): its mouse
// options would change nothing
bool MouseMoves(Context c, const controls::Options& o) {
    return c != Context::Driving || o.mouse_x != controls::MouseAxis::None || o.mouse_y != controls::MouseAxis::None;
}

bool MouseLooks(Context c, Action a) {
    if (c == Context::OnFoot) return a >= Action::RSUp && a <= Action::RSRight;
    if (c == Context::OnRails) return a >= Action::LSUp && a <= Action::LSRight;
    return false;
}

std::string KeyText(const Tab& t, Action a, int slot) {
    std::string k = controls::BindingName(t.context, t.device, a, slot);
    if (k.empty() && slot == 0 && MouseLooks(t.context, a)) return "MOUSE";
    return k.empty() ? "-" : k;
}

std::vector<CRow> Rows(const Tab& t, bool view_only) {
    std::vector<CRow> rows;
    controls::Options o = controls::GetOptions(t.context);
    for (Action a : controls::Actions(t.context, t.device)) {
        CRow r{RowKind::Action, a};
        r.label = controls::ActionLabel(t.context, a);
        if (t.device == Device::Controller) {
            r.value = controls::BindingName(t.context, t.device, a, 0);
        } else {
            std::string k0 = KeyText(t, a, 0), k1 = controls::BindingName(t.context, t.device, a, 1);
            r.value = k1.empty() ? k0 : k0 + "  /  " + k1;
        }
        if (view_only && (r.value.empty() || r.label == "NOT USED")) continue;
        rows.push_back(r);
    }
    auto opt = [&](Opt k, const char* label, std::string value) {
        CRow r{RowKind::Option};
        r.opt = k;
        r.label = label;
        r.value = std::move(value);
        rows.push_back(r);
    };
    if (t.context == Context::OnFoot) {
        rows.push_back({RowKind::Heading, Action::None, Opt::LeftSens, "GENERAL", ""});
        opt(Opt::CrouchToggle, "CROUCH", o.crouch_toggle ? "TOGGLE" : "HOLD");
        opt(Opt::AimToggle, "MANUAL AIM", o.aim_toggle ? "TOGGLE" : "HOLD");
        if (t.device == Device::Controller && !view_only) {
            opt(Opt::SmartFire, "FIRE USES GADGET", o.smart_fire ? "ON" : "OFF");
            rows.back().desc = SmartFireDesc(o);
        }
    }
    if (t.device == Device::KeyboardMouse && !view_only && (t.context == Context::OnFoot || t.context == Context::OnRails)) {
        if (t.context == Context::OnRails) rows.push_back({RowKind::Heading, Action::None, Opt::LeftSens, "GENERAL", ""});
        opt(Opt::KbmAutoAim, "AUTO LOCK-ON", o.kbm_auto_aim ? "ON" : "OFF");
        rows.back().desc = AutoAimDesc(o);
    }
    if (view_only) return rows;
    bool mouse = MouseMoves(t.context, o);
    if (t.device == Device::Controller || mouse)
        rows.push_back({RowKind::Heading, Action::None, Opt::LeftSens, t.device == Device::Controller ? "STICKS" : "MOUSE", ""});
    if (t.device == Device::Controller) {
        const char* turn = "How fast the stick turns at each tilt: 200% is twice the game's own speed";
        const char* steer = "How far the stick steers at each tilt: at 200% it reaches full lock about half way";
        const char* dead = "How far from the centre the stick does nothing";
        const char* outer = "How far short of the edge the stick already counts as full tilt";
        opt(Opt::LeftSens, "LEFT STICK SENSITIVITY", Pct(o.left.sensitivity));
        rows.back().desc = t.context == Context::Driving ? steer : turn;
        opt(Opt::LeftDead, "LEFT STICK DEADZONE", Pct(o.left.deadzone));
        rows.back().desc = dead;
        opt(Opt::LeftOuter, "LEFT STICK OUTER DEADZONE", Pct(o.left.outer_deadzone));
        rows.back().desc = outer;
        opt(Opt::RightSens, "RIGHT STICK SENSITIVITY", Pct(o.right.sensitivity));
        if (t.context != Context::Driving) rows.back().desc = turn;
        opt(Opt::RightDead, "RIGHT STICK DEADZONE", Pct(o.right.deadzone));
        rows.back().desc = dead;
        opt(Opt::RightOuter, "RIGHT STICK OUTER DEADZONE", Pct(o.right.outer_deadzone));
        rows.back().desc = outer;
    } else if (mouse) {
        opt(Opt::MouseSens, "MOUSE SENSITIVITY", Pct(o.mouse_sensitivity));
        opt(Opt::MouseInvert, "INVERT MOUSE Y", o.mouse_invert_y ? "ON" : "OFF");
        const char* looks[] = {"AUTOMATIC", "DIRECT", "AS A STICK"};
        opt(Opt::MouseLook, "MOUSE LOOK", looks[std::min<int>(int(o.mouse_look), 2)]);
    }
    rows.push_back({RowKind::ResetControls, Action::None, Opt::LeftSens, "RESET CONTROLS TO DEFAULTS", ""});
    rows.back().desc = t.device == Device::Controller ? "This tab's buttons and sticks back to the defaults"
                                                      : "This tab's keys and mouse buttons back to the defaults";
    rows.push_back({RowKind::ResetOptions, Action::None, Opt::LeftSens, "RESET OPTIONS TO DEFAULTS", ""});
    rows.back().desc = "This tab's options above back to the defaults, its controls kept";
    return rows;
}

void StepOption(Context c, Opt k, int step) {
    // the PC Settings ones the pause page offers too: the same settings, saved the same way
    switch (k) {
        case Opt::Fov: fov::Step(step); app::Save(); return;
        case Opt::DamageFx:
            damage_fx::g_level = uint32_t((int(damage_fx::Current()) + step + int(damage_fx::kLevels)) % int(damage_fx::kLevels));
            app::Save();
            return;
        case Opt::DamageFlash:
            if (damage_fx::Current() == damage_fx::kOff) return;
            damage_fx::g_flash_black = !damage_fx::g_flash_black;
            app::Save();
            return;
        default: break;
    }
    controls::Options o = controls::GetOptions(c);
    auto bump = [&](uint32_t& v, int by, int lo, int hi) { v = uint32_t(std::clamp(int(v) + step * by, lo, hi)); };
    switch (k) {
        case Opt::LeftSens: bump(o.left.sensitivity, 10, 50, 200); break;
        case Opt::LeftDead: bump(o.left.deadzone, 1, 0, 30); break;
        case Opt::LeftOuter: bump(o.left.outer_deadzone, 1, 0, 30); break;
        case Opt::RightSens: bump(o.right.sensitivity, 10, 50, 200); break;
        case Opt::RightDead: bump(o.right.deadzone, 1, 0, 30); break;
        case Opt::RightOuter: bump(o.right.outer_deadzone, 1, 0, 30); break;
        case Opt::MouseSens: bump(o.mouse_sensitivity, o.mouse_sensitivity >= 200 ? 25 : 5, 5, 1000); break;
        case Opt::MouseInvert: o.mouse_invert_y = !o.mouse_invert_y; break;
        case Opt::MouseLook: o.mouse_look = controls::MouseLook((int(o.mouse_look) + step + 3) % 3); break;
        case Opt::CrouchToggle: o.crouch_toggle = !o.crouch_toggle; break;
        case Opt::AimToggle: o.aim_toggle = !o.aim_toggle; break;
        case Opt::SmartFire: o.smart_fire = !o.smart_fire; break;
        case Opt::KbmAutoAim: o.kbm_auto_aim = !o.kbm_auto_aim; break;
    }
    controls::SetOptions(c, o);
    app::Save();
}

// a Controller Setup tab's bindings: its device in its context
void ResetControls(const Tab& t) {
    controls::ResetBindings(t.context, t.device);
    app::Save();
}

// the options a Controller Setup tab lists: its device's sticks or mouse, and the context's general ones
void ResetTabOptions(const Tab& t) {
    controls::Options o = controls::GetOptions(t.context), d = controls::DefaultOptions(t.context);
    if (t.device == Device::Controller) {
        o.left = d.left;
        o.right = d.right;
    } else {
        o.mouse_sensitivity = d.mouse_sensitivity;
        o.mouse_invert_y = d.mouse_invert_y;
        o.mouse_look = d.mouse_look;
        o.mouse_x = d.mouse_x;
        o.mouse_y = d.mouse_y;
    }
    o.crouch_toggle = d.crouch_toggle;
    o.aim_toggle = d.aim_toggle;
    if (t.device == Device::Controller) o.smart_fire = d.smart_fire;
    else o.kbm_auto_aim = d.kbm_auto_aim;
    controls::SetOptions(t.context, o);
    app::Save();
}

int Next(const std::vector<CRow>& rows, int sel, int d) {
    int n = int(rows.size());
    for (int k = 0; k < n; k++) {
        sel = (sel + d + n) % n;
        if (rows[size_t(sel)].kind != RowKind::Heading && rows[size_t(sel)].kind != RowKind::Note) return sel;
    }
    return sel;
}

void Activate(const Tab& t, const CRow& r, int step) {
    if (r.kind == RowKind::Option) StepOption(t.context, r.opt, step);
    else if (r.kind == RowKind::ResetControls && step > 0) ResetControls(t);
    else if (r.kind == RowKind::ResetOptions && step > 0) ResetTabOptions(t);
    else if (r.kind == RowKind::Action && step > 0) {
        controls::BeginCapture(t.context, t.device, r.action, t.device == Device::Controller ? 0 : g_slot);
        g_capturing = true;
        g_capture_at = Ms();
    }
}

// the two rows of tabs: the devices, then on foot, driving and on rails; the shown ones lit. Returns a clicked tab,
// -1 for none. only: one context alone (the pause menu's page)
float g_tabs_y = kTitleY, g_rows_y = kTitleY + 42;  // where the tabs and the rows start on the page drawn
constexpr int kControlRows = 11;

int DrawTabs(const PageContext& c, ImDrawList* dl, int tab, bool clickable, int only = -1) {
    int clicked = -1;
    const Tab& t = kTabs[tab];
    auto item = [&](float& x, float y, float size, const char* name, bool lit, int to) {
        // in the stats screen's way: capitals and small letters, "Keyboard & Mouse"
        std::string titled = Titled(name);
        const char* text = titled.c_str();
        c.title(dl, x, y, size + 2, lit ? c.lit : Dim(c.unlit), text, 0);
        float w = c.title_width(size + 2, text);
        ImVec2 a = c.guest(x - 3, y - 2), b = c.guest(x + w + 3, y + size + 2);
        if (clickable && ImGui::IsMouseHoveringRect(a, b, false) && ImGui::IsMouseClicked(0)) clicked = to;
        x += w + 18;
    };
    float x = kLabelX;
    for (int d = 0; d < 2; d++) item(x, g_tabs_y, 13, kDeviceNames[d], int(t.device) == d, TabOf(only >= 0 ? Context(only) : t.context, Device(d)));
    if (only < 0) {
        x = kLabelX;
        for (int k = 0; k < 3; k++) item(x, g_tabs_y + 18, 11, kContextNames[k], int(t.context) == k, TabOf(Context(k), t.device));
    } else {
        x = kLabelX;
        item(x, g_tabs_y + 18, 11, kContextNames[only], true, tab);
    }
    return clicked;
}

// the rows from kTopY, scrolled to keep sel in view; the hovered row (or -1) through hover
void DrawRows(const PageContext& c, ImDrawList* dl, const std::vector<CRow>& rows, int sel, int scroll, bool keyboard,
              const std::string& sel_value, int* hover, float value_x) {
    constexpr float kStep = 21;
    float extra = 0;  // the selected row's description pushes the rows after it down
    for (int i = scroll; i < int(rows.size()) && i < scroll + kControlRows; i++) {
        const CRow& r = rows[size_t(i)];
        float y = g_rows_y + kStep * float(i - scroll) + extra;
        if (i == sel && !r.desc.empty()) {
            c.say(dl, kLabelX + 8, y + 16, 11, Dim(c.lit), r.desc.c_str(), 0);
            extra += 13;
        }
        if (r.kind == RowKind::Heading) {
            c.title(dl, kLabelX - 10, y + 5, 11, Dim(c.lit), r.label.c_str(), 0);
            continue;
        }
        if (r.kind == RowKind::Note) {
            c.say(dl, kLabelX + 8, y - 3, 11, Dim(c.unlit), r.label.c_str(), 0);
            continue;
        }
        bool lit = i == sel;
        std::string value = lit && !sel_value.empty() ? sel_value : r.value;
        c.say(dl, kLabelX, y, kTextSize - 1, lit ? c.lit : c.unlit, Titled(r.label).c_str(), 0);
        // a long value is drawn smaller to stay inside the screen
        float size = kTextSize - 1;
        while (size > 10 && value_x + c.width(size, value.c_str()) > 604) size -= 1;
        c.say(dl, value_x, y + (kTextSize - 1 - size) * 0.7f, size, lit ? c.lit : c.value, value.c_str(), 0);
        ImVec2 a = c.guest(kLabelX - 12, y - 2), b = c.guest(620, y + kStep - 2);
        if (hover && ImGui::IsMouseHoveringRect(a, b, false)) *hover = i;
    }
    (void)keyboard;
    if (scroll > 0) c.title(dl, 320, g_rows_y - 12, 11, c.unlit, "^", 0.5f);
    if (scroll + kControlRows < int(rows.size())) c.title(dl, 320, g_rows_y + kStep * kControlRows - 2, 11, c.unlit, "v", 0.5f);
}

int Clamp(int scroll, int sel, int n) {
    if (sel >= 0 && sel < scroll) scroll = sel;
    if (sel >= scroll + kControlRows) scroll = sel - kControlRows + 1;
    return std::clamp(scroll, 0, std::max(0, n - kControlRows));
}

int g_view_tab = 0, g_view_scroll = 0;

}  // namespace

bool PageMessage(uint32_t, uintptr_t) { return false; }

namespace {

// the controls as a grid: two halves side by side, each row an action and what gives it; on the keyboard and mouse
// tab a row has two keys. sel: the selected row (-1 none), slot its key; hover gets the row and key under the mouse.
struct GridHit {
    int row = -1, slot = 0;
};
constexpr float kGridRow = 17, kGridHalf = 262, kGridGap = 12, kGridText = 14;

GridHit DrawGrid(const PageContext& c, ImDrawList* dl, const std::vector<CRow>& rows, float top, int sel, int slot,
                 bool keyboard, const std::string& waiting, const Tab& t) {
    GridHit hit;
    int n = int(rows.size());
    int per = std::max(1, (n + 1) / 2);
    float x0 = 320 - (kGridHalf * 2 + kGridGap) / 2;
    dl->AddRectFilled(c.guest(x0 - 6, top - 4), c.guest(x0 + kGridHalf * 2 + kGridGap + 6, top + 20 + kGridRow * float(per)),
                      IM_COL32(4, 8, 24, 225), 4 * c.scale());
    for (int h = 0; h < 2; h++) {
        float x = x0 + float(h) * (kGridHalf + kGridGap);
        c.title(dl, x + 6, top, 11, Dim(c.lit), "Action", 0);
        c.title(dl, x + kGridHalf - 6, top, 11, Dim(c.lit), "Control", 1);
    }
    ImVec2 m = ImGui::GetIO().MousePos;
    for (int i = 0; i < n; i++) {
        int h = i / per, k = i % per;
        float x = x0 + float(h) * (kGridHalf + kGridGap), y = top + 18 + kGridRow * float(k);
        bool lit = i == sel;
        if (lit) dl->AddRectFilled(c.guest(x, y - 1), c.guest(x + kGridHalf, y + kGridRow - 1), IM_COL32(120, 170, 255, 60),
                                  3 * c.scale());
        const CRow& r = rows[size_t(i)];
        std::string label = Titled(r.label), value;
        if (lit && !waiting.empty()) value = waiting;
        else if (keyboard) {
            std::string k0 = KeyText(t, r.action, 0), k1 = controls::BindingName(t.context, t.device, r.action, 1);
            if (k1.empty()) k1 = "-";
            value = lit ? (slot == 0 ? "[" + k0 + "]  /  " + k1 : k0 + "  /  [" + k1 + "]") : (k1 == "-" ? k0 : k0 + "  /  " + k1);
        } else {
            value = r.value.empty() ? "-" : r.value;
        }
        float vs = kGridText, ls = kGridText;
        while (vs > 9 && c.width(vs, value.c_str()) > kGridHalf * 0.58f) vs -= 1;
        float room = kGridHalf - 18 - c.width(vs, value.c_str());
        while (ls > 9 && c.width(ls, label.c_str()) > room) ls -= 1;
        c.say(dl, x + 6, y + (kGridText - ls) * 0.7f, ls, lit ? c.lit : c.unlit, label.c_str(), 0);
        c.say(dl, x + kGridHalf - 6, y + (kGridText - vs) * 0.7f, vs, c.lit, value.c_str(), 1);
        ImVec2 a = c.guest(x, y - 1), b = c.guest(x + kGridHalf, y + kGridRow - 1);
        if (m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y) {
            hit.row = i;
            // the keyboard's two keys: the left one left of the value's middle
            float mid = x + kGridHalf - 6 - c.width(vs, value.c_str()) / 2;
            hit.slot = keyboard && m.x > c.guest(mid, y).x ? 1 : 0;
        }
    }
    return hit;
}

int g_section = 0;   // 0 the grid of controls, 1 the list of general and stick or mouse options
int g_cell = 0;      // the grid's selected row
int g_opt_sel = -1;  // the list's selected row

}  // namespace

bool ControlsPage(const PageContext& c, const std::vector<PageNav>& nav) {
    const Tab& tab = kTabs[g_tab];
    // waiting for an input to bind: the page stands still until it comes (a controller gives up after 6 seconds)
    if (g_capturing) {
        controls::CaptureState st = controls::Capture();
        if (st == controls::CaptureState::Waiting && tab.device == Device::Controller && Ms() - g_capture_at > 6000)
            controls::CancelCapture(), st = controls::Capture();
        if (st != controls::CaptureState::Waiting) {
            controls::EndCapture();
            g_capturing = false;
            g_ended_frame = ImGui::GetFrameCount();
            if (st == controls::CaptureState::Bound) app::Save();
        }
    }
    bool settling = ImGui::GetFrameCount() - g_ended_frame < 3;
    auto split = [&](const Tab& t, std::vector<CRow>& actions, std::vector<CRow>& options) {
        actions.clear(), options.clear();
        for (const CRow& r : Rows(t, false)) (r.kind == RowKind::Action ? actions : options).push_back(r);
    };
    std::vector<CRow> actions, options;
    split(tab, actions, options);
    int per = std::max(1, (int(actions.size()) + 1) / 2);
    g_cell = std::clamp(g_cell, 0, std::max(0, int(actions.size()) - 1));
    if (g_opt_sel < 0 || g_opt_sel >= int(options.size()) || options[size_t(g_opt_sel)].kind == RowKind::Heading)
        g_opt_sel = Next(options, -1, 1);
    bool keyboard = tab.device == Device::KeyboardMouse;
    auto change_tab = [&](int to) {
        g_tab = (to + kTabCount) % kTabCount;
        g_cell = 0, g_slot = 0, g_section = 0, g_scroll = 0;
    };
    if (!g_capturing && !settling) {
        for (PageNav n : nav) {
            if (n == PageNav::Back) {
                g_section = 0;
                return false;
            }
            if (n == PageNav::PrevTab || n == PageNav::NextTab) {
                change_tab(g_tab + (n == PageNav::NextTab ? 1 : -1));
                split(kTabs[g_tab], actions, options);
                per = std::max(1, (int(actions.size()) + 1) / 2);
                continue;
            }
            const Tab& t = kTabs[g_tab];
            if (g_section == 0) {
                int h = g_cell / per, k = g_cell % per;
                switch (n) {
                    case PageNav::Up:
                        if (k > 0) g_cell--;
                        break;
                    case PageNav::Down:
                        if (k + 1 < per && g_cell + 1 < int(actions.size())) g_cell++;
                        else g_section = 1, g_opt_sel = Next(options, -1, 1);
                        break;
                    case PageNav::Left:
                        if (keyboard && g_slot == 1) g_slot = 0;
                        else if (h > 0) g_cell -= per, g_slot = keyboard ? 1 : 0;
                        break;
                    case PageNav::Right:
                        if (keyboard && g_slot == 0) g_slot = 1;
                        else if (h == 0 && g_cell + per < int(actions.size())) g_cell += per, g_slot = 0;
                        break;
                    case PageNav::Accept:
                        if (!actions.empty()) Activate(t, actions[size_t(g_cell)], 1);
                        break;
                    default: break;
                }
            } else {
                const CRow& r = options[size_t(g_opt_sel)];
                switch (n) {
                    case PageNav::Up: {
                        int first = Next(options, -1, 1);
                        if (g_opt_sel == first) g_section = 0, g_cell = std::min(per - 1, int(actions.size()) - 1);
                        else g_opt_sel = Next(options, g_opt_sel, -1);
                        break;
                    }
                    case PageNav::Down: {
                        int nx = Next(options, g_opt_sel, 1);
                        if (nx > g_opt_sel) g_opt_sel = nx;
                        break;
                    }
                    case PageNav::Left:
                    case PageNav::Right:
                        if (r.kind == RowKind::Option) StepOption(t.context, r.opt, n == PageNav::Left ? -1 : 1);
                        break;
                    case PageNav::Accept: Activate(t, r, 1); break;
                    default: break;
                }
            }
            if (g_capturing) break;
        }
    }
    const Tab& shown = kTabs[g_tab];
    split(shown, actions, options);
    keyboard = shown.device == Device::KeyboardMouse;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    g_tabs_y = 112;
    int clicked = DrawTabs(c, dl, g_tab, !g_capturing);
    if (clicked >= 0 && clicked != g_tab) change_tab(clicked);
    ImVec2 delta = ImGui::GetIO().MouseDelta;
    bool moved = delta.x != 0 || delta.y != 0;
    bool idle = !g_capturing && !settling;
    if (g_section == 0) {
        std::string waiting = g_capturing ? (keyboard ? "PRESS A KEY" : "PRESS A BUTTON") : "";
        GridHit hit = DrawGrid(c, dl, actions, 152, g_cell, g_slot, keyboard, waiting, shown);
        float bottom = 152 + 20 + kGridRow * float(std::max(1, (int(actions.size()) + 1) / 2));
        c.title(dl, 320, bottom + 4, 11, Dim(c.unlit), "v  General & Sticks  v", 0.5f);
        if (idle && hit.row >= 0) {
            if (moved) g_cell = hit.row, g_slot = hit.slot;
            if (ImGui::IsMouseClicked(0)) g_cell = hit.row, g_slot = hit.slot, Activate(shown, actions[size_t(hit.row)], 1);
            else if (ImGui::IsMouseClicked(1) && keyboard)
                controls::SetKeyBinding(shown.context, actions[size_t(hit.row)].action, hit.slot, controls::kNoKey), app::Save();
        }
        ImVec2 a = c.guest(200, bottom), b = c.guest(440, bottom + 18);
        if (idle && ImGui::IsMouseHoveringRect(a, b, false) && ImGui::IsMouseClicked(0)) g_section = 1;
        if (idle && ImGui::GetIO().MouseWheel < 0) g_section = 1;
    } else {
        c.title(dl, 320, 146, 11, Dim(c.unlit), "^  Controls  ^", 0.5f);
        ImVec2 a = c.guest(220, 144), b = c.guest(420, 160);
        if (idle && ImGui::IsMouseHoveringRect(a, b, false) && ImGui::IsMouseClicked(0)) g_section = 0;
        g_rows_y = 166;
        int hover = -1;
        g_scroll = Clamp(g_scroll, g_opt_sel, int(options.size()));
        DrawRows(c, dl, options, g_opt_sel, g_scroll, keyboard, "", idle ? &hover : nullptr, kValueX + 20);
        if (hover >= 0 && options[size_t(hover)].kind != RowKind::Heading) {
            if (moved) g_opt_sel = hover;
            const CRow& r = options[size_t(hover)];
            if (ImGui::IsMouseClicked(0)) Activate(shown, r, 1);
            else if (ImGui::IsMouseClicked(1) && r.kind == RowKind::Option) StepOption(shown.context, r.opt, -1);
        }
        float wheel = ImGui::GetIO().MouseWheel;
        if (idle && wheel > 0 && g_scroll == 0) g_section = 0;
        else if (idle && wheel != 0) g_scroll = std::max(0, g_scroll - int(wheel));
    }
    return true;
}

namespace {

// a table of a context's controls for a device: halves side by side, each an ACTION and a CONTROL column, a row a
// control, on a dark panel so the text stands off whatever is behind it
void DrawTable(const PageContext& c, ImDrawList* dl, controls::Context context, Device device, float top, int max_rows) {
    const Tab& t = kTabs[TabOf(context, device)];
    std::vector<CRow> rows = Rows(t, true);
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const CRow& r) { return r.kind != RowKind::Action; }), rows.end());
    int per = std::max(1, std::min(max_rows, (int(rows.size()) + 1) / 2));
    if (int(rows.size()) <= max_rows / 2) per = int(rows.size());
    constexpr float kRow = 17, kHalf = 262, kGap = 12, kText = 14;  // inside the pause page's panel (40 to 600)
    int halves = (int(rows.size()) + per - 1) / per;
    float x0 = 320 - (kHalf * float(halves) + kGap * float(halves - 1)) / 2;
    float height = 18 + kRow * float(per) + 2;
    dl->AddRectFilled(c.guest(x0 - 6, top - 4), c.guest(x0 + kHalf * float(halves) + kGap * float(halves - 1) + 6, top + height),
                      IM_COL32(4, 8, 24, 225), 4 * c.scale());
    for (int h = 0; h < halves; h++) {
        float x = x0 + float(h) * (kHalf + kGap);
        c.title(dl, x + 6, top, 10, Dim(c.lit), "ACTION", 0);
        c.title(dl, x + kHalf - 6, top, 10, Dim(c.lit), "CONTROL", 1);
    }
    for (int i = 0; i < int(rows.size()); i++) {
        int h = i / per, k = i % per;
        float x = x0 + float(h) * (kHalf + kGap), yy = top + 18 + kRow * float(k);
        std::string label = Titled(rows[size_t(i)].label), value = rows[size_t(i)].value;
        // the two share the row: the value keeps its size where it can, the label gives way first
        float vs = kText, ls = kText;
        while (vs > 10 && c.width(vs, value.c_str()) > kHalf * 0.55f) vs -= 1;
        float room = kHalf - 18 - c.width(vs, value.c_str());
        while (ls > 10 && c.width(ls, label.c_str()) > room) ls -= 1;
        c.say(dl, x + 6, yy + (kText - ls) * 0.7f, ls, c.unlit, label.c_str(), 0);
        c.say(dl, x + kHalf - 6, yy + (kText - vs) * 0.7f, vs, c.lit, value.c_str(), 1);
    }
}

// the pause page's OPTIONS tab: what can be changed in a mission, for this context
std::vector<CRow> PauseOptions(controls::Context context) {
    std::vector<CRow> rows;
    controls::Options o = controls::GetOptions(context);
    auto opt = [&](Opt k, const char* label, std::string value) {
        CRow r{RowKind::Option};
        r.opt = k;
        r.label = label;
        r.value = std::move(value);
        rows.push_back(r);
    };
    auto heading = [&](const char* name) { rows.push_back({RowKind::Heading, Action::None, Opt::LeftSens, name, ""}); };
    if (context == Context::OnFoot) {
        heading("GENERAL");
        opt(Opt::CrouchToggle, "CROUCH", o.crouch_toggle ? "TOGGLE" : "HOLD");
        opt(Opt::AimToggle, "MANUAL AIM", o.aim_toggle ? "TOGGLE" : "HOLD");
        opt(Opt::SmartFire, "FIRE USES GADGET (CONTROLLER)", o.smart_fire ? "ON" : "OFF");
        rows.back().desc = SmartFireDesc(o);
    }
    if (context == Context::OnRails) heading("GENERAL");
    if (context == Context::OnFoot || context == Context::OnRails) {
        opt(Opt::KbmAutoAim, "AUTO LOCK-ON (KEYBOARD & MOUSE)", o.kbm_auto_aim ? "ON" : "OFF");
        rows.back().desc = AutoAimDesc(o);
    }
    // the view and what being hit does, as in PC Settings (the on-rails missions keep the game's view and have no
    // damage flash, the driving ones only the view)
    if (context != Context::OnRails) {
        heading("DISPLAY");
        opt(Opt::Fov, "FIELD OF VIEW", fov::Label());
        rows.back().desc = overlay::FovDesc();
    }
    if (context == Context::OnFoot) {
        opt(Opt::DamageFx, "DAMAGE EFFECTS", damage_fx::Name());
        rows.back().desc = overlay::DamageEffectsDesc();
        opt(Opt::DamageFlash, "DAMAGE FLASH COLOUR", damage_fx::g_flash_black ? "BLACK" : "WHITE");
        rows.back().desc = overlay::DamageFlashDesc();
    }
    heading("CONTROLLER");
    opt(Opt::LeftSens, "LEFT STICK SENSITIVITY", Pct(o.left.sensitivity));
    opt(Opt::LeftDead, "LEFT STICK DEADZONE", Pct(o.left.deadzone));
    opt(Opt::LeftOuter, "LEFT STICK OUTER DEADZONE", Pct(o.left.outer_deadzone));
    opt(Opt::RightSens, "RIGHT STICK SENSITIVITY", Pct(o.right.sensitivity));
    opt(Opt::RightDead, "RIGHT STICK DEADZONE", Pct(o.right.deadzone));
    opt(Opt::RightOuter, "RIGHT STICK OUTER DEADZONE", Pct(o.right.outer_deadzone));
    if (MouseMoves(context, o)) {
        heading("MOUSE");
        opt(Opt::MouseSens, "MOUSE SENSITIVITY", Pct(o.mouse_sensitivity));
        opt(Opt::MouseInvert, "INVERT MOUSE Y", o.mouse_invert_y ? "ON" : "OFF");
    }
    rows.push_back({RowKind::Reset, Action::None, Opt::LeftSens, "RESET TO DEFAULTS", ""});
    rows.back().desc = "Everything on this tab back to the defaults";
    return rows;
}

// the pause page's RESET TO DEFAULTS: what its OPTIONS tab shows for this context, back to the defaults (the
// context's general, stick and mouse options; the field of view; the damage effects)
void ResetPauseOptions(Context context) {
    controls::Options o = controls::GetOptions(context), d = controls::DefaultOptions(context);
    o.crouch_toggle = d.crouch_toggle;
    o.aim_toggle = d.aim_toggle;
    o.smart_fire = d.smart_fire;
    o.kbm_auto_aim = d.kbm_auto_aim;
    o.left = d.left;
    o.right = d.right;
    o.mouse_sensitivity = d.mouse_sensitivity;
    o.mouse_invert_y = d.mouse_invert_y;
    controls::SetOptions(context, o);
    if (context != Context::OnRails) fov::g_offset = 0;
    if (context == Context::OnFoot) {
        damage_fx::g_level = damage_fx::kDefault;
        damage_fx::g_flash_black = false;
    }
    app::Save();
}

// a row of the pause page's OPTIONS tab changed (left -1, right or accept 1)
void ActivatePause(Context context, const CRow& r, int step) {
    if (r.kind == RowKind::Option) StepOption(context, r.opt, step);
    else if (r.kind == RowKind::Reset && step > 0) ResetPauseOptions(context);
}

int g_pause_sel = -1, g_pause_scroll = 0;

}  // namespace

// the pause menu's page: tabs CONTROLLER and KEYBOARD & MOUSE show the context's controls as a table, OPTIONS the
// toggles, sensitivities and deadzones to change. LT / RT (Q / E) or a click changes the tab.
bool ControlsViewPage(const PageContext& c, const std::vector<PageNav>& nav, controls::Context context, bool opened) {
    constexpr const char* kNames[] = {"CONTROLLER", "KEYBOARD & MOUSE", "OPTIONS"};
    if (opened) g_view_tab = controls::ActiveDevice() == Device::Controller ? 0 : 1, g_pause_sel = -1, g_pause_scroll = 0;
    g_view_tab = std::clamp(g_view_tab, 0, 2);
    std::vector<CRow> rows = PauseOptions(context);
    if (g_pause_sel < 0 || g_pause_sel >= int(rows.size()) ||
        (rows[size_t(g_pause_sel)].kind != RowKind::Option && rows[size_t(g_pause_sel)].kind != RowKind::Reset))
        g_pause_sel = Next(rows, -1, 1);
    for (PageNav n : nav) {
        switch (n) {
            case PageNav::PrevTab: g_view_tab = (g_view_tab + 2) % 3; break;
            case PageNav::NextTab: g_view_tab = (g_view_tab + 1) % 3; break;
            case PageNav::Back: return false;
            case PageNav::Up:
                if (g_view_tab == 2) g_pause_sel = Next(rows, g_pause_sel, -1);
                break;
            case PageNav::Down:
                if (g_view_tab == 2) g_pause_sel = Next(rows, g_pause_sel, 1);
                break;
            case PageNav::Left:
            case PageNav::Right:
            case PageNav::Accept:
                if (g_view_tab == 2) ActivatePause(context, rows[size_t(g_pause_sel)], n == PageNav::Left ? -1 : 1);
                else if (n != PageNav::Accept) g_view_tab = n == PageNav::Left ? (g_view_tab + 2) % 3 : (g_view_tab + 1) % 3;
                break;
            default: break;
        }
    }
    rows = PauseOptions(context);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // the tabs, and the context's name
    float x = kLabelX - 30;
    for (int i = 0; i < 3; i++) {
        std::string name = Titled(kNames[i]);
        c.title(dl, x, 80, 15, i == g_view_tab ? c.lit : Dim(c.unlit), name.c_str(), 0);
        float w = c.title_width(15, name.c_str());
        if (ImGui::IsMouseHoveringRect(c.guest(x - 3, 78), c.guest(x + w + 3, 96), false) && ImGui::IsMouseClicked(0)) g_view_tab = i;
        x += w + 22;
    }
    c.title(dl, 570, 82, 13, Dim(c.lit), Titled(kContextNames[int(context)]).c_str(), 1);
    if (g_view_tab < 2) {
        DrawTable(c, dl, context, g_view_tab == 0 ? Device::Controller : Device::KeyboardMouse, 112, 14);
        return true;
    }
    // OPTIONS: the list, left and right (or a click) to change
    g_rows_y = 116;
    int hover = -1;
    g_pause_scroll = Clamp(g_pause_scroll, g_pause_sel, int(rows.size()));
    DrawRows(c, dl, rows, g_pause_sel, g_pause_scroll, false, "", &hover, kValueX + 40);
    if (hover >= 0 && (rows[size_t(hover)].kind == RowKind::Option || rows[size_t(hover)].kind == RowKind::Reset)) {
        ImVec2 m = ImGui::GetIO().MouseDelta;
        if (m.x != 0 || m.y != 0) g_pause_sel = hover;
        if (ImGui::IsMouseClicked(0)) ActivatePause(context, rows[size_t(hover)], 1);
        else if (ImGui::IsMouseClicked(1)) ActivatePause(context, rows[size_t(hover)], -1);
    }
    if (ImGui::GetIO().MouseWheel != 0) g_pause_scroll = std::max(0, g_pause_scroll - int(ImGui::GetIO().MouseWheel));
    return true;
}

// a table of a context's controls for the device in use, over the game's pre-mission Controller Setup
void ControlsList(const PageContext& c, controls::Context context, float y, int max_rows) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    Device d = controls::ActiveDevice();
    std::string caption = Titled(kContextNames[int(context)]) + "   -   " + Titled(kDeviceNames[int(d)]);
    c.title(dl, 320, y, 14, c.lit, caption.c_str(), 0.5f);
    DrawTable(c, dl, context, d, y + 24, max_rows);
}

}  // namespace menus
#endif
