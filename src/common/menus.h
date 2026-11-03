// Entries this port adds to the game's own front end menus (bond_eng.xbe), drawn as the game draws its own:
// - QUIT GAME under OPTIONS on the main menu, a sixth bar from the bar sheet (the texture pack's image of the sheet
//   holds its art), asking before the game closes;
// - PC Settings in the options list under Controller Setup, in the list's own lettering, opening a page of the F10
//   panel's settings;
// - Controller Setup opening this port's controls page (remapping, sticks, keyboard and mouse) in place of the game's;
// - Controls and PC Settings under Quit Mission on the pause menus (bond_eng.xbe on foot, driving.xbe driving and on
//   rails): the Controls page (its options, and CONTROLLER LAYOUT opening the grid of the mission's controls) and a
//   PC Settings page of the settings that can change in a mission.
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
// The mouse cursor over the game's own menus (Ui each frame; the harness): where it is in console pixels (640x480),
// whether it is over the console's frame, moved and the left button was clicked. On a menu with the cursor shown
// (controls::MenuCursor) the row under it is selected as it moves there and a click accepts it (port 0's pad).
void MouseCursor(float x, float y, bool over, bool moved, bool clicked);
// the menus take the left button's clicks (the cursor shown on a menu, no page up): controls.cpp no longer makes
// every click A then
bool MouseClicks();
// what the mission picked in the front end plays as (its pre-mission Controller Setup lists those controls)
void SetMissionQuery(controls::Context (*query)());
// START or Escape pressed on bond_eng.xbe's pause menu (any page, row or question of it): true once, for the game's
// menu tick to close the pause menu as Continue Mission does (src/game/pause.cpp)
bool TakePauseClose();

// driving.xbe's pause menu, from its own drawing (src/driving/pause.cpp): the MISSION page's rows drawn this frame and
// the row the game has lit (-1 while one of its questions is up), and its >> marker drawn
void DrivingPauseShown(int lit);
void DrivingPauseMarker();
// driving.xbe's "Are you sure?" question: its answer word (0 the first drawn, 1 the second) is being drawn now, lit or
// not; -1 when it is done (for the mouse, which finds the words by the letters drawn in between)
void DrivingQuestionWord(int word, bool lit);
// a pause page (Controls, PC Settings) is up, or closing with the pause menu: the menu's rows and marker are not drawn
bool PausePageUp();
// the added row selected (an index of the labels), -1 while the game's own selection holds; the added rows' labels
int PauseExtraSelected();
int PauseExtraRows();
const char* PauseExtraLabel(int row);
// how far driving.xbe's pause rows and marker are moved up, in console pixels (640x480)
float DrivingPauseLift();
// driving.xbe quitting a mission (src/driving/hooks.cpp, each frame from the answer on): the keyboard's keys over the
// pause screen's prompts are not drawn from then on
void HoldPrompts();

}  // namespace menus
