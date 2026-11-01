// Autosave after each completed mission (bond_eng.xbe, PAL), on foot or driving and on rails.
//
// The game keeps every mission save in one save game, "BOND: AUF" (U:\<id>\xbsavegame.dat), 0x1570 bytes it holds in
// memory at [0x2AA048] (0x2BF6F0): after a version string, nine slots of 0xF8 bytes from +0x10, each the name the
// player gave it (12 bytes, "[------]" for an empty one), the mission progress and the OPTIONS. The profile being
// played is the same 0xF8 bytes at 0x2BE190. SAVE MISSION ends (0x3FB71) with
//   0x42910(slot)    the profile, built again from the game's globals (0x425D0), into the slot, its name kept
//   0x43630(device)  the 0x1570 bytes written to the device ([0x2A9D20]: -2 the hard disk), 0 when it worked
// and LOAD MISSION reads them first (0x43670(device), 0 when it worked) and loads a slot with 0x428A0(slot).
//
// A won mission is recorded by 0x424E0(mission, score) while [0x20C5C4] is 1, which only the level's "endlevel"
// event (0xB10D0) sets: on foot from the MISSION RESULTS screen (0x3BF60), after a driving or on-rails mission when
// the game starts again from driving.xbe with it won in the launch data (0x43740, run at boot from 0x25E30). The
// medal and the rewards follow on the next page of the front end, so the autosave waits for the front end to settle
// on a page of its own (no level, the page [0x2079D0] changed and unchanged for a second) and then does what SAVE
// MISSION does, over the slot the player loaded or saved last, without its screens. A failed or quit mission records
// nothing; with no save file loaded or saved yet nothing is written.
//
// driving.xbe is another process: the slot and its device go with the launch data (0x2A5138, 0xC00 bytes, which
// driving.xbe hands back whole), in 16 bytes at +0x540 the game leaves unused (+0 the launch object, +0x1A4 the state
// at 0x3B22A0 and +0x2EC the profile up to +0x3E4, the block 0x23210 keeps from +0x554).

#include "common/autosave.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "kernel/kernel.h"

#include <cstring>

namespace {

constexpr uint32_t kSaveData = 0x2AA048;                 // the save game's 0x1570 bytes in memory, 0 before one is read
constexpr uint32_t kSlots = 0x10, kSlotSize = 0xF8, kSlotCount = 9;
constexpr uint32_t kProfile = 0x2BE190;                  // the profile being played: a slot's layout, its name first
constexpr uint32_t kEmptyName = 0x1D2AD4;                // "[------]"
constexpr uint32_t kWon = 0x20C5C4;                      // 1: a won mission whose score is still to be recorded
constexpr uint32_t kPlayerState = 0x2A88B8;              // a level loaded
constexpr uint32_t kPage = 0x2079D0;                     // the front end page
constexpr uint32_t kLevelPage = 0x78, kResultsPage = 0x5;  // a level played, MISSION RESULTS after a driving one
constexpr uint32_t kTag = 0x2A5138 + 0x540, kMagic = 0x56534641;  // "AFSV"
constexpr uint32_t kReadSave = 0x43670, kStoreProfile = 0x42910, kWriteSave = 0x43630;
constexpr int kSettleFrames = 60;

struct SaveFile {
    bool known = false;
    int32_t device = 0;
    uint32_t slot = 0;
};
SaveFile g_file;           // the slot the player loaded or saved last
int32_t g_read_device = 0;  // the device the save game in memory came from
bool g_read = false;
int g_storing = -1;         // a slot 0x42910 filled for the write that follows

bool g_pending = false;     // a won mission recorded, not saved yet
uint32_t g_pending_page = 0, g_page = 0;
int g_settled = 0;

uint32_t SlotAt(uint32_t slot) { return kernel::R32(kSaveData) + kSlots + slot * kSlotSize; }

bool Named(uint32_t slot) {
    return kernel::R32(kSaveData) && slot < kSlotCount && std::memcmp(kernel::HostPtr(SlotAt(slot)), kernel::HostPtr(kEmptyName), 9) != 0;
}

const char* Device(int32_t device) { return device == -2 ? "the hard disk" : "a memory unit"; }

void Save(X86Context& ctx) {
    if (!autosave::g_enabled) return;
    if (!g_file.known) {
        LOGR("Autosave: mission won, no save file loaded or saved yet: nothing written");
        return;
    }
    const SaveFile f = g_file;
    // what is on the device now (after driving.xbe nothing is in memory), and the slot still the profile played
    uint32_t error = kernel::CallGuest(ctx, kReadSave, {uint32_t(f.device)});
    if (error || !kernel::R32(kSaveData)) {
        LOGR("Autosave: the save game on %s could not be read (%X): nothing written", Device(f.device), error);
        return;
    }
    if (!Named(f.slot) || std::memcmp(kernel::HostPtr(SlotAt(f.slot)), kernel::HostPtr(kProfile), 12) != 0) {
        LOGR("Autosave: slot %u on %s is no longer the mission being played: nothing written", f.slot + 1, Device(f.device));
        return;
    }
    char name[13] = {};
    std::memcpy(name, kernel::HostPtr(kProfile), 12);
    kernel::CallGuest(ctx, kStoreProfile, {f.slot});
    error = kernel::CallGuest(ctx, kWriteSave, {uint32_t(f.device)});
    if (error) LOGR("Autosave: writing %s (slot %u) on %s failed (%X)", name, f.slot + 1, Device(f.device), error);
    else LOGR("Autosave: mission progress saved to %s (slot %u) on %s", name, f.slot + 1, Device(f.device));
}

}  // namespace

// the save game read from a device: where a slot loaded next comes from
X86_EXTERN_FUNC(__imp__sub_00043670);
X86_FUNC(sub_00043670) {
    const int32_t device = int32_t(X86_MEM32(ctx.esp + 4));
    __imp__sub_00043670(ctx, base);
    g_read = ctx.eax == 0 && kernel::R32(kSaveData);
    if (g_read) g_read_device = device;
}

// LOAD MISSION: a slot with a name becomes the profile played, and the file the autosave writes
X86_EXTERN_FUNC(__imp__sub_000428A0);
X86_FUNC(sub_000428A0) {
    const uint32_t slot = X86_MEM32(ctx.esp + 4);
    const bool named = g_read && Named(slot);
    __imp__sub_000428A0(ctx, base);
    if (!named) return;
    g_file = {true, g_read_device, slot};
    LOGR("Autosave: slot %u on %s loaded", slot + 1, Device(g_read_device));
}

// SAVE MISSION: the slot filled, then written; once on the device it is the file the autosave writes
X86_EXTERN_FUNC(__imp__sub_00042910);
X86_FUNC(sub_00042910) {
    g_storing = int(X86_MEM32(ctx.esp + 4));
    __imp__sub_00042910(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00043630);
X86_FUNC(sub_00043630) {
    const int32_t device = int32_t(X86_MEM32(ctx.esp + 4));
    __imp__sub_00043630(ctx, base);
    if (ctx.eax == 0 && g_storing >= 0 && !(g_file.known && g_file.device == device && g_file.slot == uint32_t(g_storing))) {
        g_file = {true, device, uint32_t(g_storing)};
        LOGR("Autosave: slot %u on %s saved", g_file.slot + 1, Device(device));
    }
    g_storing = -1;
}

// a mission's score recorded: a won one is saved once the front end has settled
X86_EXTERN_FUNC(__imp__sub_000424E0);
X86_FUNC(sub_000424E0) {
    const bool won = kernel::R32(kWon) == 1;
    const uint32_t mission = X86_MEM32(ctx.esp + 4);
    __imp__sub_000424E0(ctx, base);
    if (!won) return;
    g_pending = true;
    g_pending_page = g_page = kernel::R32(kPage);
    g_settled = 0;
    LOGR("Autosave: mission %u won", mission + 1);
}

// before the game starts driving.xbe (0x25DB0) or another executable: the save file goes with the launch data
X86_EXTERN_FUNC(__imp__sub_00043A10);
X86_FUNC(sub_00043A10) {
    __imp__sub_00043A10(ctx, base);
    kernel::W32(kTag, g_file.known ? kMagic : 0);
    kernel::W32(kTag + 4, uint32_t(g_file.device));
    kernel::W32(kTag + 8, g_file.slot);
    kernel::W32(kTag + 12, 0);
}

// the game's start (its launch data read just before, at 0x268F0), which ends recording a driving or on-rails mission
// won (0x43740): the save file comes back from the launch data first
X86_EXTERN_FUNC(__imp__sub_00025E30);
X86_FUNC(sub_00025E30) {
    if (kernel::R32(kTag) == kMagic && kernel::R32(kTag + 8) < kSlotCount) {
        g_file = {true, int32_t(kernel::R32(kTag + 4)), kernel::R32(kTag + 8)};
        LOGR("Autosave: back from another executable, slot %u on %s", g_file.slot + 1, Device(g_file.device));
    }
    kernel::W32(kTag, 0);
    __imp__sub_00025E30(ctx, base);
}

// the front end's frame (its game state's update in the table at 0x2053A0: the menus of each player updated, 0x33920,
// and drawn, 0x34910): a won mission is saved when no level is loaded and the front end has been for a second on a
// page past the results. After a mission on foot the results show on the level's page (0x78), after a driving one on
// page 5 (the game's first page then); the next, 0x57, gives the medal and the rewards on entry (0x45369, 0x45394).
X86_EXTERN_FUNC(__imp__sub_00026B10);
X86_FUNC(sub_00026B10) {
    __imp__sub_00026B10(ctx, base);
    game_options::Frame(ctx);
    if (!g_pending || kernel::R32(kPlayerState)) return;
    const uint32_t page = kernel::R32(kPage);
    if (page != g_page) {
        g_page = page;
        g_settled = 0;
        return;
    }
    if (page == g_pending_page || page == kLevelPage || page == kResultsPage || ++g_settled < kSettleFrames) return;
    g_pending = false;
    Save(ctx);
}
