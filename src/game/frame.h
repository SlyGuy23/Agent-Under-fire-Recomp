// Work of bond_eng.xbe's hooks done once a frame on the game thread, from the front end's frame (autosave.cpp hooks
// 0x26B10) and a level's (game_options.cpp hooks 0x33390)
#pragma once

struct X86Context;

namespace game_options {
void Frame(X86Context& ctx);  // the OPTIONS remembered or put in (game_options.cpp)
}
