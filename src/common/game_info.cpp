#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "app/app.h"
#include "audio/audio.h"
#include "common/autosave.h"
#include "common/controls.h"
#include "common/damage_fx.h"
#include "common/fov.h"
#include "common/game_options.h"
#include "common/menus.h"
#include "common/overlay.h"
#include "common/sticks.h"
#include "image_symbols.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "gpu/state.h"
#include "gpu/texture.h"

#include <chrono>
#include <thread>

namespace {

constexpr const char* kGameExe = "AgentUnderFireGame";

// driving.xbe takes the game's OPTIONS from bond_eng.xbe in its launch data (bond_eng.xbe's 0x420C0, at +0x400 in
// launch.bin): difficulty +0x9C/+0xA0, music +0xE4 and sound effects +0xE8 (0 to 1), controller setup * 2 + invert
// pitch +0xEC, vibration +0xF4, HUD always visible +0x10C, no cross hair +0x110. Played again on its own, the kept data
// gets the remembered ones (src/common/game_options.h).
void DrivingOptions(const std::filesystem::path& launch) {
    using namespace game_options;
    Values v = Get();
    std::FILE* f = std::fopen(launch.string().c_str(), "r+b");
    if (!f) return;
    uint8_t d[0x1000];
    if (std::fread(d, 1, sizeof(d), f) == sizeof(d)) {
        auto put = [&](uint32_t at, uint32_t x) { std::memcpy(d + 0x400 + at, &x, 4); };
        auto putf = [&](uint32_t at, int32_t x) {
            float y = std::clamp(x, 0, 128) / 128.0f;
            std::memcpy(d + 0x400 + at, &y, 4);
        };
        auto get = [&](uint32_t at) {
            uint32_t x;
            std::memcpy(&x, d + 0x400 + at, 4);
            return x;
        };
        if (v.have[kDifficulty]) {
            static const uint32_t kLevels[3][2] = {{0, 5}, {1, 3}, {2, 2}};
            int i = std::clamp(v.value[kDifficulty], 0, 2);
            put(0x9C, kLevels[i][0]);
            put(0xA0, kLevels[i][1]);
        }
        if (v.have[kMusic]) putf(0xE4, v.value[kMusic]);
        if (v.have[kSoundEffects]) putf(0xE8, v.value[kSoundEffects]);
        uint32_t setup = get(0xEC);
        if (v.have[kControllerSetup]) setup = uint32_t(std::clamp(v.value[kControllerSetup], 0, 3)) * 2 | (setup & 1);
        if (v.have[kInvertPitch]) setup = (setup & ~1u) | (v.value[kInvertPitch] ? 1 : 0);
        put(0xEC, setup);
        if (v.have[kVibration]) put(0xF4, v.value[kVibration] ? 1 : 0);
        if (v.have[kHudAlwaysVisible]) put(0x10C, v.value[kHudAlwaysVisible] ? 1 : 0);
        if (v.have[kCrossHair]) put(0x110, v.value[kCrossHair] ? 0 : 1);
        std::fseek(f, 0, SEEK_SET);
        std::fwrite(d, 1, sizeof(d), f);
    }
    std::fclose(f);
}

// the folder the kernel boots with as the hard disk (main.cpp): hdd, or hddN for a second copy on this PC
std::filesystem::path HddDir() {
    return app::ExeDir() / (app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : std::string("hdd"));
}

// bond_eng.xbe expects to be started by default.xbe, whose launch data (0x22380 in default.xbe: its 0x1A4-byte object
// at 0x2252D0) says so: +4 "LOADER PRESENT", +0x94 2, +0xA4 the language file "IDL_ENG.LOC", +0x16C 1 (launches made).
// At boot (0x268F0) the game takes +4 as "driving.xbe is there" ([0x2A5D38]); with no launch data it is 0 and every
// driving and on-rails mission (Precious Cargo mis02a, Dangerous Pursuit mis02b, Streets of Bucharest mis07a, Forbidden
// Depths mis09, 0x4AB04) plays the Night of the Jackal level m6_1 in their place. The game is started without it when
// Skip Intros or a press during the films relaunches straight into it, or when AgentUnderFireGame.exe is run on its
// own: it then gets the same launch data the launcher hands over (read and removed by kernel::Boot after this).
// A launch.bin already there (the return from driving.xbe) is left as it is.
void LauncherLaunchData() {
    namespace fs = std::filesystem;
    fs::path hdd = HddDir(), launch = hdd / "launch.bin";
    std::error_code ec;
    if (fs::exists(launch, ec)) return;
    uint8_t page[0x1000] = {};
    auto put = [&](uint32_t at, uint32_t x) { std::memcpy(page + at, &x, 4); };
    auto str = [&](uint32_t at, const char* s) { std::memcpy(page + at, s, std::strlen(s) + 1); };
    put(0, 0);                     // LDT_TITLE
    put(4, 0x4541000D);            // the title
    str(8, "\\Device\\CdRom0;BOND_ENG.XBE");
    constexpr uint32_t kData = 0x400;
    str(kData + 4, "LOADER PRESENT");
    put(kData + 0x94, 2);
    str(kData + 0xA4, "IDL_ENG.LOC");
    put(kData + 0x16C, 1);
    fs::create_directories(hdd, ec);
    bool written = false;
    if (std::FILE* f = std::fopen(launch.string().c_str(), "wb")) {
        written = std::fwrite(page, 1, sizeof(page), f) == sizeof(page);
        std::fclose(f);
    }
    LOGR("Game: started without the launcher's launch data, given it%s", written ? "" : " (not written)");
}

// A driving mission is a separate executable the game relaunches into, with the mission in its launch data
// (hdd\launch.bin, read once at boot). That data is kept as hdd\driving_launch.bin, so starting
// AgentUnderFireDriving.exe on its own plays the last driving mission again without the on-foot mission before it.
void KeepDrivingLaunch(bool relaunched) {
    namespace fs = std::filesystem;
    fs::path hdd = HddDir(), launch = hdd / "launch.bin", kept = hdd / "driving_launch.bin";
    std::error_code ec;
    if (relaunched) {
        if (fs::exists(launch, ec)) fs::copy_file(launch, kept, fs::copy_options::overwrite_existing, ec);
        LOGR("Driving: mission launch data kept for a direct restart%s", ec ? " (not written)" : "");
    } else if (!fs::exists(launch, ec) && fs::exists(kept, ec)) {
        fs::copy_file(kept, launch, fs::copy_options::overwrite_existing, ec);
        if (!ec) DrivingOptions(launch);
        LOGR("Driving: started on its own, playing the last driving mission again%s", ec ? " (not read)" : "");
    } else if (!fs::exists(kept, ec)) {
        LOGR("Driving: started on its own with no driving mission played yet");
    }
}

// default.xbe only plays the EA Games and MGM films and the attract film before it loads the game: with Skip Intros
// the launcher goes straight to the game's executable. A relaunch into the launcher (the game quitting to it) is let be.
controls::GameState OnFootState();

// a pointer the game keeps that is safe to read through: its image and heap, or the 64 MB physical window
bool GuestPointer(uint32_t va) { return (va >= 0x10000 && va < 0x7F00000) || (va >= 0x80000000u && va < 0x84000000u); }
controls::GameState DrivingState();
controls::Action FireRedirect();
bool MountedGun();
void ReloadRequest();
bool Cutscene();
controls::Context PickedMission();

// default.xbe plays the intro films before the game itself starts: A on a controller, Enter, Space, Escape or a left
// click goes straight on to the game, as the Skip Intros setting does. The press counts as it goes down (a press
// already held when the films start does not count), the keys only while this window has the focus.
// Starting the game is a new process, which takes a few seconds to show its first frame while this one keeps its
// window up: so at the press the film goes at once. The sound is muted, a black window covers this one until the
// process ends (once the game's window is up), and the film's own loop is told to stop ([0x1F046C], what its input
// check sets), the films after it are not played and the launcher never starts the game a second time
// (src/launcher/hooks.cpp).
#ifdef _WIN32
void CoverWindowBlack() {
    HWND game = static_cast<HWND>(app::Window());
    RECT r{};
    if (!game || !GetWindowRect(game, &r)) return;
    HANDLE shown = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread([r, shown] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.hCursor = nullptr;
        wc.lpszClassName = L"AgentUnderFireIntroSkip";
        RegisterClassW(&wc);
        HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP,
                                 r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (w) {
            ShowWindow(w, SW_SHOWNOACTIVATE);
            UpdateWindow(w);
        }
        if (shown) SetEvent(shown);
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }).detach();
    if (shown) {
        WaitForSingleObject(shown, 250);
        CloseHandle(shown);
    }
}
#endif

void SkipIntrosOnPress() {
#ifdef _WIN32
    std::thread([] {
        bool armed = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (overlay::g_intros_done.load()) return;  // the launcher starts the game itself
            DWORD pid = 0;
            HWND fg = GetForegroundWindow();
            if (fg) GetWindowThreadProcessId(fg, &pid);
            bool focus = pid == GetCurrentProcessId();
            bool held = (input::HeldOnAnyController() & 0x1000) != 0;  // XINPUT_GAMEPAD_A
            if (focus)
                for (int key : {VK_RETURN, VK_SPACE, VK_ESCAPE, VK_LBUTTON}) held = held || (GetAsyncKeyState(key) & 0x8000);
            if (!held) {
                armed = true;
                continue;
            }
            if (!armed || overlay::g_intros_done.exchange(true)) continue;
            LOGR("Intros skipped");
            audio::g_master_volume.store(0);  // this process only: the game's own starts from the setting
            constexpr uint32_t kFilmStop = 0x1F046C;
            kernel::W32(kFilmStop, 1);
            CoverWindowBlack();
            kernel::Relaunch(kGameExe);
        }
    }).detach();
#endif
}

void Init() {
    overlay::ApplyVolume();
    if (std::string_view(IMAGE_XBE) == "bond_eng.xbe")
        overlay::g_mission = [] { return kernel::R32(0x2A88B8) ? kernel::R32(0x2A9F98) : ~0u; };  // in a mission only
    app::g_pack_folder_off = overlay::PackFolderOff;  // the Character / Weapon / Menu and HUD Textures settings
    if (std::string_view(IMAGE_XBE) == "bond_eng.xbe") {
        controls::SetGameStateQuery(OnFootState);
        controls::SetFireRedirect(FireRedirect);
        controls::SetMountedGunQuery(MountedGun);
        controls::SetReloadRequest(ReloadRequest);
        controls::SetCutsceneQuery(Cutscene);
        menus::SetMissionQuery(PickedMission);
    }
    if (std::string_view(IMAGE_XBE) == "driving.xbe") controls::SetGameStateQuery(DrivingState);
    bool launcher = std::string_view(IMAGE_XBE) == "default.xbe";
#ifdef _WIN32
    bool relaunched = std::wcsstr(GetCommandLineW(), L"--relaunched") != nullptr;
#else
    bool relaunched = false;
#endif
    if (launcher && overlay::g_skip_intros && !relaunched) kernel::Relaunch(kGameExe);
    if (launcher && !relaunched) SkipIntrosOnPress();
    if (std::string_view(IMAGE_XBE) == "driving.xbe") KeepDrivingLaunch(relaunched);
    if (std::string_view(IMAGE_XBE) == "bond_eng.xbe") LauncherLaunchData();
}

// The driving missions cannot be run where this port is built (their data is too big to copy), so the log gets where
// every guest thread is: after 5, 10, 20 and 40 seconds without a window, and whenever the game stops presenting
// frames for 5 seconds once it has one (again after 15 and 30 seconds of the same stall). A line every 10 seconds
// says how many frames it has shown.
void DumpGuestThreads(const char* why, int seconds) {
    LOGE("driving: %s for %d s, guest threads:", why, seconds);
    for (auto& t : kernel::Threads())
        if (t->ctx) {
            LOGE("thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32);
        }
    logx::Flush();
}

void Loaded() {
    if (std::string_view(IMAGE_XBE) == "bond_eng.xbe") {
        // the Loading Mission screen's line, in the image's read-only data (0x1D4F60, 41 bytes with its end): this
        // is no console
        constexpr uint32_t kTurnOff = 0x1D4F60;
        constexpr char kWas[] = "Please don't turn off your Xbox console.";
        constexpr char kNow[] = "Please don't turn off your system.";
        char* text = static_cast<char*>(kernel::HostPtr(kTurnOff));
        if (std::memcmp(text, kWas, sizeof(kWas)) == 0) std::memcpy(text, kNow, sizeof(kNow));
    }
    if (std::string_view(IMAGE_XBE) != "driving.xbe") return;
    LOGR("Driving: image loaded, starting");
    app::g_smooth_figures = true;  // the pause screen's running figure
    std::thread([] {
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        int window_dumps = 0;
        bool shown = false;
        uint64_t last = 0;
        auto last_change = clock::now();
        auto last_report = clock::now();
        int stall_dumps = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto now = clock::now();
            if (app::HandingOver()) {
                int waited = int(std::chrono::duration_cast<std::chrono::seconds>(now - start).count());
                static const int kAt[] = {5, 10, 20, 40};
                if (window_dumps < 4 && waited >= kAt[window_dumps]) DumpGuestThreads("no window", kAt[window_dumps++]);
                continue;
            }
            if (!shown) {
                shown = true;
                LOGR("Driving: window up");
            }
            uint64_t f = overlay::g_frames.load();
            if (f != last) {
                if (stall_dumps) LOGR("Driving: frames again after the stall (%llu)", static_cast<unsigned long long>(f));
                last = f;
                last_change = now;
                stall_dumps = 0;
            }
            int stalled = int(std::chrono::duration_cast<std::chrono::seconds>(now - last_change).count());
            static const int kStall[] = {5, 15, 30};
            if (stall_dumps < 3 && stalled >= kStall[stall_dumps]) DumpGuestThreads("no new frame", kStall[stall_dumps++]);
            if (now - last_report >= std::chrono::seconds(10)) {
                last_report = now;
                LOGR("Driving: %llu frames", static_cast<unsigned long long>(f));
                logx::Flush();
            }
        }
    }).detach();
}


// the controls first: they see every key, and take only one being bound (see controls.h); then the settings panel
// and the menus
// Mouse look on foot, straight into the game's view angles (bond_eng.xbe keeps them like Quake III's client: float
// degrees it packs into each tick's command, the movement code clamping pitch at about 85 degrees). Written when the
// game polls the pad, just before it builds the frame's command, and only while a level is being played.
void MouseLook(int port) {
    static const bool kOnFoot = std::string_view(IMAGE_XBE) == "bond_eng.xbe";
    if (!kOnFoot || port != 0) return;
    float yaw = 0, pitch = 0;
    bool direct = controls::TakeMouseLook(yaw, pitch);
    constexpr uint32_t kPitch = 0x2A8124, kYaw = 0x2A8128;              // the local player's view: + is down, + is left
    constexpr uint32_t kYawScale = 0x2A8144, kPitchScale = 0x2A8148;    // the game's look scale: aiming, zoom, 0 when it may not
    constexpr uint32_t kPlayerState = 0x2A88B8, kPaused = 0x3B2428, kCommandsOff = 0x3B2418;
    constexpr uint32_t kFlags = 0x0C, kFrozen = 0x40;
    if (!direct || (yaw == 0 && pitch == 0)) return;
    uint32_t ps = kernel::R32(kPlayerState);
    if (!ps || kernel::R32(kPaused) || kernel::R32(kCommandsOff)) return;
    auto f = [](uint32_t a) { return static_cast<float*>(kernel::HostPtr(a)); };
    float ys = *f(kYawScale), pscale = *f(kPitchScale);
    if (!(ys > 0) || (kernel::R32(ps + kFlags) & kFrozen)) return;
    *f(kYaw) -= yaw * ys;
    *f(kPitch) -= pitch * (pscale > 0 ? pscale : ys);
}

// Where the on-foot player is (controls::GameState): a level loaded (its playerState array set) and the pause menu
// shut is play; the pause menu open is Paused; no level is the front end.
controls::GameState OnFootState() {
    constexpr uint32_t kPlayerState = 0x2A88B8, kPaused = 0x3B2428;
    if (!kernel::R32(kPlayerState)) return controls::GameState::Menu;
    return kernel::R32(kPaused) ? controls::GameState::Paused : controls::GameState::Playing;
}

// driving.xbe: the mission object's state (+0x404: 1 playing, 2 paused, 3 complete, 4 failed) and its kind of
// controls (+0x408: 1 for the on-rails gunner's), and the pause menu's flag (3 while it is open)
controls::GameState DrivingState() {
    constexpr uint32_t kMission = 0x1C2C2C, kMenu = 0x1BEB1C;
    uint32_t m = kernel::R32(kMission);
    if (!GuestPointer(m)) m = 0;
    if (m) controls::SetDrivingMode(kernel::G<uint8_t>(m + 0x408) ? controls::Context::OnRails : controls::Context::Driving);
    if (kernel::R32(kMenu) == 3) return controls::GameState::Paused;
    return m && kernel::R32(m + 0x404) == 1 ? controls::GameState::Playing : controls::GameState::Menu;
}

// On foot, player 0 on a mounted gun (the FixedGun class: Fire & Water's AA gun, the 50 cal guns of Night of the
// Jackal and Fire & Water, Mediterranean Crisis's AA guns): taking it (0x73690, the only writer) sets the gun at
// player+0x2F0 and the player's gentity flag 0x100 ([0x2A88A8] + 8 for player 0), and leaving it clears both. The
// flag is what the game's own stick code tests (0x24490): set, it puts the view angles back and aims the gun with
// the left stick.
bool MountedGun() {
    if (!kernel::R32(0x2A88B8)) return false;
    uint32_t gent = kernel::R32(0x2A88A8);
    return GuestPointer(gent) && (kernel::R32(gent + 8) & 0x100) != 0;
}

// The mission picked in the front end ([0x2A9F98], 0 to 11): Precious Cargo and Forbidden Depths are on-rails
// gunner runs, Dangerous Pursuit and Streets of Bucharest driving, the rest on foot
controls::Context PickedMission() {
    switch (kernel::R32(0x2A9F98)) {
        case 1: case 8: return controls::Context::OnRails;
        case 2: case 6: return controls::Context::Driving;
        default: return controls::Context::OnFoot;
    }
}

// On foot, the item in player 0's hands ([[0x329390]+0x278] is the player, +0x2EC what it holds): a gadget (its
// byte +0x395 set, the flag the game's own fire code reads) and no mounted gun (+0x2F0) is used with B, where FIRE
// would only put it away. The Q-Camera and the Q-Jet are not held: picked, they leave the hands empty (+0x2EC 0)
// and sit at +0x84C, the gadget B uses first (0xB4C00); FIRE with empty hands brings the weapon back (0xBBA9F)
// instead, so empty hands with one of those out are B as well.
controls::Action FireRedirect() {
    if (!kernel::R32(0x2A88B8)) return controls::Action::RT;
    uint32_t players = kernel::R32(0x329390);
    uint32_t player = GuestPointer(players) ? kernel::R32(players + 0x278) : 0;
    if (!GuestPointer(player) || kernel::R32(player + 0x2F0)) return controls::Action::RT;
    uint32_t held = kernel::R32(player + 0x2EC);
    if (!held) return GuestPointer(kernel::R32(player + 0x84C)) ? controls::Action::B : controls::Action::RT;
    if (!GuestPointer(held)) return controls::Action::RT;
    return kernel::G<uint8_t>(held + 0x395) ? controls::Action::B : controls::Action::RT;
}

// Reload on its own: the game checks [0x333524] every frame (0xBBD7C) and reloads the weapon in hand once it is
// ready, clearing it; nothing of its own ever sets it. It is let go of after half a second if still set, so a
// request made just before the weapon was put away does not reload later.
std::atomic<int64_t> g_reload_at{0};
constexpr uint32_t kReloadRequest = 0x333524;

int64_t SteadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void ReloadRequest() {
    if (!kernel::R32(0x2A88B8)) return;
    kernel::W32(kReloadRequest, 1);
    g_reload_at.store(SteadyMs());
}

void ReloadExpiry() {
    int64_t at = g_reload_at.load();
    if (at && SteadyMs() - at > 500) {
        if (kernel::R32(0x2A88B8)) kernel::W32(kReloadRequest, 0);
        g_reload_at.store(0);
    }
}

// the controller does not shake while the keyboard and mouse play
void GameRumble(int, uint16_t& left, uint16_t& right) {
    if (controls::ActiveDevice() == controls::Device::KeyboardMouse) left = right = 0;
}

// a cutscene playing on foot: a level loaded, not paused, the game's cinematic flag ([0x329480 + 0x180], what both
// of its skip paths read)
bool Cutscene() {
    return kernel::R32(0x2A88B8) != 0 && kernel::R32(0x3B2428) == 0 && kernel::R32(0x329600) != 0;
}

void GamePad(int port, input::Pad& pad) {
    if (port == 0 && std::string_view(IMAGE_XBE) == "bond_eng.xbe") {
        ReloadExpiry();
        // the game's controller setup 1 ([0x205288], its binding tables at 0x20503C): the left stick moves forward,
        // back and sideways, the right stick turns and looks; buttons as setup 0. The port's remapping builds on it.
        constexpr uint32_t kSetup = 0x205288;
        if (kernel::R32(kSetup) != 1) kernel::W32(kSetup, 1);
    }
    overlay::Pad(port, pad);
    MouseLook(port);
}

bool WindowMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    return controls::Message(hwnd, msg, wp, lp, result) || overlay::Message(hwnd, msg, wp, lp, result);
}

void WindowUp(void* hwnd) {
    menus::Window(hwnd);
    controls::Window(hwnd);
}

// Two of the front end's 2D pictures, put right as they are drawn (bond_eng.xbe's menus: two triangles of float4
// screen positions and float4 or float2 texture coordinates under an identity projection).
// - The title logo is two 256 wide halves whose quads overlap by a pixel (x 91-322 and 321-552) at 80% opacity: the
//   column under both was blended twice, a light line 4 to 5 pixels wide once upscaled, and the artwork is continuous
//   across the halves, so the right half sat a pixel left of it. A quad that starts a pixel inside the one drawn just
//   before it, over the same rows, moves a pixel right.
// - The menu background's right column of tiles (x 513-641 from row 137) sits a pixel right of the tile above it
//   (x 512-640), leaving a column of the backdrop showing down the bottom right. It moves a pixel left.
// - Beside Difficulty the menu draws an empty part of the button sheet, whose only colour is the edge of the A
//   button's glow: two faint vertical marks. That draw is dropped.
// - The corner logo shows columns 0-168 and rows 113-147 of the button sheet, short of the artwork's 172 columns and
//   149 rows: the end of "Fire" and the shadow under it were cut. That quad shows all of it, grown in proportion.
struct Quad {
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bool valid = false;
};
Quad g_last_quad;

void Draw(const void* state, uint32_t prim, uint8_t* v, uint32_t stride, uint32_t count) {
    static const bool kMenus = std::string_view(IMAGE_XBE) == "bond_eng.xbe";
    menus::Draw(state, prim, v, stride, count);  // the front end's menus, and driving.xbe's pause menu
    if (!kMenus) return;
    const auto& s = *static_cast<const gpu::State*>(state);
    constexpr uint32_t kTriangleList = 5, kFloat4 = 0x42, kFloat2 = 0x22;
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool quad = prim == kTriangleList && count == 6 && s.vs && s.vs->program.empty() &&
                std::memcmp(s.transforms[3].data(), kIdentity, sizeof(kIdentity)) == 0;
    const auto& pos = s.vs ? s.vs->inputs[0] : gpu::VertexShader::Input{};
    const auto& tc = s.vs ? s.vs->inputs[9] : gpu::VertexShader::Input{};
    if (!quad || pos.stream != 0 || pos.type != kFloat4 || tc.stream != 0 || (tc.type != kFloat4 && tc.type != kFloat2) ||
        pos.offset + 8 > stride || tc.offset + 8 > stride) {
        g_last_quad.valid = false;
        return;
    }
    auto at = [&](uint32_t i, uint32_t off) { return reinterpret_cast<float*>(v + i * stride + off); };
    Quad q;
    float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
    q.x0 = q.y0 = 1e9f;
    q.x1 = q.y1 = -1e9f;
    for (uint32_t i = 0; i < 6; i++) {
        float* p = at(i, pos.offset);
        float* t = at(i, tc.offset);
        q.x0 = std::min(q.x0, p[0]), q.x1 = std::max(q.x1, p[0]), q.y0 = std::min(q.y0, p[1]), q.y1 = std::max(q.y1, p[1]);
        u0 = std::min(u0, t[0]), u1 = std::max(u1, t[0]), v0 = std::min(v0, t[1]), v1 = std::max(v1, t[1]);
    }
    q.valid = q.x1 > q.x0 && q.y1 > q.y0;
    auto same = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    const Quad last = g_last_quad;
    g_last_quad = q;
    if (!q.valid) return;

    // the right half of a picture drawn a pixel too far left: its artwork goes on from the left half's last column
    if (last.valid && same(q.x0, last.x1 - 1) && same(q.y0, last.y0) && same(q.y1, last.y1) && u1 > u0) {
        for (uint32_t i = 0; i < 6; i++) at(i, pos.offset)[0] += 1;
        g_last_quad.x0 += 1;
        g_last_quad.x1 += 1;
        return;
    }

    // the background's right column, drawn a pixel right of the tile above it (x 513 to 641 under 512 to 640)
    if (same(q.x0, 513) && same(q.x1, 641) && q.y0 >= 136.5f && same(u0, 0) && same(u1, 1)) {
        for (uint32_t i = 0; i < 6; i++) at(i, pos.offset)[0] -= 1;
        g_last_quad.x0 -= 1;
        g_last_quad.x1 -= 1;
        return;
    }

    gpu::TextureDesc d;
    bool sheet = s.textures[0] && gpu::Describe(s.textures[0], d) && d.width == 256 && d.height == 256;

    // beside Difficulty, an empty corner of the button sheet whose one coloured column is the A button's glow: a mark
    if (sheet && same(u0, 46 / 256.0f) && same(u1, 56 / 256.0f) && same(v0, 91 / 256.0f) && same(v1, 107 / 256.0f)) {
        for (uint32_t i = 0; i < 6; i++) at(i, pos.offset)[0] = q.x0;  // no area, nothing drawn
        return;
    }

    // the corner logo
    if (sheet && same(u0, 0) && same(u1, 168 / 256.0f) && same(v0, 113 / 256.0f) && same(v1, 147 / 256.0f)) {
        constexpr float kU1 = 172 / 256.0f, kV1 = 149 / 256.0f;
        float x1 = q.x0 + (q.x1 - q.x0) * (172.0f / 168.0f), y1 = q.y0 + (q.y1 - q.y0) * (149.0f - 113) / (147.0f - 113);
        for (uint32_t i = 0; i < 6; i++) {
            float* p = at(i, pos.offset);
            float* t = at(i, tc.offset);
            if (same(p[0], q.x1)) p[0] = x1;
            if (same(p[1], q.y1)) p[1] = y1;
            if (same(t[0], u1)) t[0] = kU1;
            if (same(t[1], v1)) t[1] = kV1;
        }
    }
}

// the settings file: the port's controls, and the game's OPTIONS remembered
void LoadSettings(const void* table) {
    controls::Load(table);
    game_options::Load(table);
    fov::Load(table);
}

void SaveSettings(void* table) {
    controls::Save(table);
    game_options::Save(table);
    fov::Save(table);
}

}  // namespace

const app::GameInfo app::kGame = {
    .title = L"007: Agent Under Fire",
    .short_title = L"Agent Under Fire",
    .name = L"AgentUnderFire",
    .log = "aufire",
    .settings = "settings.toml",
    .splash = L"splash",
    .missing = L"Put your disc image of the game (PAL, .iso) in the ROM folder beside AgentUnderFire.exe, then start "
               L"AgentUnderFire.exe again.",
    .images = {{"default.xbe", "AgentUnderFire"}, {"bond_eng.xbe", "AgentUnderFireGame"},
               {"driving.xbe", "AgentUnderFireDriving"}},
    .options = {{"skip_intros", &overlay::g_skip_intros, nullptr}, {"fps_counter", &overlay::g_fps_counter, nullptr},
                {"master_volume", nullptr, &overlay::g_master_volume}, {"modern_sticks", &sticks::g_modern, nullptr},
                {"stick_deadzone", nullptr, &sticks::g_deadzone},
                {"menus_4x3", &app::g_menus_4x3, nullptr}, {"hd_character_textures", &overlay::g_hd_characters, nullptr},
                {"hd_weapon_textures", &overlay::g_hd_weapons, nullptr}, {"hd_menu_textures", &overlay::g_hd_menus, nullptr},
                {"autosave", &autosave::g_enabled, nullptr},
                {"damage_effects", nullptr, &damage_fx::g_level},
                {"damage_flash_black", &damage_fx::g_flash_black, nullptr}},
    .load = LoadSettings,
    .save = SaveSettings,
    .init = Init,
    .loaded = Loaded,
    .window = WindowUp,
    .message = WindowMessage,
    .ui = overlay::Draw,
    .pad = GamePad,
    .rumble = GameRumble,
    .draw = Draw,
    .after_draw = menus::AfterDraw,
};
