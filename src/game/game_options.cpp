// The game's OPTIONS kept across starts (bond_eng.xbe, PAL): src/common/game_options.h holds them in the settings file.
//
// The OPTIONS page (front end page 9, and the pause menu's) changes player 0's entries of the game's per-player
// arrays: Difficulty [0x207320], Invert Pitch byte [0x3B22D5], Vibration [0x207328], HUD Always Visible [0x2A9C64],
// Cross Hair [0x207324], Gold Rewards [0x20732C], Music [0x207340] and Sound Effects [0x207338] (0 to [0x20733C] and
// [0x207334], 128); Controller Setup's page [0x2A9CD4] (0 to 3; the port's own controls replace what it maps to,
// [0x205288], but the choice is kept). A save holds them in its profile (+0x48 to +0x64, 0x425D0), and loading one
// puts them back (0x42760, also at the start after a driving mission), after which the volumes go to the sound
// (0x37780) and the controller setup to its table (0x37760).
//
// Here: at the front end's first frame (the game's own start is done, its defaults set) the remembered values are put
// in, or, with none in the settings file yet, the game's are taken; after a save's are put back, the remembered ones
// again. A change the player makes is seen on the next frame (front end or level) and remembered.
// driving.xbe takes these from bond_eng.xbe in its launch data (0x420C0, from 0x43A10), so it plays with them too.

#include "common/game_options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "kernel/kernel.h"

#include <algorithm>

namespace {

using game_options::kCount;

struct Field {
    uint32_t va;
    bool byte;
    int32_t max;     // 0: read from max_va
    uint32_t max_va;
};
constexpr Field kFields[kCount] = {
    {0x207320, false, 2, 0},       // difficulty
    {0x2A9CD4, false, 3, 0},       // controller setup
    {0x3B22D5, true, 1, 0},        // invert pitch
    {0x207328, false, 1, 0},       // vibration
    {0x2A9C64, false, 1, 0},       // HUD always visible
    {0x207324, false, 1, 0},       // cross hair
    {0x20732C, false, 1, 0},       // gold rewards
    {0x207340, false, 0, 0x20733C},  // music
    {0x207338, false, 0, 0x207334},  // sound effects
};
constexpr uint32_t kApplyControllerSetup = 0x37760, kApplyVolumes = 0x37780;

bool g_applied = false;     // the remembered values in, from the front end's first frame
game_options::Values g_seen;  // the game's values last frame

game_options::Values Read() {
    game_options::Values v;
    for (int i = 0; i < kCount; i++) {
        const Field& f = kFields[i];
        v.have[i] = true;
        v.value[i] = f.byte ? kernel::G<uint8_t>(f.va) : int32_t(kernel::R32(f.va));
    }
    return v;
}

bool Same(const game_options::Values& a, const game_options::Values& b) {
    for (int i = 0; i < kCount; i++)
        if (a.value[i] != b.value[i]) return false;
    return true;
}

void Write(X86Context& ctx, const game_options::Values& v) {
    for (int i = 0; i < kCount; i++) {
        if (!v.have[i]) continue;
        const Field& f = kFields[i];
        int32_t max = f.max ? f.max : int32_t(kernel::R32(f.max_va));
        int32_t x = std::clamp(v.value[i], 0, std::max(max, 0));
        if (f.byte) kernel::G<uint8_t>(f.va) = uint8_t(x);
        else kernel::W32(f.va, uint32_t(x));
    }
    kernel::CallGuest(ctx, kApplyControllerSetup, {});
    kernel::CallGuest(ctx, kApplyVolumes, {});
}

}  // namespace

namespace game_options {

void Frame(X86Context& ctx) {
    if (!g_applied) {
        g_applied = true;
        Values kept = Get(), game = Read();
        bool all = true;
        for (int i = 0; i < kCount; i++)
            if (!kept.have[i]) kept.have[i] = true, kept.value[i] = game.value[i], all = false;
        Write(ctx, kept);
        g_seen = Read();
        if (!all) Set(g_seen);  // a settings file without them (or some) takes the game's
        LOGR("Game options: %s", all ? "the remembered ones put in" : "the game's taken into the settings file");
        return;
    }
    Values now = Read();
    if (Same(now, g_seen)) return;
    g_seen = now;
    Set(now);
    LOGR("Game options: changed, remembered");
}

}  // namespace game_options

// a profile's settings and progress put in (a save loaded, or the start after a driving mission): the remembered
// OPTIONS win over the save's
X86_EXTERN_FUNC(__imp__sub_00042760);
X86_FUNC(sub_00042760) {
    __imp__sub_00042760(ctx, base);
    if (!g_applied) return;  // the start: the front end's first frame puts them in
    Write(ctx, game_options::Get());
    g_seen = Read();
}

// a level's frame (the game state's update, 0x26D00, ends with the end of level check 0x33390): options changed in the
// pause menu are remembered at once
X86_EXTERN_FUNC(__imp__sub_00033390);
X86_FUNC(sub_00033390) {
    __imp__sub_00033390(ctx, base);
    if (g_applied) game_options::Frame(ctx);
}
