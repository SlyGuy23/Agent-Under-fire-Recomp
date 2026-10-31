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
