#include <algorithm>
#include <cmath>
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
#include "gpu/state.h"
#include "gpu/texture.h"

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


// Two of the front end's 2D pictures, put right as they are drawn (bond_eng.xbe's menus: two triangles of float4
// screen positions and float4 or float2 texture coordinates under an identity projection).
// - The title logo is two 256 wide halves whose quads overlap by a pixel (x 91-322 and 321-552) at 80% opacity: the
//   shared column was blended twice, a light line 4 to 5 pixels wide once upscaled. A quad that starts a pixel inside
//   the one drawn just before it, over the same rows, starts at its end instead, its texture coordinate moved with it.
// - The corner logo shows columns 0-168 and rows 113-147 of the button sheet, short of the artwork's 172 columns and
//   149 rows: the end of "Fire" and the shadow under it were cut. That quad shows all of it, grown in proportion.
struct Quad {
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bool valid = false;
};
Quad g_last_quad;

void Draw(const void* state, uint32_t prim, uint8_t* v, uint32_t stride, uint32_t count) {
    static const bool kMenus = std::string_view(IMAGE_XBE) == "bond_eng.xbe";
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
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    const Quad last = g_last_quad;
    g_last_quad = q;
    if (!q.valid) return;

    // the halves' shared column
    if (last.valid && near(q.x0, last.x1 - 1) && near(q.y0, last.y0) && near(q.y1, last.y1) && u1 > u0) {
        float du = (u1 - u0) / (q.x1 - q.x0);
        for (uint32_t i = 0; i < 6; i++) {
            float* p = at(i, pos.offset);
            if (near(p[0], q.x0)) {
                p[0] += 1;
                at(i, tc.offset)[0] += du;
            }
        }
        g_last_quad.x0 += 1;
        return;
    }

    // the corner logo
    gpu::TextureDesc d;
    if (near(u0, 0) && near(u1, 168 / 256.0f) && near(v0, 113 / 256.0f) && near(v1, 147 / 256.0f) && s.textures[0] &&
        gpu::Describe(s.textures[0], d) && d.width == 256 && d.height == 256) {
        constexpr float kU1 = 172 / 256.0f, kV1 = 149 / 256.0f;
        float x1 = q.x0 + (q.x1 - q.x0) * (172.0f / 168.0f), y1 = q.y0 + (q.y1 - q.y0) * (149.0f - 113) / (147.0f - 113);
        for (uint32_t i = 0; i < 6; i++) {
            float* p = at(i, pos.offset);
            float* t = at(i, tc.offset);
            if (near(p[0], q.x1)) p[0] = x1;
            if (near(p[1], q.y1)) p[1] = y1;
            if (near(t[0], u1)) t[0] = kU1;
            if (near(t[1], v1)) t[1] = kV1;
        }
    }
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
                {"stick_deadzone", nullptr, &sticks::g_deadzone},
                {"menus_4x3", &app::g_menus_4x3, nullptr}},
    .load = nullptr,
    .save = nullptr,
    .init = Init,
    .loaded = Loaded,
    .window = nullptr,
    .message = overlay::Message,
    .ui = overlay::Draw,
    .pad = overlay::Pad,
    .rumble = nullptr,
    .draw = Draw,
};
