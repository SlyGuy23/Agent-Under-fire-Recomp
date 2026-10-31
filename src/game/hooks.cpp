// Hooks for bond_eng.xbe. A strong definition replaces the recompiled function at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00012345);
// X86_FUNC(sub_00012345) {
//     __imp__sub_00012345(ctx, base);  // the original
// }

#include "common/controls.h"
#include "cpu/x86_ops.h"

#include <cmath>

// A (the action button) also reloads in the game: 0xB7390, the "use" function it calls, ends by reloading the weapon
// in hand (0x742E0 for the weapon, at 0xB7D1D, then its vfunc +0x220) when nothing was used. With Reload bound on its
// own (controls.cpp) the weapon looked up for that reload is none, so A only uses and opens things; everything
// before it in 0xB7390 runs as it did. The port's Reload asks the game itself ([0x333524], game_info.cpp).
X86_EXTERN_FUNC(__imp__sub_000742E0);
X86_FUNC(sub_000742E0) {
    constexpr uint32_t kReloadSite = 0xB7D22;  // the return address of the call at 0xB7D1D
    if (X86_MEM32(ctx.esp) == kReloadSite && controls::SeparateReload()) {
        ctx.eax = 0;
        ctx.esp += 4;  // the call's return
        return;
    }
    __imp__sub_000742E0(ctx, base);
}

// The frame time ([0x3B2420], what movement and everything timed runs on) comes from 0x1389F0, which with a
// presentation interval set measures between the last two vertical blank callbacks that saw a new frame. On the
// console Swap waits for the blank, so that is the frame's time; here the blanks come from a 60 Hz timer of their
// own while frames follow the frame cap and the monitor, so a frame could count as a whole blank interval or more:
// above 60 fps, or with the two clocks drifting on a 75 or 144 Hz monitor, the game ran fast (by chance a new
// process, as after a driving mission, could land on it). Its IMMEDIATE path times the real gap between frames with
// the performance counter: it is taken every time.
X86_EXTERN_FUNC(__imp__sub_001389F0);
X86_FUNC(sub_001389F0) {
    constexpr uint32_t kInterval = 0x346788;  // the game's presentation interval
    constexpr uint32_t kImmediate = 0x80000000u;
    uint32_t keep = X86_MEM32(kInterval);
    X86_MEM32(kInterval) = kImmediate;
    __imp__sub_001389F0(ctx, base);
    X86_MEM32(kInterval) = keep;
}

// Walking up a slope or stair, PM_StepSlideMove's step down clips the velocity to the ground: the horizontal part
// along the slope's rise is cut (vz gained) and PM_GroundTrace (0xC6FBE) then zeroes vz, so every frame the walk
// loses some of its uphill part and keeps all of its sideways part: the player turned along the stairs, away from
// where he looks (15 deg at the Xbox's 30 fps, 27 at 60, 41 at 144). On walkable ground the horizontal velocity is
// kept as it was, so the player goes up the way he faces; walls and flat ground are left to the game.
X86_EXTERN_FUNC(__imp__sub_000C5340);
X86_FUNC(sub_000C5340) {
    uint32_t pm = X86_MEM32(0x3B30B8u), ps = pm ? X86_MEM32(pm) : 0;
    float v0[3] = {};
    if (ps) for (int i = 0; i < 3; i++) v0[i] = X86_MEMF32(ps + 0x24 + 4 * i);
    __imp__sub_000C5340(ctx, base);
    if (!ps || !X86_MEM32(0x335468u)) return;           // pml.walking
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = X86_MEMF32(ps + 0x24 + 4 * i) - v0[i];
    float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), dh = std::hypot(d[0], d[1]);
    if (!(d[2] > 0 && d[2] >= 0.7f * dl && dh > 1e-4f)) return;  // only a clip on walkable ground
    X86_MEMF32(ps + 0x24) = v0[0];
    X86_MEMF32(ps + 0x28) = v0[1];                      // vz is zeroed by the ground trace anyway
}

// The game's auto-aim (0xB8BB0, the player's target search each think) bends the aim and the shots onto an enemy
// while player+0x716 is set (at spawn: every difficulty but 00 Agent). With the keyboard and mouse and the port's
// AUTO LOCK-ON off the search runs as if it were clear, so the aim follows the view; the byte is put back after, so
// the option can come back on at any time.
X86_EXTERN_FUNC(__imp__sub_000B8BB0);
X86_FUNC(sub_000B8BB0) {
    if (!controls::AimAssistOff()) {
        __imp__sub_000B8BB0(ctx, base);
        return;
    }
    const uint32_t player = ctx.ecx;
    const uint8_t allowed = X86_MEM8(player + 0x716);
    X86_MEM8(player + 0x716) = 0;
    __imp__sub_000B8BB0(ctx, base);
    X86_MEM8(player + 0x716) = allowed;
}
