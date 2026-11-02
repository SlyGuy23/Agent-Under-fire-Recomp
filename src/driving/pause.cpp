// driving.xbe's pause menu (driving and on-rails missions): START (Escape) shuts it from any of its pages, rows or
// questions.
//
// The pause menu's input handler 0xB5B00 (thiscall: the menu, an input event) takes event 6, START, as event 1, A
// (both select through its table at 0xB6134): START picked the lit row, so on Restart or Quit Mission it asked "Are
// you sure?" and on a question it answered it. The menu keeps its page at +0x420 (0 MISSION), the lit row at +0x424
// (0 Continue Mission), a question at +0x428 (0 none, 1 restart, 2 quit) and its answer at +0x42C. Event 6 now goes in
// as a select of Continue Mission on the MISSION page with no question up: the game's own way out of the pause menu.
// Other events (B 16, D-pad down 13, right 9 ...) are the game's. Only while the pause menu is up ([0x1BEB1C] 3).
#include <cstdint>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_000B5B00);
X86_FUNC(sub_000B5B00) {
    constexpr uint32_t kRunState = 0x1BEB1C, kPaused = 3;
    constexpr uint32_t kStart = 6, kSelect = 1;
    const uint32_t menu = ctx.ecx;
    if (kernel::R32(ctx.esp + 4) == kStart && kernel::R32(kRunState) == kPaused && menu) {
        kernel::W32(menu + 0x420, 0);  // MISSION
        kernel::W32(menu + 0x424, 0);  // Continue Mission
        kernel::W32(menu + 0x428, 0);  // no question
        kernel::W32(menu + 0x42C, 0);
        kernel::W32(ctx.esp + 4, kSelect);
    }
    __imp__sub_000B5B00(ctx, base);
}
