// Hooks for driving.xbe. A strong definition replaces the recompiled function at that address.
#include <cmath>
#include <cstdint>
#include <initializer_list>

#include "common/controls.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

// a pointer the game keeps that is safe to read through: its image and heap, or the 64 MB physical window
bool GuestPointer(uint32_t va) { return (va >= 0x10000 && va < 0x7F00000) || (va >= 0x80000000u && va < 0x84000000u); }

float& F(uint32_t va) { return kernel::G<float>(va); }

// what goes past +-limit, leaving v at the limit
float Excess(float& v, float limit) {
    if (!(limit > 0)) return 0;
    float over = v > limit ? v - limit : v < -limit ? v + limit : 0;
    v -= over;
    return over;
}

// Mouse aim of the on-rails gunner (mis02a, mis07b, mis09). The gun object (its vtable 0x13A02C, a pointer at
// 0x18994C) keeps its aim as a base frame and a pitch, angles in revolutions (1 = 360 degrees):
//   +0x20 up, +0x30 forward (float4 each), turned about the world's Y axis for yaw
//   +0x1B8 pitch (negative is up), clamped by the update between the gun's limits (about 58 degrees up)
//   +0x1B0 / +0x1B4 the crosshair's pitch and yaw inside the view while aiming (LT): a box of
//     [[0x18F950] + [+0x247]*0xB0 + 0x40 / +0x44] times the zoom factor, the view dragged at its edge
//   +0x60 the field of view in degrees (33, 20 while aiming); the stick's rates are scaled by sin(fov) * 1.836,
//     1 at 33 degrees
//   +0x2F8 the input state: byte +0x0A aiming, +0x0B going back to the centre (Back)
// The stick only sets rates (+0x260 pitch, +0x264 yaw, 0.0015 or 0.005 a frame at past half and past three
// quarters tilt, nothing under half) that the update 0x77A20 eases toward and turns the aim by, after an
// acceleration of about a second: through the stick the mouse could never turn faster than the controller. So
// the mouse turns the aim itself, just before the update, which then clamps the pitch and builds the view from it.
// Its degrees are those of mouse look on foot (0.044 a count at 100%), scaled by the zoom as the stick is.
void RailsMouseAim(uint32_t gun) {
    constexpr uint32_t kMission = 0x1C2C2C, kMenu = 0x1BEB1C, kBoxes = 0x18F950;
    uint32_t m = kernel::R32(kMission);
    if (!GuestPointer(m) || !kernel::G<uint8_t>(m + 0x408)) return;  // not the gunner's controls
    float yaw = 0, pitch = 0;
    if (!controls::TakeMouseLook(yaw, pitch) || (yaw == 0 && pitch == 0)) return;
    // the update itself does nothing while the driving camera is up ([m+0x40C]) or the pause menu is open
    if (kernel::R32(m + 0x40C) || kernel::R32(m + 0x404) != 1 || kernel::R32(kMenu) != 0) return;
    if (!GuestPointer(gun)) return;
    uint32_t in = kernel::R32(gun + 0x2F8);
    if (!GuestPointer(in)) return;

    constexpr float kPi = 3.14159265f;
    float fov = F(gun + 0x60);
    float zoom = fov > 0 && fov < 180 ? std::sin(fov * kPi / 180) * 1.836078f : 1.0f;
    float turn = yaw / 360 * zoom;     // + is right
    float lift = -pitch / 360 * zoom;  // + is down, as the game's pitch

    if (kernel::G<uint8_t>(in + 0x0A)) {
        // aiming: the crosshair moves inside its box, what goes past the edge turns the view
        uint32_t boxes = kernel::R32(kBoxes);
        uint32_t box = boxes + uint32_t(int32_t(kernel::G<int8_t>(gun + 0x247)) * 0xB0);
        if (GuestPointer(boxes)) {
            F(gun + 0x1B0) += lift;
            F(gun + 0x1B4) += turn;
            lift = Excess(F(gun + 0x1B0), F(box + 0x40) * zoom);
            turn = Excess(F(gun + 0x1B4), F(box + 0x44) * zoom);
        }
    }
    F(gun + 0x1B8) += lift;
    if (turn != 0) {
        float c = std::cos(turn * 2 * kPi), s = std::sin(turn * 2 * kPi);
        for (uint32_t v : {gun + 0x20, gun + 0x30}) {
            float x = F(v), z = F(v + 8);
            F(v) = x * c + z * s;
            F(v + 8) = z * c - x * s;
        }
    }
    kernel::G<uint8_t>(in + 0x0B) = 0;  // a look of its own stops the way back to the centre, as the stick does
}

}  // namespace

// the gunner's update, each frame of an on-rails mission (a task the gun sets up at 0x6DECD)
X86_EXTERN_FUNC(__imp__sub_00077A20);
X86_FUNC(sub_00077A20) {
    RailsMouseAim(ctx.ecx);
    __imp__sub_00077A20(ctx, base);
}
