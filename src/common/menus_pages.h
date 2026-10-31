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
};

// each returns false once the page closes
bool PcSettingsPage(const PageContext& c, const std::vector<PageNav>& nav);
bool ControlsPage(const PageContext& c, const std::vector<PageNav>& nav);
// the pause menu's view of the controls (nothing to rebind); opened: the first frame since it was opened
bool ControlsViewPage(const PageContext& c, const std::vector<PageNav>& nav, controls::Context context, bool opened);
// the list of a context's controls drawn from console y down, over the game's pre-mission Controller Setup
void ControlsList(const PageContext& c, controls::Context context, float y, int max_rows);
// window messages while a page is up (a key being bound); true when taken
bool PageMessage(uint32_t msg, uintptr_t wp);

}  // namespace menus
