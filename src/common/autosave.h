// Autosave after each completed mission (src/game/autosave.cpp): the save file the player loaded or saved last is
// written again, as the main menu's SAVE MISSION over it would
#pragma once

namespace autosave {

inline bool g_enabled = true;  // GameInfo::options "autosave"

}  // namespace autosave
