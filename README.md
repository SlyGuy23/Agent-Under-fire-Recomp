# 007: Agent Under Fire - XBE Recomp

An unofficial native PC port of the original Xbox version of 007: Agent Under Fire (PAL), with extended graphics options and other enhancements.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation

- Put your game files in the `assets` folder. This can be an ISO, or the extracted data files.
- Run `AgentUnderFire.exe`. It plays the intro films (`default.xbe`), then switches to `AgentUnderFireGame.exe` (`bond_eng.xbe`), which switches to `AgentUnderFireDriving.exe` (`DRIVING.XBE`) for the driving missions and back. Keep all three beside each other.
- The game is also found in a `game` folder beside the exe.
- Recomp settings can be adjusted in `settings.toml` beside the exe.

## Features

### Settings panel
Press **F10**, or click **both sticks** on a controller, to open the settings panel. Left and Right change a setting; B, Escape or F10 closes it.
- **Display** - Display mode, resolution (Fullscreen and Windowed; Borderless always uses the desktop), render resolution, max frame rate, vertical sync and an FPS counter.
- **Graphics** - Anisotropic filtering.
- **General** - Skip Intros starts straight at the game; master volume.
- **Textures** - Texture replacement and dumping.

### Display


### Graphics

### Controller
Supports Xbox, PlayStation and Switch controllers, with hot plugging and rumble.

### General

### Miscellaneous
- **Frame Capture** - F3 saves the next frame, every draw and texture, into a `captures` folder beside the exe, for bug reports.
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
