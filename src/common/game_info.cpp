#include <cstring>
#include <cwchar>
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
}

// The driving missions cannot be run where this port is built (their data is too big to copy), so while a run shows
// no picture the log gets where every guest thread is, after 5, 10, 20 and 40 seconds.
void Loaded() {
    if (std::string_view(IMAGE_XBE) != "driving.xbe") return;
    LOGR("Driving: image loaded, starting");
    std::thread([] {
        int waited = 0;
        for (int at : {5, 10, 20, 40}) {
            std::this_thread::sleep_for(std::chrono::seconds(at - waited));
            waited = at;
            if (!app::HandingOver()) return;  // the window is up
            LOGE("driving: no frame after %d s, guest threads:", at);
            for (auto& t : kernel::Threads())
                if (t->ctx) {
                    LOGE("thread %X", t->id);
                    kernel::LogGuestStack(*t->ctx, 32);
                }
            logx::Flush();
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
