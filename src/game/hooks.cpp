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
