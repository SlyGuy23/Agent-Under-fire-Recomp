#include "common/fov.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/app.h"

#if __has_include(<toml++/toml.hpp>)
#include <toml++/toml.hpp>
#define FOV_TOML 1
#else
#define FOV_TOML 0  // the headless harness: no settings file
#endif

namespace fov {

namespace {

constexpr float kDeg = 3.14159265f / 180.0f;

float Aspect() { return app::Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f; }

}  // namespace

float Horizontal(float vertical) { return 2 * std::atan(std::tan(vertical * 0.5f * kDeg) * Aspect()) / kDeg; }

float Scale() {
    int offset = g_offset.load(std::memory_order_relaxed);
    if (offset == 0) return 1;
    float base = Horizontal(kOnFootVertical), wide = std::clamp(base + float(offset), 1.0f, 170.0f);
    return std::tan(wide * 0.5f * kDeg) / std::tan(base * 0.5f * kDeg);
}

float Apply(float degrees) {
    float k = Scale();
    if (k == 1 || !(degrees > 0 && degrees < 179)) return degrees;
    return 2 * std::atan(std::tan(degrees * 0.5f * kDeg) * k) / kDeg;
}

float Unapply(float degrees) {
    float k = Scale();
    if (k == 1 || !(degrees > 0 && degrees < 179)) return degrees;
    return 2 * std::atan(std::tan(degrees * 0.5f * kDeg) / k) / kDeg;
}

std::string Label() {
    int offset = g_offset.load(std::memory_order_relaxed);
    int across = int(std::lround(Horizontal(kOnFootVertical))) + offset;
    char text[32];
    if (offset == 0) std::snprintf(text, sizeof(text), "DEFAULT (%d)", across);
    else std::snprintf(text, sizeof(text), "%+d (%d)", offset, across);
    return text;
}

void Step(int step) {
    int v = g_offset.load(std::memory_order_relaxed) + step * kStep;
    g_offset.store(std::clamp(v, kMin, kMax), std::memory_order_relaxed);
}

void Load(const void* table) {
#if FOV_TOML
    const toml::table& t = *static_cast<const toml::table*>(table);
    int64_t v = t["fov"].value_or(int64_t(0));
    v = std::clamp<int64_t>(v, kMin, kMax);
    g_offset.store(int(v / kStep * kStep), std::memory_order_relaxed);
#else
    (void)table;
#endif
}

void Save(void* table) {
#if FOV_TOML
    static_cast<toml::table*>(table)->insert("fov", int64_t(g_offset.load(std::memory_order_relaxed)));
#else
    (void)table;
#endif
}

}  // namespace fov
