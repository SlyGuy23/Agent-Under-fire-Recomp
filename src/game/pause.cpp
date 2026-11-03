// bond_eng.xbe's pause menu: START (Escape) shuts it from any of its pages, rows or questions.
//
// The game's menus take START as A (its menu input reads both as select): on the pause menu START picked the lit row,
// so on Restart Mission it asked "Are you sure?", and on the OBJECTIVES or BRIEFING page it did nothing; B does not
// shut the pause menu either. Only Continue Mission does: its command (id 4 in the menu command handler 0x4AA40,
// case 0x4AF46) calls 0x34040 (in one-player games 0x26550: [0x3B2428] = 0, the pause screen's state reset) and then
// 0x60B40 (the sounds paused with the game go on).
//
// menus.cpp keeps START from the game while it is paused and asks for the close (menus::TakePauseClose). 0x33920
// (screen, time), which the game calls every frame for the menu screen up, runs the Continue Mission command itself
// first when asked, as the pause menu does when the row is picked (from inside that screen's tick, its deferred
// command at +0x54 / +0x60 of the menu), then ticks the screen as it would have. Only for the one-player pause screen
// ([0x205354], screen 2 made at the level's start) with the game paused: the other screens (mission complete or
// failed) are left alone.
#include <cstdint>

#include "common/controls.h"
#include "common/menus.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(sub_00034040);
X86_EXTERN_FUNC(sub_00060B40);

X86_EXTERN_FUNC(__imp__sub_00033920);
X86_FUNC(sub_00033920) {
    constexpr uint32_t kPaused = 0x3B2428, kPauseScreen = 0x205354;
    const uint32_t screen = kernel::R32(ctx.esp + 4);
    // split screen: each player's pause menu is ticked with [0x3B23FC] = that player (0x26F4E), on whom 0x34040 acts
    const uint32_t player = kernel::R32(0x3B23FC);
    const bool split = int32_t(kernel::R32(0x205408)) > 1 && player < 4 && kernel::R32(0x3B2200 + player * 0x10) == 3 &&
                       screen == kernel::R32(0x3B2204 + player * 0x10);
    if ((kernel::R32(kPaused) && screen == kernel::R32(kPauseScreen) && menus::TakePauseClose()) ||
        (split && controls::TakeSplitPauseClose(int(player)))) {
        // called as the game calls them, each with a return address on the stack that its ret takes off
        X86_PUSH32(0x4AF4Bu);
        sub_00034040(ctx, base);
        X86_PUSH32(0x4AF58u);
        sub_00060B40(ctx, base);
    }
    __imp__sub_00033920(ctx, base);
}
