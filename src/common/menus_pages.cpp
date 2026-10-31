#ifdef _WIN32
#include "common/menus_pages.h"

#include <algorithm>
#include <string>

#include "common/overlay.h"

namespace menus {

namespace {

// the options list's places, in console pixels
constexpr float kTitleY = 96, kTopY = 120, kRowStep = 23, kLabelX = 96, kValueX = 352, kTextSize = 15;
constexpr int kVisibleRows = 12;

struct Row {
    std::string label, value;
    bool enabled = true;
    bool heading = false;  // a section's name, not selectable
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
    c.say(dl, kLabelX, kTitleY, kTextSize + 1, c.lit, title, 0);
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + kVisibleRows) scroll = sel - kVisibleRows + 1;
    scroll = std::clamp(scroll, 0, std::max(0, int(rows.size()) - kVisibleRows));
    MouseHit hit;
    ImGuiIO& io = ImGui::GetIO();
    for (int i = scroll; i < int(rows.size()) && i < scroll + kVisibleRows; i++) {
        const Row& r = rows[size_t(i)];
        float y = kTopY + kRowStep * float(i - scroll);
        if (r.heading) {
            c.say(dl, kLabelX - 10, y + 4, kTextSize - 3, Dim(c.lit), r.label.c_str(), 0);
            continue;
        }
        ImU32 col = i == sel ? c.lit : (r.enabled ? c.unlit : Dim(c.unlit));
        c.say(dl, kLabelX, y, kTextSize, col, r.label.c_str(), 0);
        if (!r.value.empty()) c.say(dl, kValueX, y, kTextSize, i == sel ? c.lit : (r.enabled ? c.value : Dim(c.value)), r.value.c_str(), 0);
        ImVec2 a = c.guest(kLabelX - 12, y - 2), b = c.guest(600, y + kRowStep - 2);
        if (ImGui::IsMouseHoveringRect(a, b, false)) {
            hit.row = i;
            if (ImGui::IsMouseClicked(0)) hit.step = 1;
            if (ImGui::IsMouseClicked(1)) hit.step = -1;
        }
    }
    if (scroll > 0) c.say(dl, 320, kTopY - 14, kTextSize - 3, c.unlit, "^", 0.5f);
    if (scroll + kVisibleRows < int(rows.size())) c.say(dl, 320, kTopY + kRowStep * kVisibleRows - 4, kTextSize - 3, c.unlit, "v", 0.5f);
    if (hint) c.say(dl, 320, kTopY + kRowStep * kVisibleRows + 6, kTextSize - 4, Dim(c.unlit), hint, 0.5f);
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
        rows.push_back({settings[i].label, settings[i].value, settings[i].enabled, false});
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

enum class RowKind { Action, Option, Reset, Heading };
enum class Opt { LeftSens, LeftDead, LeftOuter, RightSens, RightDead, RightOuter, MouseSens, MouseInvert, MouseLook,
                 CrouchToggle, AimToggle };

struct CRow {
    RowKind kind;
    Action action = Action::None;
    Opt opt = Opt::LeftSens;
    std::string label, value;
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

// the mouse looks around where no key is bound to looking: on foot turning and looking up and down, on rails aiming
bool MouseLooks(Context c, Action a) {
    if (c == Context::OnFoot) return a == Action::LSLeft || a == Action::LSRight || a == Action::RSUp || a == Action::RSDown;
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
    }
    if (view_only) return rows;
    rows.push_back({RowKind::Heading, Action::None, Opt::LeftSens, t.device == Device::Controller ? "STICKS" : "MOUSE", ""});
    if (t.device == Device::Controller) {
        opt(Opt::LeftSens, "LEFT STICK SENSITIVITY", Pct(o.left.sensitivity));
        opt(Opt::LeftDead, "LEFT STICK DEADZONE", Pct(o.left.deadzone));
        opt(Opt::LeftOuter, "LEFT STICK OUTER DEADZONE", Pct(o.left.outer_deadzone));
        opt(Opt::RightSens, "RIGHT STICK SENSITIVITY", Pct(o.right.sensitivity));
        opt(Opt::RightDead, "RIGHT STICK DEADZONE", Pct(o.right.deadzone));
        opt(Opt::RightOuter, "RIGHT STICK OUTER DEADZONE", Pct(o.right.outer_deadzone));
    } else {
        opt(Opt::MouseSens, "MOUSE SENSITIVITY", Pct(o.mouse_sensitivity));
        opt(Opt::MouseInvert, "INVERT MOUSE Y", o.mouse_invert_y ? "ON" : "OFF");
        const char* looks[] = {"AUTOMATIC", "DIRECT", "AS A STICK"};
        opt(Opt::MouseLook, "MOUSE LOOK", looks[std::min<int>(int(o.mouse_look), 2)]);
    }
    rows.push_back({RowKind::Reset, Action::None, Opt::LeftSens, "RESET TO DEFAULTS", ""});
    return rows;
}

void StepOption(Context c, Opt k, int step) {
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
    }
    controls::SetOptions(c, o);
    app::Save();
}

void Reset(const Tab& t) {
    controls::ResetBindings(t.context, t.device);
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
    controls::SetOptions(t.context, o);
    app::Save();
}

int Next(const std::vector<CRow>& rows, int sel, int d) {
    int n = int(rows.size());
    for (int k = 0; k < n; k++) {
        sel = (sel + d + n) % n;
        if (rows[size_t(sel)].kind != RowKind::Heading) return sel;
    }
    return sel;
}

void Activate(const Tab& t, const CRow& r, int step) {
    if (r.kind == RowKind::Option) StepOption(t.context, r.opt, step);
    else if (r.kind == RowKind::Reset && step > 0) Reset(t);
    else if (r.kind == RowKind::Action && step > 0) {
        controls::BeginCapture(t.context, t.device, r.action, t.device == Device::Controller ? 0 : g_slot);
        g_capturing = true;
        g_capture_at = Ms();
    }
}

// the two rows of tabs: the devices, then on foot, driving and on rails; the shown ones lit. Returns a clicked tab,
// -1 for none. only: one context alone (the pause menu's page)
int DrawTabs(const PageContext& c, ImDrawList* dl, int tab, bool clickable, int only = -1) {
    int clicked = -1;
    const Tab& t = kTabs[tab];
    auto item = [&](float& x, float y, float size, const char* text, bool lit, int to) {
        c.say(dl, x, y, size, lit ? c.lit : Dim(c.unlit), text, 0);
        float w = c.width(size, text);
        ImVec2 a = c.guest(x - 3, y - 2), b = c.guest(x + w + 3, y + size + 2);
        if (clickable && ImGui::IsMouseHoveringRect(a, b, false) && ImGui::IsMouseClicked(0)) clicked = to;
        x += w + 18;
    };
    float x = kLabelX;
    for (int d = 0; d < 2; d++) item(x, kTitleY - 8, 13, kDeviceNames[d], int(t.device) == d, TabOf(only >= 0 ? Context(only) : t.context, Device(d)));
    if (only < 0) {
        x = kLabelX;
        for (int k = 0; k < 3; k++) item(x, kTitleY + 8, 11, kContextNames[k], int(t.context) == k, TabOf(Context(k), t.device));
    } else {
        x = kLabelX;
        item(x, kTitleY + 8, 11, kContextNames[only], true, tab);
    }
    return clicked;
}

// the rows from kTopY, scrolled to keep sel in view; the hovered row (or -1) through hover
void DrawRows(const PageContext& c, ImDrawList* dl, const std::vector<CRow>& rows, int sel, int scroll, bool keyboard,
              const std::string& sel_value, int* hover, float value_x) {
    for (int i = scroll; i < int(rows.size()) && i < scroll + kVisibleRows; i++) {
        const CRow& r = rows[size_t(i)];
        float y = kTopY + 6 + kRowStep * float(i - scroll);
        if (r.kind == RowKind::Heading) {
            c.say(dl, kLabelX - 10, y + 3, kTextSize - 3, Dim(c.lit), r.label.c_str(), 0);
            continue;
        }
        bool lit = i == sel;
        std::string value = lit && !sel_value.empty() ? sel_value : r.value;
        c.say(dl, kLabelX, y, kTextSize - 1, lit ? c.lit : c.unlit, r.label.c_str(), 0);
        c.say(dl, value_x, y, kTextSize - 1, lit ? c.lit : c.value, value.c_str(), 0);
        ImVec2 a = c.guest(kLabelX - 12, y - 2), b = c.guest(620, y + kRowStep - 2);
        if (hover && ImGui::IsMouseHoveringRect(a, b, false)) *hover = i;
    }
    (void)keyboard;
    if (scroll > 0) c.say(dl, 320, kTopY - 4, kTextSize - 3, c.unlit, "^", 0.5f);
    if (scroll + kVisibleRows < int(rows.size())) c.say(dl, 320, kTopY + 6 + kRowStep * kVisibleRows - 4, kTextSize - 3, c.unlit, "v", 0.5f);
}

int Clamp(int scroll, int sel, int n) {
    if (sel >= 0 && sel < scroll) scroll = sel;
    if (sel >= scroll + kVisibleRows) scroll = sel - kVisibleRows + 1;
    return std::clamp(scroll, 0, std::max(0, n - kVisibleRows));
}

int g_view_tab = 0, g_view_scroll = 0;

}  // namespace

bool PageMessage(uint32_t, uintptr_t) { return false; }

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
    std::vector<CRow> rows = Rows(tab, false);
    if (g_sel < 0 || g_sel >= int(rows.size()) || rows[size_t(g_sel)].kind == RowKind::Heading) g_sel = 0;
    bool settling = ImGui::GetFrameCount() - g_ended_frame < 3;
    if (!g_capturing && !settling) {
        for (PageNav n : nav) {
            const CRow& r = rows[size_t(g_sel)];
            switch (n) {
                case PageNav::Up: g_sel = Next(rows, g_sel, -1); break;
                case PageNav::Down: g_sel = Next(rows, g_sel, 1); break;
                case PageNav::Left:
                case PageNav::Right: {
                    int step = n == PageNav::Left ? -1 : 1;
                    if (r.kind == RowKind::Option) StepOption(tab.context, r.opt, step);
                    else if (r.kind == RowKind::Action && tab.device == Device::KeyboardMouse) g_slot = step < 0 ? 0 : 1;
                    break;
                }
                case PageNav::Accept: Activate(tab, r, 1); break;
                case PageNav::Back: g_sel = -1; return false;
                case PageNav::PrevTab: g_tab = (g_tab + kTabCount - 1) % kTabCount, g_sel = 0, g_scroll = 0; break;
                case PageNav::NextTab: g_tab = (g_tab + 1) % kTabCount, g_sel = 0, g_scroll = 0; break;
            }
            if (g_capturing) break;
        }
    }
    const Tab& shown = kTabs[g_tab];
    rows = Rows(shown, false);
    if (g_sel >= int(rows.size())) g_sel = 0;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    int clicked = DrawTabs(c, dl, g_tab, !g_capturing);
    if (clicked >= 0 && clicked != g_tab) g_tab = clicked, g_sel = 0, g_scroll = 0;
    g_scroll = Clamp(g_scroll, g_sel, int(rows.size()));
    ImVec2 m = ImGui::GetIO().MousePos;
    bool moved = m.x != g_mouse_last.x || m.y != g_mouse_last.y;
    g_mouse_last = m;
    std::string sel_value;
    const CRow& sr = rows[size_t(g_sel)];
    if (g_capturing) sel_value = shown.device == Device::Controller ? "PRESS A BUTTON" : "PRESS A KEY";
    else if (sr.kind == RowKind::Action && shown.device == Device::KeyboardMouse) {
        std::string k0 = KeyText(shown, sr.action, 0), k1 = controls::BindingName(shown.context, shown.device, sr.action, 1);
        if (k1.empty()) k1 = "-";
        sel_value = (g_slot == 0 ? "[" + k0 + "]" : k0) + "  /  " + (g_slot == 1 ? "[" + k1 + "]" : k1);
    }
    int hover = -1;
    DrawRows(c, dl, rows, g_sel, g_scroll, shown.device == Device::KeyboardMouse, sel_value,
             (!g_capturing && !settling) ? &hover : nullptr, kValueX - 20);
    if (hover >= 0) {
        const CRow& r = rows[size_t(hover)];
        if (moved) g_sel = hover;
        float y = kTopY + 6 + kRowStep * float(hover - g_scroll);
        if (r.kind == RowKind::Action && shown.device == Device::KeyboardMouse && moved) {
            ImVec2 mid = c.guest(kValueX + 60, y);
            g_slot = m.x < mid.x ? 0 : 1;
        }
        if (ImGui::IsMouseClicked(0)) Activate(shown, r, 1);
        else if (ImGui::IsMouseClicked(1)) {
            if (r.kind == RowKind::Option) StepOption(shown.context, r.opt, -1);
            else if (r.kind == RowKind::Action && shown.device == Device::KeyboardMouse)
                controls::SetKeyBinding(shown.context, r.action, g_slot, controls::kNoKey), app::Save();
        }
    }
    if (!g_capturing && ImGui::GetIO().MouseWheel != 0) g_scroll = std::max(0, g_scroll - int(ImGui::GetIO().MouseWheel));
    return true;
}

// the pause menu's page: the controls of this context for the device in use, nothing to change but the toggles;
// LT / RT (or a click) shows the other device
bool ControlsViewPage(const PageContext& c, const std::vector<PageNav>& nav, controls::Context context, bool opened) {
    if (opened) g_view_tab = TabOf(context, controls::ActiveDevice()), g_view_scroll = 0;
    for (PageNav n : nav) {
        const Tab& t = kTabs[g_view_tab];
        switch (n) {
            case PageNav::Up: g_view_scroll = std::max(0, g_view_scroll - 1); break;
            case PageNav::Down: g_view_scroll++; break;
            case PageNav::PrevTab:
            case PageNav::NextTab:
                g_view_tab = TabOf(context, t.device == Device::Controller ? Device::KeyboardMouse : Device::Controller);
                g_view_scroll = 0;
                break;
            case PageNav::Back:
            case PageNav::Accept: return false;
            default: break;
        }
    }
    const Tab& t = kTabs[g_view_tab];
    std::vector<CRow> rows = Rows(t, true);
    while (!rows.empty() && rows.back().kind != RowKind::Action) rows.pop_back();  // only the controls
    g_view_scroll = std::clamp(g_view_scroll, 0, std::max(0, int(rows.size()) - kVisibleRows));
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    int clicked = DrawTabs(c, dl, g_view_tab, true, int(context));
    if (clicked >= 0) g_view_tab = clicked, g_view_scroll = 0;
    if (ImGui::GetIO().MouseWheel != 0) g_view_scroll = std::max(0, g_view_scroll - int(ImGui::GetIO().MouseWheel));
    DrawRows(c, dl, rows, -1, g_view_scroll, t.device == Device::KeyboardMouse, "", nullptr, kValueX - 20);
    return true;
}

// a still list of a context's controls for the device in use, from y (the game's pre-mission Controller Setup)
void ControlsList(const PageContext& c, controls::Context context, float y, int max_rows) {
    const Tab& t = kTabs[TabOf(context, controls::ActiveDevice())];
    std::vector<CRow> rows = Rows(t, true);
    while (!rows.empty() && rows.back().kind != RowKind::Action) rows.pop_back();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // two columns when they do not fit in one
    int per = int(rows.size()) > max_rows ? (int(rows.size()) + 1) / 2 : int(rows.size());
    float step = std::min(20.0f, 300.0f / float(std::max(per, 1)));
    for (int i = 0; i < int(rows.size()); i++) {
        int col = i / per, k = i % per;
        float x = col ? 330 : 60, yy = y + step * float(k);
        c.say(dl, x, yy, 12, c.unlit, rows[size_t(i)].label.c_str(), 0);
        c.say(dl, x + 270, yy, 12, c.lit, rows[size_t(i)].value.c_str(), 1);
    }
}

}  // namespace menus
#endif
