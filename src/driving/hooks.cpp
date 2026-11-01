// Hooks for driving.xbe. A strong definition replaces the recompiled function at that address.
#include <cmath>
#include <cstdint>
#include <initializer_list>

#include "common/controls.h"
#include "common/fov.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"
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

// Stick sensitivity on rails. The gun's action handler (0x7B230) turns the further of the two sticks on each axis into
// a rate (0x75620: nothing up to half tilt, 0.0015 a frame to three quarters, 0.005 past it) that 0x76C00 stores as the
// target yaw rate (+0x264) and 0x76BE0 as the pitch one (+0x260); the update eases toward them. With only those steps
// the stick cannot carry a sensitivity: the rates are scaled by it (controls::TurnGains, the same on both sticks' axes).
X86_EXTERN_FUNC(__imp__sub_00076C00);
X86_FUNC(sub_00076C00) {
    const uint32_t gun = ctx.ecx;
    __imp__sub_00076C00(ctx, base);
    float lx, ly, rx, ry;
    controls::TurnGains(lx, ly, rx, ry);
    if (lx != 1) F(gun + 0x264) *= lx;
}

X86_EXTERN_FUNC(__imp__sub_00076BE0);
X86_FUNC(sub_00076BE0) {
    const uint32_t gun = ctx.ecx;
    __imp__sub_00076BE0(ctx, base);
    float lx, ly, rx, ry;
    controls::TurnGains(lx, ly, rx, ry);
    if (ly != 1) F(gun + 0x260) *= ly;
}

// The gunner's auto-aim (0x76310, the camera's turn toward the current target [0x1C6C18]): with the keyboard and mouse
// and the port's AUTO LOCK-ON off it sees no target, which also clears its auto-aiming flag; the target is put back.
X86_EXTERN_FUNC(__imp__sub_00076310);
X86_FUNC(sub_00076310) {
    if (!controls::AimAssistOff()) {
        __imp__sub_00076310(ctx, base);
        return;
    }
    const uint32_t target = kernel::R32(0x1C6C18);
    kernel::W32(0x1C6C18, 0);
    __imp__sub_00076310(ctx, base);
    kernel::W32(0x1C6C18, target);
}

// Quit Mission -> "Yes": the frame that takes the answer closes the pause menu and runs one plain frame of the mission
// (no dimming, no panel) before the mission's teardown (0x43680) fades the screen out with 0xB7B60. That fade draws only
// a black quad of growing alpha onto the back buffer each frame, over whatever picture the buffer last had, and the
// device rotates three buffers: the two older ones hold the pause screen, the newest that plain frame, so the panel
// flashed off every third frame of the fade (and on the answering frame itself). So when that plain frame is about to
// be shown, the last pause picture is copied over it and over the oldest buffer: every buffer the fade darkens, and
// the answering frame, show the pause screen.
// 0x100690 is the end of a frame (EndScene, Swap). The quit is asked for at 0xB5B89 ([0x1E0780] = 1) while the run
// state [0x1BEB1C] is still 3 (pause); it becomes 2 at the end of that frame.
namespace {

struct Shown {
    uint32_t header = 0, data = 0;
};
Shown g_shown[2];  // the back buffer of the last two frames ended, newest first

void CopySurface(X86Context& ctx, uint32_t header, uint32_t from, uint32_t to) {
    static uint32_t src = 0, dst = 0;  // surface headers of our own, the device's back buffer with another memory
    if (!src) {
        src = mem::AllocSystem(32);
        dst = mem::AllocSystem(32);
    }
    for (uint32_t i = 0; i < 32; i += 4) {
        kernel::W32(src + i, kernel::R32(header + i));
        kernel::W32(dst + i, kernel::R32(header + i));
    }
    kernel::W32(src + 4, from);
    kernel::W32(dst + 4, to);
    constexpr uint32_t kCopyRects = 0x1153E0;  // D3DDevice_CopyRects(src, rects, count, dst, points), whole surface
    kernel::CallGuest(ctx, kCopyRects, {src, 0, 0, dst, 0});
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00100690);
X86_FUNC(sub_00100690) {
    const gpu::Surface rt = d3d::State().rt;
    if (kernel::R32(0x1E0780) == 1 && kernel::R32(0x1BEB1C) == 3 && rt.header && g_shown[0].header == rt.header &&
        g_shown[0].data && g_shown[0].data != rt.data) {
        CopySurface(ctx, rt.header, g_shown[0].data, rt.data);
        if (g_shown[1].header == rt.header && g_shown[1].data && g_shown[1].data != rt.data &&
            g_shown[1].data != g_shown[0].data)
            CopySurface(ctx, rt.header, g_shown[0].data, g_shown[1].data);
    }
    g_shown[1] = g_shown[0];
    g_shown[0] = {rt.header, rt.data};
    __imp__sub_00100690(ctx, base);
}

// FIELD OF VIEW: 0x102990 sets a view's projection (thiscall, three arguments, the first the field of view top to
// bottom in degrees; ret 0xC) through D3DXMatrixPerspectiveFovLH. The mission's camera asks it from
// 0x84746 each frame with twice the camera's half field [camera+0x60]: widened there as on foot, on the tangent, so a
// zoom keeps its power. Only while driving ([0x1C2C2C]+0x408 clear): the on-rails gunner's rifle and arms are drawn
// with the view's own projection, and a wider view stretched them across the screen, so rails keep the game's 66
// degrees (33 doubled; 40 while aiming). The game's own uses of the field (the aim's rates, the crosshair) are left.
X86_EXTERN_FUNC(__imp__sub_00102990);
X86_FUNC(sub_00102990) {
    constexpr uint32_t kMissionCamera = 0x8474B;  // the return address of the call at 0x84746
    constexpr uint32_t kMission = 0x1C2C2C;
    const uint32_t mission = kernel::R32(kMission);
    if (kernel::R32(ctx.esp) == kMissionCamera && GuestPointer(mission) && !kernel::G<uint8_t>(mission + 0x408))
        F(ctx.esp + 4) = fov::Apply(F(ctx.esp + 4));
    __imp__sub_00102990(ctx, base);
}
