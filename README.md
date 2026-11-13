# ReNFS5: Open Unleashed

ReNFS5 is an unofficial community ARM64 Linux port of **Need for Speed: Porsche Unleashed**
(2000) for PortMaster handhelds. The game runs on
**[Open-Unleashed](https://github.com/Second-Boot/Open-Unleashed)**, the open-source
reimplementation of the game by **Second-Boot**, with the ReNFS5 handheld layer on top:

- a native **OpenGL ES 3** renderer with a render thread (on the Allwinner H700 about 50 FPS in
  a full race at the game's default graphics settings, 60 in lighter scenes),
- an SDL2 platform layer: window, GLES / Vulkan, audio and controllers all through the
  firmware's SDL2 — the game links no GPU or audio library of its own,
- handheld controls through gptokeyb2 (pad pointer, d-pad menus, name entry by pad),
- LAN multiplayer between handhelds and PCs running the engine,
- a native launcher that imports and verifies your own game files and offers settings.

The engine with these changes is the [`engine/`](https://github.com/rzrbld/Open-Unleashed/tree/renfs5-h700)
submodule: the `renfs5-h700` branch of [rzrbld/Open-Unleashed](https://github.com/rzrbld/Open-Unleashed),
a fork of Second-Boot's repository.

## Quick start

1. Download `nfs5.zip` from the [Releases](https://github.com/rzrbld/ReNFS5/releases) page.
2. Copy **`NFS5.sh`** and the **`nfs5`** folder from it into your device's **ports** directory.
3. Put your own installed game folder, or a ZIP of it, into `nfs5/source/`. It must contain
   `FEData/` and `GameData/`; `Porsche.exe` is used for the intro movies when present. CD
   images (`.iso`, `.bin` + `.cue`, `.mdf` + `.mds`) work when they contain an installed game.
4. Launch **NFS5**. The launcher imports the files once (allow about 1.5 GiB free), then opens
   its settings: frame cap, FPS counter, vibration (off by default), graphics (GLES, or Vulkan
   where the firmware has a Vulkan driver), movies, game screen. Select **SAVE & LAUNCH**.

For the smoothest races at high graphics settings set **FRAME CAP** to **30**: every frame then
lasts exactly two screen refreshes. Uncapped is faster on average but less even.

On muOS, copy `NFS5.sh` into `Roms/PORTS` if that is where your menu looks for shortcuts.
Requires AArch64 firmware with glibc 2.30+, SDL2 and an OpenGL ES 3.0 driver SDL2 can load.
Tested on muOS (RG40XX-H, H700, Mali-G31).

Controls are in [`packaging/README.md`](packaging/README.md). In the game, **Start + Select** quits.

## LAN multiplayer

In the game, **Multiplayer → LAN games**: one player chooses **create...** and confirms the game
name, the others select the game in the list, **join** and press **ready**; the host then
presses **RACE**. Games are found by UDP broadcast on port **9442**, so every machine must be on
the same subnet with broadcasts allowed (some guest or client-isolated Wi-Fi networks block them).
Tested between an RG40XX-H and a Linux PC running the engine.

## Saves and settings

Saves are in `nfs5/saves/`, settings in `nfs5/configs/`; keep both when updating. Keep
`nfs5/log.txt` and `nfs5/open-unleashed.log` when reporting a problem.

## Build

Every push is built by GitHub Actions ([`.github/workflows/build.yml`](.github/workflows/build.yml));
a `portmaster-v*` tag publishes the package on the Releases page. Locally, on Linux with Docker,
Python 3, Rust (`cargo`, for the licence list), `zip` and `file`:

```sh
git clone --recurse-submodules https://github.com/rzrbld/ReNFS5.git
cd ReNFS5
bash scripts/ci_build.sh        # -> build/package-out/nfs5.zip
```

The engine and launcher are cross-built in Debian bullseye (`packaging/Dockerfile.portmaster-cross`).
No game files are needed to build. The earlier static-recompilation port (`runtime/`, `tools/`,
`third_party/pcrecomp/`, releases before November 13, 2026) stays in the tree for reference.

## License and acknowledgements

Huge thanks to **Second-Boot** and the Open-Unleashed contributors: the game you play here is their
engine. Thanks also to EA Canada / EA Seattle for the original game, and to PortMaster,
gptokeyb2, SDL and miniz.

The engine is GPL-3.0-or-later (its source: the `engine/` submodule). The ReNFS5 launcher and
scripts use the [MIT license](LICENSE); third-party components keep their own licenses (see
[THIRD_PARTY.md](THIRD_PARTY.md) and `nfs5/licenses/` in the package).

**Need for Speed: Porsche Unleashed**, its original code, artwork, audio and trademarks belong to
their respective rights holders (Electronic Arts, Porsche). This project is unofficial and is not
affiliated with or endorsed by them. No original game files are included: supply your own copy.
