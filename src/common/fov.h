// FIELD OF VIEW (PC Settings): degrees added to the game's own view across the screen, on foot and in the driving
// and on-rails missions. Every field of view the game asks for (its normal one, a scope's or a zoom's) is widened by
// the same factor on the tangent, so a zoom keeps its magnification over the new view and the crosshair stays where
// the shots go (src/game/hooks.cpp, src/driving/hooks.cpp).
#pragma once
#include <atomic>
#include <string>

namespace fov {

constexpr int kMin = -10, kMax = 40, kStep = 5;
inline std::atomic<int> g_offset{0};  // settings file "fov": degrees on the on-foot view across the screen, 0 the game's

// on foot the game draws 60 degrees from top to bottom whatever the screen's shape (wider screens see more across)
constexpr float kOnFootVertical = 60.0f;

float Horizontal(float vertical);  // across the screen at the output's shape (16:9 or 4:3), degrees
float Scale();                     // tan(new / 2) / tan(game's / 2); 1 at the game's own view
float Apply(float degrees);        // a field of view the game asks for, widened by Scale()
float Unapply(float degrees);      // Apply undone: the game's own field from a widened one
std::string Label();               // the PC Settings value
void Step(int step);               // -1 or 1, within kMin to kMax

void Load(const void* table);  // GameInfo::load / save: a signed value, so not one of GameInfo::options
void Save(void* table);

}  // namespace fov
