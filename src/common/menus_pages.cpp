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
// the controls page until the bindings land: the game's own page stays reachable
namespace menus {
bool ControlsPage(const PageContext&, const std::vector<PageNav>&) { return false; }
bool PageMessage(uint32_t, uintptr_t) { return false; }
}  // namespace menus
#endif
