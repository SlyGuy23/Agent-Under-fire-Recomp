// The settings panel on F10, or both sticks clicked on a controller
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace input {
struct Pad;
}

namespace overlay {

// settings of this port beyond the SDK's, kept in the settings file through GameInfo::options
inline bool g_skip_intros = false;
inline bool g_fps_counter = false;
inline uint32_t g_master_volume = 100;  // percent
// the character pack, textures\replacement\AUF Characters 4x: drawn while on, left out of the pack while off
inline bool g_hd_characters = true;
inline constexpr const wchar_t* kCharacterPack = L"AUF Characters 4x";
bool PackFolderOff(const std::wstring& folder);  // app::g_pack_folder_off

inline std::atomic<uint64_t> g_frames{0};  // frames the game presented, counted in Draw

bool Open();
void ApplyVolume();  // g_master_volume to the audio output
void Toggle();
void Draw(double fps);  // GameInfo::ui
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);  // GameInfo::message
void Pad(int port, input::Pad& pad);  // GameInfo::pad: the chord, and no input to the game while open

// the panel's settings as rows, for the game's own PC Settings page (menus.cpp); empty without a window
struct SettingRow {
    std::string section, label, value;
    bool enabled;
    std::string desc;  // a line saying what it does, shown under the row while it is selected (may be empty)
};
std::vector<SettingRow> SettingRows();
void StepSetting(int index, int step);  // index into SettingRows, step -1 or 1
void ResetSettings();                   // the panel's Defaults

}  // namespace overlay
