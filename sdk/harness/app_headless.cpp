// the host side without a window: settings at their defaults, the run folder as the exe folder
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>
#include "harness.h"

#include "app/app.h"

namespace app {

Settings& Get() {
    static Settings s;
    return s;
}
void Load() {}
void Save() {}
void ApplyDisplay() {}
Mode Desktop() { return {1280, 720}; }
Mode Output() { return Desktop(); }
float RenderScale(uint32_t) { return 1.0f; }
bool Widescreen() { return true; }

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = std::filesystem::current_path();
    return dir;
}
int Instance() {
    static const int n = [] {
        const char* e = std::getenv("INSTANCE");
        return e ? std::atoi(e) : 0;
    }();
    return n;
}
void* Window() { return nullptr; }

bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

// PACE=fps: frames held to that rate, so game time runs with the audio clock as on Windows
void Pace() {
    static const long fps = harness::Env("PACE");
    if (fps <= 0) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    next += std::chrono::nanoseconds(1000000000LL / fps);
    auto now = clock::now();
    if (next < now - std::chrono::milliseconds(100)) next = now;
    std::this_thread::sleep_until(next);
}

}  // namespace app
