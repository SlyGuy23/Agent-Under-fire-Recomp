# 007: Agent Under Fire - XBE Recomp

An unofficial native PC port of the original Xbox version of 007: Agent Under Fire (PAL), with extended graphics options and other enhancements.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation

1. Unzip the release into a folder of its own.
2. Put your disc image of the game (PAL, `.iso`) in the `ROM` folder.
3. Run `AgentUnderFire.exe`.

That's all. Keep `AgentUnderFireGame.exe` and `AgentUnderFireDriving.exe` beside it: the game switches to them for the missions and the driving levels on its own. Settings are kept in `settings.toml`, created on first start. Upscaled texture packs go in `textures\replacement` (each pack in a folder of its own).

## Features

### Settings panel
Press **F10**, or click **both sticks** on a controller, to open the settings panel. Left and Right change a setting; B, Escape or F10 closes it.
- **Display** - Display mode, resolution (Fullscreen and Windowed; Borderless always uses the desktop), max frame rate, vertical sync and an FPS counter.
- **Graphics** - Anisotropic filtering and anti-aliasing (FXAA, 2x or 4x supersampling).
- **General** - Skip Intros starts straight at the game; master volume.
- **Controls** - Modern Stick Response gives both sticks a round deadzone that keeps the stick's exact angle, in place of the game's per axis deadzone that snaps movement to eight directions; Stick Deadzone sets its size. The driving missions get the same, steering from the first movement past it.
- **Textures** - Upscaled texture replacement, with the character, weapon and menu/HUD parts each on or off, and texture dumping.

### Display


### Graphics

### Controller
Supports Xbox, PlayStation and Switch controllers, with hot plugging and rumble.

### General

### Miscellaneous
- **Frame Capture** - F3 saves the next frame, every draw and texture, into a `captures` folder beside the exe, for bug reports (`final_full.bmp` at the resolution it was drawn).
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
Place your extracted game data files in the `assets` folder. `bond_eng.xbe` must be there, with the game's folders beside it.

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

## Status

The intro films play and hand over to the game; the menus and on-foot missions are playable with a controller. The driving missions (`DRIVING.XBE`) are wired up but not yet tested. The previous sp00nznet xboxrecomp attempt is kept on the `xboxrecomp-legacy` branch.
