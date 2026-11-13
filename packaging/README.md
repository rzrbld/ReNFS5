# ReNFS5

ReNFS5 is an unofficial community ARM64 Linux port of **Need for Speed: Porsche Unleashed**
(2000) for PortMaster handhelds. The game runs on the open-source
[Open-Unleashed](https://github.com/Second-Boot/Open-Unleashed) engine, a reimplementation of
the original, with the ReNFS5 handheld layer: a native OpenGL ES 3 renderer, SDL2 window, audio
and controller input, and a native launcher that imports and verifies your game files.

Everything goes through the firmware's SDL2 and PortMaster: graphics through the GLES (or
Vulkan) driver SDL2 loads, sound through SDL2's audio driver, controls through gptokeyb2. The
game links no GPU or audio library of its own.

## Quick start

1. Extract `nfs5.zip` into your device's **ports** directory.
2. Put your own installed game folder, or a ZIP of it, into `nfs5/source/`. It must contain
   `FEData/` and `GameData/` (the installed game also has `Porsche.exe`, whose tables the intro
   movies use, and `SaveData/`, whose players are carried over).
3. Launch **NFS5**. The launcher imports and verifies the files once, then opens its settings.
4. Choose your settings and select **SAVE & LAUNCH**.

On muOS, copy `NFS5.sh` into `Roms/PORTS` if that is where your menu looks for shortcuts.
Allow at least **1.5 GiB free** for the import. CD images (`.iso`, `.bin` + `.cue`,
`.mdf` + `.mds`) work when they contain an installed game tree.

Launcher settings: frame cap, FPS counter, **vibration** (pad rumble, off by default), **graphics** (GLES, or Vulkan where the firmware has a
Vulkan driver, e.g. Panfrost), movies, game screen (dual-screen handhelds).

Requires AArch64 firmware with glibc 2.30+, SDL2 2.0.12+ and an OpenGL ES 3.0 driver that SDL2
can load.

## Controls

gptokeyb2's virtual Xbox pad; the game reads it directly.

| | Menus | Race |
|---|---|---|
| Left stick | moves the pointer | steering |
| A | click (after the stick) / select (after the d-pad) | game action per the pad layout |
| B | back | |
| D-pad | moves between buttons | |
| Triggers | | throttle / brake |
| Start + Select | quit | quit |

Typing a name (Create Player and other text boxes): D-pad up / down changes the letter, right
adds a letter, left (or B) deletes one, L1 / R1 jump ten letters, **Start** finishes (on Create
Player it also presses OK). A still clicks with the pointer, so OK / Cancel and the other buttons
stay reachable. Any button skips the intro movies.

## Saves and settings

Saves are in `nfs5/saves/` (on first start, the players from your game's `SaveData` are copied
there); settings are in `nfs5/configs/`. Preserve both when updating. Keep `nfs5/log.txt` and
`nfs5/open-unleashed.log` when reporting a problem.

## Compile

Engine: the Open-Unleashed fork with the ReNFS5 handheld layer, built for `aarch64` in Debian
bullseye with `--no-default-features` (SDL2-only audio). Launcher: C, SDL2. See
`licenses/ENGINE-SOURCE.txt` for the engine's source.

## License and acknowledgements

Thanks to the **Open-Unleashed** developers (Second-Boot) for the engine, to EA Canada /
EA Seattle for Need for Speed: Porsche Unleashed, and to PortMaster, gptokeyb2, SDL and miniz.

The engine is GPL-3.0-or-later (`licenses/ENGINE-GPL-3.0.txt`, the third-party crates in
`licenses/THIRD-PARTY-LICENSES.txt`); the ReNFS5 launcher and scripts are MIT
(`licenses/PORT-MIT.txt`). The original game is not covered by these licenses: provide your own
game files. This project is unofficial and is not affiliated with or endorsed by Electronic Arts
or Porsche; their names and trademarks belong to their respective owners.
