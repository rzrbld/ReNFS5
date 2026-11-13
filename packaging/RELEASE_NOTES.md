ReNFS5: Open Unleashed, for ARM64 PortMaster handhelds, on the **[Open-Unleashed](https://github.com/Second-Boot/Open-Unleashed)** engine by **Second-Boot** — thanks to them. Supply your own installed copy of the game.

**Multithreaded rendering and smooth collisions.** Measured on the RG40XX-H (H700), Farmland, 7 opponents, chase view, the game's default graphics settings:
- **Render thread on by default**: the GPU work of one frame is sent from its own thread while the next frame is built. Farmland 42 → 53 FPS, Alps 39 → 47 FPS; with **FRAME CAP 30**, all but a handful of frames last exactly two refreshes. (`NFS_RENDER_THREAD=0` in `nfs5/nfs5-env.sh` turns it off.)
- **No stutter at the first contact**: each car's crushable body parts are prepared while the race loads; before, the first crash with the opponents moved a few hundred parts at once (a 30–60 ms freeze). Parts a crash re-sends unchanged are skipped, and a frame's part updates go to the GPU in one copy.
- Includes the previous release: mesh streaming, shader cache, even frame pacing.

Install or upgrade:
1. Download `nfs5.zip` and its SHA-256 file.
2. From the ZIP, copy **`NFS5.sh`** and the **`nfs5`** folder into your **ports** directory, replacing existing files. Leave the other ZIP files (`port.json`, `README.md`, `gameinfo.xml`, `screenshot.png`) out of `ports`; they are PortMaster metadata. `saves/` and `configs/` are preserved.
3. On muOS, update `NFS5.sh` in `Roms/PORTS` if used; keep the payload in `ports/nfs5`.
4. For a new installation, put your installed game folder (or a ZIP of it) into `nfs5/source/` and launch **NFS5**. It needs `FEData/` and `GameData/`; `Porsche.exe` is used for the intro movies when present.

Requires AArch64 firmware with glibc 2.30+, SDL2, PortMaster and an OpenGL ES 3.0 driver that SDL2 can load. This is a PortMaster-compatible package, not a PortMaster catalogue submission. It contains no original game files, assets, saves or user settings.

License: the engine is **GPL-3.0-or-later** (source linked below); the ReNFS5 launcher and scripts are **MIT**. Third-party licenses are in `nfs5/licenses/`. The original game, its assets and trademarks belong to their owners (Electronic Arts, Porsche); this project is unofficial and not affiliated with them.
