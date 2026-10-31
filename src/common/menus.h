// Entries this port adds to the game's own front end menus (bond_eng.xbe), drawn as the game draws its own:
// - QUIT GAME under OPTIONS on the main menu, a sixth bar from the bar sheet (the texture pack's image of the sheet
//   holds its art), asking before the game closes;
// - PC Settings in the options list under Controller Setup, in the list's own lettering, opening a page of the F10
//   panel's settings;
// - Controller Setup opening this port's controls page (remapping, sticks, keyboard and mouse) in place of the game's.
// The game's menus stay the game's: the hooks move its rows to make room, draw the extra rows after its own with its
// draw state, and keep a selection of their own past its last row by holding the pad back from it.
#pragma once
#include <cstdint>

#include "app/app.h"
#include "common/controls.h"

namespace input {
struct Pad;
}

namespace menus {

// GameInfo::draw, before the game_info quad fixes: moves the menus' rows, recolours the row the game has selected
// while an added one is
void Draw(const void* state, uint32_t prim, uint8_t* vertices, uint32_t stride, uint32_t count);
// GameInfo::after_draw: the added rows
void AfterDraw(const void* state, uint32_t prim, const uint8_t* vertices, uint32_t stride, uint32_t count,
               app::GameInfo::Emit emit);
// GameInfo::pad, port 0's pad before the game sees it: the added rows' selection, and the pages
void Pad(int port, input::Pad& pad);
// GameInfo::ui: the pages (the quit question, PC Settings, controls)
void Ui();
// GameInfo::window: the pages' font
void Window(void* hwnd);
// GameInfo::message: keys and mouse on the pages; true when taken
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
// a page is up: the game gets no input
bool PageOpen();
// what the mission picked in the front end plays as (its pre-mission Controller Setup lists those controls)
void SetMissionQuery(controls::Context (*query)());

}  // namespace menus
