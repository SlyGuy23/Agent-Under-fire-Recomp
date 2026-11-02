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
// parts of the texture pack (folders under textures\replacement\), each drawn while on and left out while off:
// the characters (props they handle too: keycards, the business card), the weapons, the menus and HUD (with the
// driving HUD and the loading screens)
inline bool g_hd_characters = true;
inline bool g_hd_weapons = true;
inline bool g_hd_menus = true;
inline constexpr const wchar_t* kCharacterPack = L"AUF Characters 4x";
inline constexpr const wchar_t* kWeaponPack = L"AUF Weapons 4x";
inline constexpr const wchar_t* kMenuPacks[] = {L"AUF Menus and HUD 4x", L"AUF Driving HUD 4x", L"AUF Loading Screens 4x"};
// a pack folder named "AUF Evil Summit Only 4x" is drawn only in the cutscenes of Evil Summit's last part (mission 12,
// level alp2_3), and only with the character part on: the Harrier and Bloch's rocket launcher in the closing cutscene.
// While it is drawn its images replace the same textures of the other folders (the launcher's 4x one in the weapon
// part); the rest of the time it is left out and those stay as the other folders have them. The mission's first two
// parts (alp2_1, alp2_2, with the Harrier landing at the start) and every other mission do not use it
inline constexpr const wchar_t* kEvilSummitPack = L"AUF Evil Summit Only 4x";
inline constexpr const char* kEvilSummitLevel = "alp2_3";
inline bool (*g_level_is)(const char* name) = nullptr;  // the level being played is <name> (set by the game)
inline bool (*g_in_cutscene)() = nullptr;               // a cutscene is playing (set by the game)
bool PackFolderOff(const std::wstring& folder);        // app::g_pack_folder_off
bool PackFolderOverrides(const std::wstring& folder);  // app::g_pack_folder_overrides: names ending in "Only 4x"

inline std::atomic<uint64_t> g_frames{0};  // frames the game presented, counted in Draw
// default.xbe: the game is being started, the intro films skipped or over. Set once, by whichever starts it: a press
// that skips the films (game_info.cpp) or the launcher's own start of the game after them (src/launcher/hooks.cpp)
inline std::atomic<bool> g_intros_done{false};

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
    std::string desc;  // a line saying what its current value does, shown under the row while it is selected (may be empty)
};
std::vector<SettingRow> SettingRows();
void StepSetting(int index, int step);  // index into SettingRows, step -1 or 1
void ResetSettings();                   // the panel's Defaults
// the settings with these labels (as SettingRows gives them) back to their defaults, the others kept: the pause
// menu's PC Settings page
void ResetSettings(const std::vector<std::string>& labels);

// what the current value of a setting does, for the line under its row (PC Settings, the pause menu's OPTIONS):
// a line for the value chosen, not one listing them all
std::string FovDesc();
std::string DamageEffectsDesc();  // damage_fx::g_level
std::string DamageFlashDesc();    // damage_fx::g_flash_black (and whether there is a flash at all)

}  // namespace overlay
