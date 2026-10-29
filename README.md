# 007 Agent Under Fire – Windows recomp

Static recompilation of the original Xbox release (PAL, title ID `4541000D`) using
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp). **Keep this repo private.** It
contains the game's executables.

## How it builds
Every push runs **Actions → Build** on GitHub. For each executable on the disc it:
1. decompiles the XBE in `xbe/` to C (xboxrecomp pipeline),
2. compiles it (MSVC for bond/launcher, clang-cl for driving),
3. uploads the `.exe` plus a `doctor_*.json` bring-up report as a downloadable artifact.

| Project | XBE | What it is | Entry point |
|---|---|---|---|
| `bond/` | bond_eng.xbe | the main game (English) | 0x0002A954 |
| `driving/` | DRIVING.XBE | driving missions (built with clang-cl; MSVC hangs on it) | 0x000E41EF |
| `launcher/` | default.xbe | intro movies, then hands off to the game | 0x00024462 |

## Running a build
1. Open the latest run under **Actions**, download `aufire_bond` (or the others).
2. Put the `.exe` in a folder that has the unpacked disc in a `game\` subfolder
   (on Luke's PC: `D:\RECOMPS\OWN ATTEMPTS\Agent Under Fire Recomp\`).
3. Double-click it. The first runs are expected to crash during bring-up.

## Working on it
Hand-written fixes go in `<project>/src/main.c` and `<project>/src/recomp_manual.c`.
Generated C is never committed, because CI regenerates it from `xbe/`.

## Boot fixes (main game)

What it took to get `bond_eng.xbe` from a silent exit to its title screen:

1. **Disassemble the XDK library sections** (`--extra-sections D3D,D3DX,XGRPH,DSOUND,XPP,DOLBY`).
   With `--text-only` alone Direct3D/DirectSound were never scanned and every call into them hit an empty stub.
2. **D3D completion fence**: `xbox_Nv2aMirrorFence(0x1A32A0, 0x30, 0x34)` in `main.c`. `D3D_BlockOnTime`
   (`0x197CE0`) waits for `*(device+0x34)` (GPU "completed") to reach `device+0x30` ("submitted").
3. **`RECOMP_TITLE_KEVENTS=1` + `RECOMP_VBLANK=paced`**: D3D waits on a KEVENT it builds itself
   (`0x1A2BF0`), set by the vblank interrupt.
4. **`AUF_NOSOUND`** (default on): DirectSound needs the emulated APU/DSP. 18 game functions that call
   DSOUND are skipped (`config/bond_patches.json`, applied by `tools/patch_gen.py`). Set `AUF_SOUND=1` to try audio.
5. **`RECOMP_PB_EXEC=1` + `RECOMP_FB_WINDOW=1`**: execute the GPU pushbuffer and show the frames in a window.

`bond/src/main_posix.c` is a Linux/macOS entry point used for headless test runs.
