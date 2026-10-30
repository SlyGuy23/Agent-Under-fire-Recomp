#include <cstring>
#include <cwchar>
#include <filesystem>
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
#include "common/overlay.h"
#include "common/sticks.h"
#include "image_symbols.h"
#include "core/log.h"
#include "kernel/kernel.h"

#include <chrono>
#include <thread>

namespace {

constexpr const char* kGameExe = "AgentUnderFireGame";

// A driving mission is a separate executable the game relaunches into, with the mission in its launch data
// (hdd\launch.bin, read once at boot). That data is kept as hdd\driving_launch.bin, so starting
// AgentUnderFireDriving.exe on its own plays the last driving mission again without the on-foot mission before it.
void KeepDrivingLaunch(bool relaunched) {
    namespace fs = std::filesystem;
    fs::path hdd = app::ExeDir() / "hdd", launch = hdd / "launch.bin", kept = hdd / "driving_launch.bin";
    std::error_code ec;
    if (relaunched) {
        if (fs::exists(launch, ec)) fs::copy_file(launch, kept, fs::copy_options::overwrite_existing, ec);
        LOGR("Driving: mission launch data kept for a direct restart%s", ec ? " (not written)" : "");
    } else if (!fs::exists(launch, ec) && fs::exists(kept, ec)) {
        fs::copy_file(kept, launch, fs::copy_options::overwrite_existing, ec);
        LOGR("Driving: started on its own, playing the last driving mission again%s", ec ? " (not read)" : "");
    } else if (!fs::exists(kept, ec)) {
        LOGR("Driving: started on its own with no driving mission played yet");
    }
}

// default.xbe only plays the EA Games and MGM films and the attract film before it loads the game: with Skip Intros
// the launcher goes straight to the game's executable. A relaunch into the launcher (the game quitting to it) is let be.
void Init() {
    overlay::ApplyVolume();
    bool launcher = std::string_view(IMAGE_XBE) == "default.xbe";
#ifdef _WIN32
    bool relaunched = std::wcsstr(GetCommandLineW(), L"--relaunched") != nullptr;
#else
    bool relaunched = false;
#endif
    if (launcher && overlay::g_skip_intros && !relaunched) kernel::Relaunch(kGameExe);
    if (std::string_view(IMAGE_XBE) == "driving.xbe") KeepDrivingLaunch(relaunched);
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
    if (std::string_view(IMAGE_XBE) != "driving.xbe") return;
    LOGR("Driving: image loaded, starting");
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

}  // namespace

const app::GameInfo app::kGame = {
    .title = L"007: Agent Under Fire",
    .short_title = L"Agent Under Fire",
    .name = L"AgentUnderFire",
    .log = "aufire",
    .settings = "settings.toml",
    .splash = L"splash",
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe, bond_eng.xbe and DRIVING.XBE "
               L"with the game's folders beside them.",
    .images = {{"default.xbe", "AgentUnderFire"}, {"bond_eng.xbe", "AgentUnderFireGame"},
               {"driving.xbe", "AgentUnderFireDriving"}},
    .options = {{"skip_intros", &overlay::g_skip_intros, nullptr}, {"fps_counter", &overlay::g_fps_counter, nullptr},
                {"master_volume", nullptr, &overlay::g_master_volume}, {"modern_sticks", &sticks::g_modern, nullptr},
                {"stick_deadzone", nullptr, &sticks::g_deadzone}},
    .load = nullptr,
    .save = nullptr,
    .init = Init,
    .loaded = Loaded,
    .window = nullptr,
    .message = overlay::Message,
    .ui = overlay::Draw,
    .pad = overlay::Pad,
    .rumble = nullptr,
};
