#pragma once
#include <atomic>
#include <cstdint>
#include <functional>

namespace audio {

constexpr uint32_t kRate = 48000;

// the output's gain, 0 to 1, applied after the game's mix (a game repo's volume setting)
inline std::atomic<float> g_master_volume{1.0f};

// the platform output pulls interleaved stereo float frames from render
void StartOutput(std::function<void(float* out, uint32_t frames)> render);

}  // namespace audio
