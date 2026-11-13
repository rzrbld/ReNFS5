ReNFS5: Open Unleashed, for ARM64 PortMaster handhelds. Supply your own installed copy of the game.

**New engine.** ReNFS5 now runs on **[Open-Unleashed](https://github.com/Second-Boot/Open-Unleashed)**, the open-source reimplementation of Need for Speed: Porsche Unleashed by **Second-Boot** — huge thanks to them. The static recompilation of earlier releases is retired. On top of the engine, ReNFS5 adds the handheld layer:
- **Native OpenGL ES 3 renderer**: about 60 FPS in races on the RG40XX-H (H700, Mali-G31); heavy stretches dip into the 30s–50s. The engine's original wgpu renderer stays available as the **Vulkan** setting on firmwares with a Vulkan driver (e.g. Panfrost).
- **Everything through SDL2 and PortMaster**: graphics (GLES, and Vulkan via SDL2's loader), audio and controllers come from the firmware's SDL2; the game links no GPU or audio library. Controls through gptokeyb2.
- **Handheld controls**: left stick + A as the pointer, d-pad menus, name entry with the d-pad (Start finishes and confirms), Start + Select quits.
- **Easier game files**: any installed copy with `FEData/` and `GameData/` works (folder, ZIP or CD image); `Porsche.exe` is used for the intro movies when present.
- Launcher: renamed **Open Unleashed**, large FPS counter, graphics setting (GLES default / Vulkan).

Install or upgrade:
1. Download `nfs5.zip` and its SHA-256 file.
2. From the ZIP, copy **`NFS5.sh`** and the **`nfs5`** folder into your **ports** directory, replacing existing files. Leave the other ZIP files (`port.json`, `README.md`, `gameinfo.xml`, `screenshot.png`) out of `ports`; they are PortMaster metadata. `saves/` and `configs/` are preserved.
3. On muOS, update `NFS5.sh` in `Roms/PORTS` if used; keep the payload in `ports/nfs5`.
4. For a new installation, put your installed game folder (or a ZIP of it) into `nfs5/source/` and launch **NFS5**.

Requires AArch64 firmware with glibc 2.30+, SDL2, PortMaster and an OpenGL ES 3.0 driver that SDL2 can load. This is a PortMaster-compatible package, not a PortMaster catalogue submission. It contains no original game files, assets, saves or user settings.

License: the engine is **GPL-3.0-or-later** (source linked below); the ReNFS5 launcher and scripts are **MIT**. Third-party licenses are in `nfs5/licenses/`. The original game, its assets and trademarks belong to their owners (Electronic Arts, Porsche); this project is unofficial and not affiliated with them.
