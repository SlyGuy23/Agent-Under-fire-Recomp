// Analog sticks for bond_eng.xbe and driving.xbe: a round deadzone with the angle kept, in place of the per axis ones
#pragma once
#include <cstdint>

namespace input {
struct Pad;
}

namespace sticks {

inline bool g_modern = true;      // off: the stick goes to the game as it is
inline uint32_t g_deadzone = 10;  // percent of full tilt, the round deadzone

void Apply(input::Pad& pad);

}  // namespace sticks
