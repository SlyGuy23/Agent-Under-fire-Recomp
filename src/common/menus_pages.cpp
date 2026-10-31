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
    MouseHit hit = DrawList(c, "PC SETTINGS", rows, g_pc_sel, g_pc_scroll,
                            "LEFT / RIGHT or click to change     B / ESC to go back");
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
    const char* name;
    Context context;
    Device device;
};
constexpr Tab kTabs[] = {{"CONTROLLER", Context::OnFoot, Device::Controller},
                         {"CONTROLLER - DRIVING", Context::Driving, Device::Controller},
                         {"KEYBOARD & MOUSE", Context::OnFoot, Device::KeyboardMouse},
                         {"KEYBOARD & MOUSE - DRIVING", Context::Driving, Device::KeyboardMouse}};
constexpr int kTabCount = int(sizeof(kTabs) / sizeof(kTabs[0]));

enum class RowKind { Action, Option, Reset, Heading };
enum class Opt { LeftSens, LeftDead, LeftOuter, RightSens, RightDead, RightOuter, MouseSens, MouseInvert, MouseLook };

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

std::vector<CRow> Rows(const Tab& t) {
    std::vector<CRow> rows;
    controls::Options o = controls::GetOptions(t.context);
    for (Action a : controls::Actions(t.context, t.device)) {
        CRow r{RowKind::Action, a};
        r.label = controls::ActionLabel(t.context, a);
        if (t.device == Device::Controller) {
            r.value = controls::BindingName(t.context, t.device, a, 0);
        } else {
            std::string k0 = controls::BindingName(t.context, t.device, a, 0), k1 = controls::BindingName(t.context, t.device, a, 1);
            r.value = (k0.empty() ? "-" : k0) + "   /   " + (k1.empty() ? "-" : k1);
        }
        rows.push_back(r);
    }
    auto opt = [&](Opt k, const char* label, std::string value) {
        CRow r{RowKind::Option};
        r.opt = k;
        r.label = label;
        r.value = std::move(value);
        rows.push_back(r);
    };
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
    bool just_bound = false;
    if (!g_capturing && g_capture_at && Ms() - g_capture_at < 0) just_bound = true;
    std::vector<CRow> rows = Rows(tab);
    if (g_sel < 0 || g_sel >= int(rows.size()) || rows[size_t(g_sel)].kind == RowKind::Heading) g_sel = 0;
    bool settling = ImGui::GetFrameCount() - g_ended_frame < 3;
    (void)just_bound;
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
    rows = Rows(shown);
    if (g_sel >= int(rows.size())) g_sel = 0;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // the tabs along the top: the shown one lit
    float tx = kLabelX;
    for (int i = 0; i < kTabCount; i++) {
        ImU32 col = i == g_tab ? c.lit : Dim(c.unlit);
        c.say(dl, tx, kTitleY, 11, col, kTabs[i].name, 0);
        float w = c.width(11, kTabs[i].name);
        ImVec2 a = c.guest(tx - 3, kTitleY - 2), b = c.guest(tx + w + 3, kTitleY + 14);
        if (!g_capturing && ImGui::IsMouseHoveringRect(a, b, false) && ImGui::IsMouseClicked(0)) g_tab = i, g_sel = 0, g_scroll = 0;
        tx += w + 14;
    }
    // the rows
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + kVisibleRows) g_scroll = g_sel - kVisibleRows + 1;
    g_scroll = std::clamp(g_scroll, 0, std::max(0, int(rows.size()) - kVisibleRows));
    ImVec2 m = ImGui::GetIO().MousePos;
    bool moved = m.x != g_mouse_last.x || m.y != g_mouse_last.y;
    g_mouse_last = m;
    for (int i = g_scroll; i < int(rows.size()) && i < g_scroll + kVisibleRows; i++) {
        const CRow& r = rows[size_t(i)];
        float y = kTopY + kRowStep * float(i - g_scroll);
        if (r.kind == RowKind::Heading) {
            c.say(dl, kLabelX - 10, y + 4, kTextSize - 3, Dim(c.lit), r.label.c_str(), 0);
            continue;
        }
        bool sel = i == g_sel;
        std::string value = r.value;
        if (sel && g_capturing)
            value = shown.device == Device::Controller ? "PRESS A BUTTON..." : "PRESS A KEY...  (ESC cancels)";
        else if (sel && r.kind == RowKind::Action && shown.device == Device::KeyboardMouse) {
            std::string k0 = controls::BindingName(shown.context, shown.device, r.action, 0);
            std::string k1 = controls::BindingName(shown.context, shown.device, r.action, 1);
            value = (g_slot == 0 ? "[ " + (k0.empty() ? std::string("-") : k0) + " ]" : (k0.empty() ? std::string("-") : k0)) + "   /   " +
                    (g_slot == 1 ? "[ " + (k1.empty() ? std::string("-") : k1) + " ]" : (k1.empty() ? std::string("-") : k1));
        }
        c.say(dl, kLabelX, y, kTextSize - 1, sel ? c.lit : c.unlit, r.label.c_str(), 0);
        c.say(dl, kValueX - 20, y, kTextSize - 1, sel ? c.lit : c.value, value.c_str(), 0);
        ImVec2 a = c.guest(kLabelX - 12, y - 2), b = c.guest(620, y + kRowStep - 2);
        if (!g_capturing && !settling && ImGui::IsMouseHoveringRect(a, b, false)) {
            if (moved) g_sel = i;
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
    }
    if (!g_capturing && ImGui::GetIO().MouseWheel != 0) g_scroll = std::max(0, g_scroll - int(ImGui::GetIO().MouseWheel));
    if (g_scroll > 0) c.say(dl, 320, kTopY - 12, kTextSize - 3, c.unlit, "^", 0.5f);
    if (g_scroll + kVisibleRows < int(rows.size())) c.say(dl, 320, kTopY + kRowStep * kVisibleRows - 4, kTextSize - 3, c.unlit, "v", 0.5f);
    const char* hint = shown.device == Device::Controller
                           ? "A change     LT / RT switch tab     B back"
                           : "ENTER or click to change     LEFT / RIGHT choose key     RIGHT CLICK clear     Q / E switch tab";
    c.say(dl, 320, kTopY + kRowStep * kVisibleRows + 6, kTextSize - 4, Dim(c.unlit), hint, 0.5f);
    return true;
}

}  // namespace menus
#endif
