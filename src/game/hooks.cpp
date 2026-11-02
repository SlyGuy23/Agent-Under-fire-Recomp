// Hooks for bond_eng.xbe. A strong definition replaces the recompiled function at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00012345);
// X86_FUNC(sub_00012345) {
//     __imp__sub_00012345(ctx, base);  // the original
// }

#include "common/controls.h"
#include "common/damage_fx.h"
#include "common/fov.h"
#include "cpu/x86_ops.h"

#include <algorithm>
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

// Stick sensitivity past 100% on foot: the game turns the view in proportion to the stick (0x24130 yaw, 0x24260 pitch,
// at most 180 and 90 degrees a second times its look scale), so a stick can turn it no faster than full tilt does.
// 0x24570 builds player p's command from the pad, turning the client view angles (0x2A8124 pitch, 0x2A8128 yaw; the
// mouse look of game_info.cpp is already in them) by the stick on the way: what it turned player 0's view by is
// scaled by the part of the stick's sensitivity the pad could not carry (controls::TurnGains). In manual aim (0x2A814C)
// and on a mounted gun (the player's gentity flag 0x100) the game puts the angles back and aims with the left stick
// (0x24310, 0x24490), otherwise the right stick's X turns and its Y looks up and down (the game's setup 1).
X86_EXTERN_FUNC(__imp__sub_00024570);
X86_FUNC(sub_00024570) {
    constexpr uint32_t kPitch = 0x2A8124, kYaw = 0x2A8128, kAiming = 0x2A814C, kGentities = 0x2A88A8;
    float lx, ly, rx, ry;
    controls::TurnGains(lx, ly, rx, ry);
    const uint32_t player = X86_MEM32(ctx.esp + 8);
    if (player != 0 || (lx == 1 && ly == 1 && rx == 1 && ry == 1)) {
        __imp__sub_00024570(ctx, base);
        return;
    }
    const uint32_t gent = X86_MEM32(kGentities);
    const bool left = X86_MEM32(kAiming) != 0 || (gent && (X86_MEM32(gent + 8) & 0x100));
    const float pitch = X86_MEMF32(kPitch), yaw = X86_MEMF32(kYaw);
    __imp__sub_00024570(ctx, base);
    X86_MEMF32(kYaw) = yaw + (X86_MEMF32(kYaw) - yaw) * (left ? lx : rx);
    X86_MEMF32(kPitch) = pitch + (X86_MEMF32(kPitch) - pitch) * (left ? ly : ry);
}

// FIELD OF VIEW: 0xB5FB0, the player's end of frame, copies what the player sees into its playerState, the field of
// view (player+0x4A0: 60 degrees top to bottom, less while a scope or zoom is in use) to ps+0x2E0. Everything after
// reads it from there: the view's projection and culling (0x15DAA0 into the render camera, 0x357E54 in radians), the
// HUD's copy with its projection (0x55C20) and the aim. It is widened there, on the tangent, so a zoom keeps its
// magnification. A cinematic camera (pm_flags 0x100, its field at ps+0x30C) is left as the game frames it.
// 0xB5FB0 only runs in frames the game plays: paused (the pause menu's OPTIONS, where FIELD OF VIEW is changed most)
// the world kept the field it was last widened to while the weapon and hands (0x15C920, asked every drawn frame) took
// the new setting, so only the hands moved. The game's own field is kept here and the view's is widened again from it
// each drawn frame, when the camera is set up (0x15DAA0), so the setting shows at once, paused or not.
namespace {
struct ViewField {
    uint32_t ps = 0;
    float game = 0;      // the game's own field (player+0x4A0 as 0xB5FB0 copied it)
    uint32_t shown = 0;  // the bits written to ps+0x2E0 from it
};
ViewField g_view_fields[4];

ViewField& FieldOf(uint32_t ps) {
    for (ViewField& f : g_view_fields)
        if (f.ps == ps) return f;
    for (ViewField& f : g_view_fields)
        if (!f.ps) return f;
    return g_view_fields[ps / 4 % 4];
}

void WidenField(uint8_t* base, ViewField& f) {
    const float shown = fov::Apply(f.game);
    X86_MEMF32(f.ps + 0x2E0) = shown;
    std::memcpy(&f.shown, &shown, 4);
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_000B5FB0);
X86_FUNC(sub_000B5FB0) {
    const uint32_t player = ctx.ecx;
    __imp__sub_000B5FB0(ctx, base);
    const uint32_t ps = X86_MEM32(player + 0x10);
    if (!ps || (X86_MEM32(ps + 0x0C) & 0x100)) return;
    ViewField& f = FieldOf(ps);
    f.ps = ps;
    f.game = X86_MEMF32(ps + 0x2E0);
    WidenField(base, f);
}

// the view's camera for local player [esp+4], each drawn frame: its playerState's field widened again from the game's
// own at the current setting, unless something other than 0xB5FB0 wrote the field since (or a cinematic camera is up)
X86_EXTERN_FUNC(__imp__sub_0015DAA0);
X86_FUNC(sub_0015DAA0) {
    const int32_t players = int32_t(X86_MEM32(0x205408));
    const uint32_t states = X86_MEM32(0x2A88B8);
    if (players > 0 && states) {
        const uint32_t ps = states + uint32_t(int32_t(X86_MEM32(ctx.esp + 4)) % players) * X86_MEM32(0x2A88BC);
        for (ViewField& f : g_view_fields) {
            if (f.ps != ps || X86_MEM32(ps + 0x2E0) != f.shown || (X86_MEM32(ps + 0x0C) & 0x100)) continue;
            WidenField(base, f);
        }
    }
    __imp__sub_0015DAA0(ctx, base);
}

// The weapon and hands in view have a projection of their own (0x14B600), at a fraction of the view's field
// (0x15C920: ps+0x340, 0.55, times ps+0x2E0, in radians): from the game's own field, so they keep their size and shape
// at any FIELD OF VIEW. 0x644xx, which carries points between the two projections, asks the same function.
X86_EXTERN_FUNC(__imp__sub_0015C920);
X86_FUNC(sub_0015C920) {
    const uint32_t local = X86_MEM32(ctx.esp + 4);
    __imp__sub_0015C920(ctx, base);
    const int32_t players = int32_t(X86_MEM32(0x205408));
    const uint32_t states = X86_MEM32(0x2A88B8);
    if (players <= 0 || !states) return;
    const uint32_t ps = states + uint32_t(int32_t(local) % players) * X86_MEM32(0x2A88BC);
    // the game's own field as 0xB5FB0's hook kept it while the view's is the one widened from it (the setting may have
    // changed since, paused), else the view's undone
    const ViewField* kept = nullptr;
    for (const ViewField& f : g_view_fields)
        if (f.ps == ps && X86_MEM32(ps + 0x2E0) == f.shown) kept = &f;
    if (!kept && fov::g_offset.load(std::memory_order_relaxed) == 0) return;
    const float game = kept ? kept->game : fov::Unapply(X86_MEMF32(ps + 0x2E0));
    constexpr double kRad = 3.14159265358979 / 180;
    X87_ST(0) = double(X86_MEMF32(ps + 0x340)) * double(game) * kRad;
}

// The crosshair (0x51320, the HUD's crosshair element in esi, called each frame from 0x55589) is picked by the state
// the player's think leaves in ps+0xC4, seen through the HUD's copy ([esi+0x2F0] = HUD player + 0x158): 1 the normal
// crosshair_auto_aim_on, 0x201 crosshair_auto_aim_on_targeted (a larger cross, whenever the aim trace 0xB2750 meets an
// enemy, locked on or not), 2 crosshair_manual, 0x107 crosshair_qcamera. With the keyboard and mouse the crosshair keeps
// its normal size over an enemy: the targeted state is drawn as the normal one. The state itself is left as it was.
X86_EXTERN_FUNC(__imp__sub_00051320);
X86_FUNC(sub_00051320) {
    constexpr uint32_t kTargeted = 0x201, kNormal = 1;
    const uint32_t state = X86_MEM32(ctx.esi + 0x2F0);
    if (!state || X86_MEM32(state) != kTargeted || controls::ActiveDevice() != controls::Device::KeyboardMouse) {
        __imp__sub_00051320(ctx, base);
        return;
    }
    X86_MEM32(state) = kNormal;
    __imp__sub_00051320(ctx, base);
    X86_MEM32(state) = kTargeted;
}

// GOLDEN ACCURACY in manual aim. The gold reward (bit 3 of the rewards mask, 0x3BEB0: Gold Rewards on, [0x20732C], and
// mission 4's reward earned, [0x2A9BBC + 3 * 4]) only reaches the Golden Gun: its "rewardlevel" handler (0xA0720, class
// SPGoldenGun) zeroes the weapon's bulletspread (+0x4A0/+0x4A4); every other weapon's handler is empty (0x9A530). The
// player's shot goes from the eye along the aim angles (player+0x758/+0x75C, which manual aim moves with the crosshair,
// 0xB7FA0) and is scattered by the spread 0x9DB30 works out (the weapon's base, plus what firing adds, up to its
// maximum) before 0x83D50 traces it. With the reward on and the player in manual aim (player+0x671), the Golden Gun's
// shot is fired with no spread at all: exactly where the crosshair points. Other weapons keep their spread, as the
// reward is the Golden Gun's alone. Outside manual aim the game's own aim (auto-aim included) and spread are left as
// they are; the crosshair's size (0x12E0B7 asks the same function) is untouched.
X86_EXTERN_FUNC(__imp__sub_0009DB30);
X86_FUNC(sub_0009DB30) {
    constexpr uint32_t kFireSite = 0xA36F0;  // the return address of the player's fire (0xA36EB)
    const uint32_t ret = X86_MEM32(ctx.esp), out = X86_MEM32(ctx.esp + 4), weapon = ctx.ecx;
    __imp__sub_0009DB30(ctx, base);
    if (ret != kFireSite || !out) return;
    const uint32_t game = X86_MEM32(0x328664);  // deathmatch cvar: single player only
    if (game && X86_MEM32(game + 0x20) != 0) return;
    if (X86_MEM32(0x20732C) == 0 || X86_MEM32(0x2A9BBC + 3 * 4) == 0) return;  // Golden Accuracy not on
    constexpr uint32_t kGoldenGun = 0x1DF778;  // SPGoldenGun's vtable: the reward is the Golden Gun's alone
    if (X86_MEM32(weapon) != kGoldenGun) return;
    const uint32_t owner = X86_MEM32(weapon + 0x2CC), entities = X86_MEM32(0x329390);
    if (!owner || !entities) return;
    const int32_t players = std::clamp(int32_t(X86_MEM32(0x205408)), 1, 4);
    bool player = false;
    for (int32_t n = 0; n < players; n++) player = player || X86_MEM32(entities + uint32_t(n) * 0x2E0 + 0x278) == owner;
    if (!player || X86_MEM8(owner + 0x671) == 0) return;
    X86_MEMF32(out) = 0;
    X86_MEMF32(out + 4) = 0;
}

// DAMAGE EFFECTS, the flash: the HUD's per-player think (0x534DC) sets its hit flash, HUD+0x30, to 0.4 to 0.9 (more as
// the health gets lower) whenever the health drops, and its draw (0x539A9) covers the player's view with a rectangle of
// colour (1, 1, 1, HUD+0x30) through 0x43CA0 (x, y, w, h, colour*), then fades it. That one call is scaled (the colour is
// the caller's, built for the call) or left out, and drawn in black with DAMAGE FLASH COLOUR. The fade keeps its time.
X86_EXTERN_FUNC(__imp__sub_00043CA0);
X86_FUNC(sub_00043CA0) {
    constexpr uint32_t kHitFlash = 0x53A58;  // the return address of the hit flash's draw (0x53A53)
    if (X86_MEM32(ctx.esp) != kHitFlash) {
        __imp__sub_00043CA0(ctx, base);
        return;
    }
    const float scale = damage_fx::Scale();
    if (scale <= 0) {
        ctx.esp += 4;  // cdecl: the call's return
        return;
    }
    const uint32_t colour = X86_MEM32(ctx.esp + 0x14);
    if (colour) {
        if (damage_fx::g_flash_black)
            for (int i = 0; i < 3; i++) X86_MEMF32(colour + 4 * i) = 0;
        X86_MEMF32(colour + 12) *= scale;
    }
    __imp__sub_00043CA0(ctx, base);
}

// DAMAGE EFFECTS, the view kick: 0x27910 (local client n, cdecl), the client's damage feedback, sees the playerState's
// damage count change (ps+0xDC, from the player's DamageFeedback 0xAF2D0) and starts a kick: the time ([0x3B2280] into
// +0x6C of the client's view state, 0x2A8490 + n * 0xB0) and how far the view pitches (+0x70) and rolls (+0x74), which
// 0x280A7 adds to the view angles over the next tenth of a second or so. Those two are scaled when a kick starts. The
// script's screen shakes (ps+0xF0, explosions and the like) and the landing dip are separate and left as they are.
X86_EXTERN_FUNC(__imp__sub_00027910);
X86_FUNC(sub_00027910) {
    const uint32_t view = 0x2A8490 + X86_MEM32(ctx.esp + 4) * 0xB0;
    const uint32_t started = X86_MEM32(view + 0x6C);
    __imp__sub_00027910(ctx, base);
    const float scale = damage_fx::Scale();
    if (scale == 1 || X86_MEM32(view + 0x6C) == started) return;
    X86_MEMF32(view + 0x70) *= scale;
    X86_MEMF32(view + 0x74) *= scale;
}
