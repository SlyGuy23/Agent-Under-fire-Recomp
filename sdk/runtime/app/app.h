#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };
// anti_aliasing: FXAA smooths the edges of the finished picture; supersampling draws it with twice or four times the
// pixels (on top of render_percent) and shrinks it to the window
enum class AntiAliasing : uint32_t { Off, Fxaa, Ssaa2x, Ssaa4x };
inline constexpr uint32_t kFpsCapRefresh = 1;  // fps_cap: the monitor's refresh rate

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the window's pixels the frame is shown on
    uint32_t fps_cap = 60;           // 0 unlimited, kFpsCapRefresh the monitor's refresh rate
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    AntiAliasing anti_aliasing = AntiAliasing::Off;
    bool custom_textures = true;     // images in textures\replacement\ beside the exe replace the game's
    bool dump_textures = false;      // each texture once into textures\dumps\ beside the exe
    uint32_t texture_dump_format = 0;  // 0 PNG, 1 DDS
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
};

Settings& Get();
void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
// host pixels per console pixel of the render targets: render_percent (and supersampling) of the window's pixels where
// the frame is shown; window 0 x 0 takes the output mode
float RenderScale(uint32_t guest_width, uint32_t guest_height, uint32_t window_width, uint32_t window_height);
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();
// a widescreen output shows frames drawn in 2D alone (front end menus, loading screens, films) at 4:3 between bars:
// their art is laid out for 640x480 and only the 3D scenes are drawn for 16:9. Set by the game (a setting there)
inline bool g_menus_4x3 = true;
// small figures drawn from 128x128 linear A8R8G8B8 textures are upscaled smooth with an outline (gpu::SmoothFigure)
inline bool g_smooth_figures = false;
// a folder right under textures\replacement\ (its name as given) the texture pack leaves out while this says so. Set by
// the game, which bumps g_pack_folders whenever an answer may have changed: the pack is listed again at the next
// present and every texture looks its image up again (gpu::texturedumper)
inline bool (*g_pack_folder_off)(const std::wstring& folder) = nullptr;
// a folder right under textures\replacement\ whose images replace the same keys of every other folder while it is on
// (g_pack_folder_off says no); while it is off it is only left out, and the keys it shares stay with the other folders
inline bool (*g_pack_folder_overrides)(const std::wstring& folder) = nullptr;
inline std::atomic<uint32_t> g_pack_folders{0};
// frames the game has swapped (D3DDevice_Swap), for hooks that look at a whole frame's draws
inline std::atomic<uint64_t> g_swaps{0};
// the frame being presented, as g_swaps counted while the game drew it (GameInfo::ui runs while it is presented, on
// the render thread a frame or so behind the game): for overlays that must show exactly on the frames whose draws
// asked for them
inline std::atomic<uint64_t> g_shown_frame{0};
// where the console's frame was last shown in the window, in window pixels: x, y, width, height (set by the renderer)
inline float g_frame_rect[4] = {0, 0, 0, 0};
// the size in pixels the renderer last drew the console's frame at, before it was scaled to the window (set by the
// renderer, 0 before its first frame)
inline uint32_t g_render_size[2] = {0, 0};

std::filesystem::path ExeDir();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
std::string Sha1(const std::vector<uint8_t>& data);  // lower case hex
// the recompiled code, <exe>.dll, built from the game in assets when missing or out of date (game_code.cpp) and
// loaded; false after telling the player why the game cannot start, or when they chose not to start it yet
bool PrepareGameCode(const GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    // an unindexed draw before it is recorded (state is the gpu::State it draws with): the game may adjust its vertices,
    // count of them stride apart in guest memory, which the title writes again for its next frame anyway
    void (*draw)(const void* state, uint32_t prim, uint8_t* vertices, uint32_t stride, uint32_t count);
    // after that draw is recorded: emit draws more vertices (count of them, stride apart, laid out as the draw's) with
    // the same state, for pieces the game adds to the title's picture (a menu entry of its own)
    using Emit = void (*)(uint32_t prim, const void* vertices, uint32_t count);
    void (*after_draw)(const void* state, uint32_t prim, const uint8_t* vertices, uint32_t stride, uint32_t count,
                       Emit emit);
    // the controller (input::Device's slot, -1 none) a console port reads and rumbles; left out, each port its own
    int (*pad_device)(int port);
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap
void Pace();

}  // namespace app
