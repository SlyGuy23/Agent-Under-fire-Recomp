// The game's own OPTIONS (bond_eng.xbe's front end and pause menu) remembered in the settings file's [game_options],
// so they come back on the next start without loading a save, and win over the ones a loaded save brings
// (src/game/game_options.cpp reads and writes them in the game; driving.xbe gets them from it in its launch data).
#pragma once
#include <cstdint>

namespace game_options {

enum Option : int {
    kDifficulty,        // 0 Operative, 1 Agent, 2 00 Agent
    kControllerSetup,   // the game's controller setup, 0 to 3
    kInvertPitch,
    kVibration,
    kHudAlwaysVisible,
    kCrossHair,
    kGoldRewards,
    kMusic,             // 0 to 128
    kSoundEffects,      // 0 to 128
    kCount
};
extern const char* const kKeys[kCount];

struct Values {
    bool have[kCount] = {};  // what the settings file held (or was given)
    int32_t value[kCount] = {};
};

Values Get();
void Set(const Values& v);  // remembered, and the settings file written (app::Save)

void Load(const void* table);  // from GameInfo::load
void Save(void* table);        // from GameInfo::save

}  // namespace game_options
