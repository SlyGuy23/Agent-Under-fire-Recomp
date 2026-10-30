#pragma once
#include <cstdint>
#include <vector>

namespace gpu {

// times the figure is upscaled in each direction
constexpr uint32_t kFigureScale = 4;

// A small animated figure the title draws from a linear A8R8G8B8 texture (Agent Under Fire's driving pause screen):
// kFigureScale times larger with Catmull-Rom filtering, its soft edge made crisp and smooth, and a light outline round
// it. out gets w * kFigureScale by h * kFigureScale BGRA texels, rows tightly packed.
void SmoothFigure(const uint8_t* src, uint32_t pitch, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

}  // namespace gpu
