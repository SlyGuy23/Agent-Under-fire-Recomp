// bond_eng.xbe turns each thumb axis into a value on its own (0x1378D0): f = (v + 0.5) / 32767.5, then
// sign(f) * max(|f| - 0.33, 0) * 1.4925. A third of the travel on each axis is dead, so a push that is mostly up
// loses its sideways part and moves straight up: the stick snaps to the four axes and the diagonals between them.
// driving.xbe (0xDC96E) keeps f = v / 32768 as it is but zeroes an axis under 0.25.
//
// Here the stick gets a round deadzone instead, keeps its angle, and is scaled over the rest of its travel to the
// value d (each axis -1 to 1) a modern game would use. Each axis is then sent as the raw value the game's own
// conversion turns back into d: sign(d) * (0.33 + 0.67 * |d|) for bond_eng.xbe. driving.xbe cannot show a value
// under 0.25, so there d is spread over 0.25 to 1, which steers from the first movement past the round deadzone.
#include "common/sticks.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "core/input.h"
#include "image_symbols.h"

namespace sticks {

namespace {

constexpr bool kGame = std::string_view(IMAGE_XBE) == "bond_eng.xbe";
constexpr bool kDriving = std::string_view(IMAGE_XBE) == "driving.xbe";
constexpr bool kThisImage = kGame || kDriving;  // the launcher only reads the pad to skip its films
constexpr float kGameDeadzone = kGame ? 0.33f : 0.25f;  // bond_eng 0x1E1DFC, driving 0x138A88

float Normal(int16_t v) { return (float(v) + 0.5f) / 32767.5f; }

int16_t Raw(float f) {
    float v = std::round(f * 32767.5f - 0.5f);
    return int16_t(std::clamp(v, -32768.0f, 32767.0f));
}

// what the game's conversion maps to d
float Inverse(float d) {
    if (d == 0) return 0;
    float m = kGameDeadzone + std::min(std::fabs(d), 1.0f) * (1.0f - kGameDeadzone);
    return d < 0 ? -m : m;
}

void Stick(int16_t& sx, int16_t& sy) {
    float x = Normal(sx), y = Normal(sy);
    float r = std::sqrt(x * x + y * y);
    float dz = std::min<uint32_t>(g_deadzone, 50) / 100.0f;
    float dx = 0, dy = 0;
    if (r > dz && r > 0) {
        // past the deadzone the magnitude starts from 0; corners of a square gate stay at full tilt
        float m = std::min((r - dz) / (1.0f - dz), 1.0f);
        dx = x / r * m;
        dy = y / r * m;
    }
    sx = Raw(Inverse(dx));
    sy = Raw(Inverse(dy));
}

}  // namespace

void Apply(input::Pad& pad) {
    if (!kThisImage || !g_modern || !pad.connected) return;
    Stick(pad.lx, pad.ly);
    Stick(pad.rx, pad.ry);
}

}  // namespace sticks
