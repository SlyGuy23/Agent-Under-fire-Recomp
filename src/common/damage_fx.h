// DAMAGE EFFECTS and DAMAGE FLASH COLOUR (PC Settings, ACCESSIBILITY): what being hit on foot does to the screen.
// The game flashes the HUD white over the whole view when the health drops (0x534DC: alpha 0.4 to 0.9, more the lower
// the health, held two frames then faded) and kicks the view in pitch and roll away from the hit (0x27910, the client's
// damage feedback). Reduced draws the flash at half strength and kicks half as far, Minimal at a quarter, Off neither.
// The flash colour draws the same flash in black. Applied at once (src/game/hooks.cpp); the driving and on-rails
// missions have neither (their camera only shakes near explosions), so they are left alone.
#pragma once
#include <algorithm>
#include <cstdint>

namespace damage_fx {

enum Level : uint32_t { kDefault, kReduced, kMinimal, kOff, kLevels };

inline uint32_t g_level = kDefault;   // settings file "damage_effects", 0 to 3
inline bool g_flash_black = false;   // settings file "damage_flash_black"

inline uint32_t Current() { return std::min<uint32_t>(g_level, kOff); }

// the share of the game's flash strength and view kick kept
inline float Scale() {
    constexpr float kScales[kLevels] = {1.0f, 0.5f, 0.25f, 0.0f};
    return kScales[Current()];
}

inline const char* Name() {
    constexpr const char* kNames[kLevels] = {"DEFAULT", "REDUCED", "MINIMAL", "OFF"};
    return kNames[Current()];
}

}  // namespace damage_fx
