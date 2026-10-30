// The settings panel on F10, or both sticks clicked on a controller
#pragma once
#include <cstdint>

namespace input {
struct Pad;
}

namespace overlay {

// settings of this port beyond the SDK's, kept in the settings file through GameInfo::options
inline bool g_skip_intros = false;
inline bool g_fps_counter = false;
inline uint32_t g_master_volume = 100;  // percent

bool Open();
void ApplyVolume();  // g_master_volume to the audio output
void Toggle();
void Draw(double fps);  // GameInfo::ui
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);  // GameInfo::message
void Pad(int port, input::Pad& pad);  // GameInfo::pad: the chord, and no input to the game while open

}  // namespace overlay
