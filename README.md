# 007 Agent Under Fire – Windows recomp

Static recompilation of the original Xbox release (PAL, title ID `4541000D`) using
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp). **Keep this repo private.** It
contains the game's executables.

## How it builds
Every push runs **Actions → Build** on GitHub. For each executable on the disc it:
1. decompiles the XBE in `xbe/` to C (xboxrecomp pipeline),
2. compiles it with MSVC,
3. uploads the `.exe` plus a `doctor_*.json` bring-up report as a downloadable artifact.

| Project | XBE | What it is | Entry point |
|---|---|---|---|
| `bond/` | bond_eng.xbe | the main game (English) | 0x0002A954 |
| `driving/` | DRIVING.XBE | driving missions | 0x000E41EF |
| `launcher/` | default.xbe | intro movies, then hands off to the game | 0x00024462 |

## Running a build
1. Open the latest run under **Actions**, download `aufire_bond` (or the others).
2. Put the `.exe` in a folder that has the unpacked disc in a `game\` subfolder
   (on Luke's PC: `D:\RECOMPS\OWN ATTEMPTS\Agent Under Fire Recomp\`).
3. Double-click it. The first runs are expected to crash during bring-up.

## Working on it
Hand-written fixes go in `<project>/src/main.c` and `<project>/src/recomp_manual.c`.
Generated C is never committed, because CI regenerates it from `xbe/`.
