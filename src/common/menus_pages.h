// The pages menus.cpp opens from the game's options list: PC Settings (the F10 panel's settings) and the controls
// page (remapping, sticks, keyboard and mouse). Windows only: drawn with ImGui over the game's options screen, whose
// own rows menus.cpp hides meanwhile, in its places (console coordinates) and its colours.
#pragma once
#include <cstdint>
#include <vector>

#include <imgui.h>

#include "common/controls.h"

namespace menus {

enum class PageNav { Up, Down, Left, Right, Accept, Back, PrevTab, NextTab };

struct PageContext {
    ImVec2 (*guest)(float x, float y);  // console coordinates to the window's
    float (*scale)();                   // window pixels per console pixel
    // text at console coordinates with the list's shadow; align 0 left, 0.5 centre, 1 right
    void (*say)(ImDrawList* dl, float x, float y, float size, ImU32 col, const char* text, float align);
    float (*width)(float size, const char* text);  // a text's width in console pixels
    // the same in the italic title lettering
    void (*title)(ImDrawList* dl, float x, float y, float size, ImU32 col, const char* text, float align);
    float (*title_width)(float size, const char* text);
    ImU32 lit, unlit, value;  // the list's colours: the selected row, the others, values
    // the pause pages' places on the pause menu's panel as drawn under them (taller, menus.cpp PanelStretch): the
    // title's line (in the tabs' bar), the rows' left edge and first row, and the prompts' bar at the bottom
    float pause_title_y = 80, pause_x = 116, pause_top = 112, pause_bar_y = 378;
    // a panel behind a grid of controls in the pause menu's look (its moving light-blue background over a dark base),
    // console coordinates; null: a plain dark panel
    void (*panel)(ImDrawList* dl, float x0, float y0, float x1, float y1) = nullptr;
    // the pause Controls page's controller layout grid: its middle and its size (1 the main menu grid's), so it stays
    // clear of the panel's curved left column and of the bar at its bottom
    float pause_grid_centre = 346, pause_grid_scale = 1;
    // the layout's selected tab (Controller or Keyboard & Mouse) spans x0 to x1 in the tabs' bar (console pixels): the
    // game's light tab box is drawn there (menus.cpp); called each frame the layout is up
    void (*layout_tab)(float x0, float x1) = nullptr;
    // the game draws Back to the list (B BACK) in the prompts' bar while the controller layout is up (driving.xbe,
    // menus.cpp), x back_x0 to back_x1 (console pixels): no words of the page's there, only the mouse's place
    bool (*game_back)() = nullptr;
    float back_x0 = 0, back_x1 = 0;
    // a scroll chevron (more rows above: up, below: down) centred on x in the 11 px cell from y; null: drawn here
    void (*chevron)(ImDrawList* dl, float x, float y, bool up, ImU32 col) = nullptr;
};

// each returns false once the page closes
bool PcSettingsPage(const PageContext& c, const std::vector<PageNav>& nav);
bool ControlsPage(const PageContext& c, const std::vector<PageNav>& nav);
// the pause menu's Controls page: its options list, CONTROLLER LAYOUT at the top opening the controls grid (nothing
// to rebind); opened: the first frame since it was opened
bool ControlsViewPage(const PageContext& c, const std::vector<PageNav>& nav, controls::Context context, bool opened);
// the pause menu's PC Settings page: the PC Settings that can change in a mission (the same settings, by label)
bool PausePcSettingsPage(const PageContext& c, const std::vector<PageNav>& nav, controls::Context context, bool opened);
// the list of a context's controls drawn from console y down, over the game's pre-mission Controller Setup
void ControlsList(const PageContext& c, controls::Context context, float y, int max_rows);
// window messages while a page is up (a key being bound); true when taken
bool PageMessage(uint32_t msg, uintptr_t wp);

}  // namespace menus
