// Hooks for default.xbe (the intro films). A strong definition replaces the recompiled function at that address.
//
// Its main (0x21060) plays the films one after another with 0x20BD0 (the .MPC file's name): EAG.MPC, two seconds of
// blank frames, MGM.MPC, then the attract film ATT.MPC (or HILITE.MPC), then 0x22480 starts the game (bond_eng.xbe,
// through XLaunchNewImage). A film's frame loop (0x20E60 to 0x20FC6) and its wait for the sound to end run until
// [0x1F046C] is set: by 0x20A70, the loop's input check, on a button or a controller plugged in (only once
// [0x222488] is set, for the attract film), or here.
//
// A press skips them all at once (game_info.cpp, overlay::g_intros_done): from then on the film playing stops at its
// next frame, no other film starts, and the launcher does not start the game a second time (the press does).
#include <chrono>
#include <thread>

#include "common/overlay.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {
constexpr uint32_t kFilmStop = 0x1F046C;
}

// the film loop's input check: the film stops
X86_EXTERN_FUNC(__imp__sub_00020A70);
X86_FUNC(sub_00020A70) {
    if (overlay::g_intros_done.load()) {
        kernel::W32(kFilmStop, 1);
        ctx.esp += 4;  // its ret
        return;
    }
    __imp__sub_00020A70(ctx, base);
}

// a film (cdecl, its name): not played
X86_EXTERN_FUNC(__imp__sub_00020BD0);
X86_FUNC(sub_00020BD0) {
    if (overlay::g_intros_done.load()) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_00020BD0(ctx, base);
}

// the game started after the films: by the launcher only when no press did it first. Otherwise this thread waits
// for the process to end, which it does once the game's window is up.
X86_EXTERN_FUNC(__imp__sub_00022480);
X86_FUNC(sub_00022480) {
    if (overlay::g_intros_done.exchange(true))
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    __imp__sub_00022480(ctx, base);
}
