# 007: Agent Under Fire - XBE Recomp

An unofficial native PC port of the original Xbox version of 007: Agent Under Fire (PAL), with extended graphics options and other enhancements.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation

1. Unzip the release into a folder of its own.
2. Put your disc image of the game (PAL, `.iso`) in the `ROM` folder.
3. Run `AgentUnderFire.exe`.

That's all. Everything else lives in the `Game Files` folder: the game itself, `settings.toml`, your saves (`hdd`), logs, frame captures and upscaled texture packs (`textures\replacement`, each pack in a folder of its own).

## Features

### PC Settings
Everything is in **PC Settings** on the main menu and the pause menu, and in the settings panel (**F10**, or click **both sticks** on a controller; Left and Right change a setting, B, Escape or F10 closes it).
- **Display** - Fullscreen, borderless or windowed, resolution, vertical sync, max frame rate and an FPS counter.
- **Graphics** - Anisotropic filtering, anti-aliasing (FXAA, 2x or 4x supersampling) and menus in 4:3.
- **General** - Skip Intros, master volume and field of view.
- **Textures** - Upscaled texture replacement, with the character, weapon and menu/HUD parts each on or off, and texture dumping (PNG or DDS) for making your own packs.
- **Accessibility** - Damage effects (the flash and view kick when hit) from full to off, and a dark or white flash.

### Controls
- **Keyboard and mouse** with mouse look, rebindable keys for on foot, driving and on rails missions, and a mouse cursor in the menus.
- **Controllers** - Xbox, PlayStation and Switch controllers, with hot plugging and rumble. Modern stick response (a round deadzone that keeps the stick's exact angle), sensitivity and inner and outer deadzones per stick, crouch and manual aim as hold or toggle.
- **Controls** pages in the pause menu, with the controller and keyboard layout of the mission being played.
- **On-screen prompts** follow the device in use, with keyboard keys in place of controller buttons.
- **Multiplayer** - Split screen with up to four players; a keyboard and mouse player can play alongside controller players.

### Upscaled textures
Texture packs go in `Game Files\textures\replacement`, each in a folder of its own. A folder whose name ends in `Only 4x` replaces textures only where the game says so (for example `AUF Evil Summit Only 4x`, used only in Evil Summit's cutscenes).

### Miscellaneous
- **Frame Capture** - F3 saves the next frame, every draw and texture, into `Game Files\captures`, for bug reports.
- **Boot Splash** - Place a `splash.png` (or `.bmp`) beside the exe to display a splash screen at launch.

## System Requirements

### Minimum
- **OS**
  - Windows 10 64-bit
- **GPU** with support for Direct3D 12

## Building

### 1. Clone the Repository
Clone this repository using [Git](https://git-scm.com/).

### 2. Add the Required Game Files
Copy `default.xbe`, `bond_eng.xbe` and `driving.xbe` from your own copy of the game into an `assets` folder in the repository (it is ignored by git).

### 3. Install Dependencies
Install Visual Studio 2022 with **Desktop development with C++**, **C++ Clang tools for Windows** and **C++ CMake tools for Windows** (`build.bat` offers to add the last two).

### 4. Build the Project
Run `build.bat`. The first build recompiles the game and takes a while. The exe is written to `out\build\release`.

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp), the recompiler and runtime this is built on.
- [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp) (MIT, TGP482 and Joshhhuaaa), whose F10 panel the settings panel follows.
- [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) and [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) for the XDK 4039 render state table and library symbols.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.

## Licence

The source code in this repository is under the [MIT licence](LICENSE). The SDK in `sdk` has its own licence, and the third party libraries are listed in `sdk/THIRD_PARTY.txt`.

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Electronic Arts, MGM, Danjaq or Eon Productions. 007, James Bond and Agent Under Fire are trademarks of their respective owners. No game code, executables or assets are included in this repository: building or playing needs your own copy of the game.
