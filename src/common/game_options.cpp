#include "common/game_options.h"

#include <mutex>

#include "app/app.h"

#if __has_include(<toml++/toml.hpp>)
#include <toml++/toml.hpp>
#define GAME_OPTIONS_TOML 1
#else
#define GAME_OPTIONS_TOML 0  // the headless harness: no settings file
#endif

namespace game_options {

const char* const kKeys[kCount] = {"difficulty", "controller_setup", "invert_pitch", "vibration", "hud_always_visible",
                                   "cross_hair", "gold_rewards", "music_volume", "sound_effects_volume"};

namespace {

std::mutex g_lock;
Values g_values;

}  // namespace

Values Get() {
    std::lock_guard lock(g_lock);
    return g_values;
}

void Set(const Values& v) {
    {
        std::lock_guard lock(g_lock);
        g_values = v;
    }
    app::Save();
}

// every executable keeps the table as it read it, so driving.xbe and the launcher write it back unchanged
void Load(const void* table) {
#if GAME_OPTIONS_TOML
    const toml::table& t = *static_cast<const toml::table*>(table);
    Values v;
    if (const toml::table* o = t["game_options"].as_table())
        for (int i = 0; i < kCount; i++) {
            const toml::node* n = o->get(kKeys[i]);
            if (!n) continue;
            if (n->is_boolean()) v.value[i] = n->as_boolean()->get() ? 1 : 0;
            else if (n->is_integer()) v.value[i] = int32_t(n->as_integer()->get());
            else continue;
            v.have[i] = true;
        }
    std::lock_guard lock(g_lock);
    g_values = v;
#else
    (void)table;
#endif
}

void Save(void* table) {
#if GAME_OPTIONS_TOML
    toml::table& t = *static_cast<toml::table*>(table);
    Values v = Get();
    toml::table o;
    for (int i = 0; i < kCount; i++) {
        if (!v.have[i]) continue;
        bool flag = i >= kInvertPitch && i <= kGoldRewards;
        if (flag) o.insert(kKeys[i], v.value[i] != 0);
        else o.insert(kKeys[i], int64_t(v.value[i]));
    }
    if (!o.empty()) t.insert("game_options", std::move(o));
#else
    (void)table;
#endif
}

}  // namespace game_options
