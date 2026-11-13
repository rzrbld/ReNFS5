ReNFS5: Open Unleashed, for ARM64 PortMaster handhelds, on the **[Open-Unleashed](https://github.com/Second-Boot/Open-Unleashed)** engine by **Second-Boot** — thanks to them. Supply your own installed copy of the game.

- **Vibration off by default**: the pad's rumble in races is now a launcher setting, **VIBRATION** (OFF / ON), off unless you turn it on.
- Includes the previous releases: render thread on by default (Farmland 42 → 53 FPS, Alps 39 → 47 FPS on the RG40XX-H), no stutter at the first contact with opponents, mesh streaming, shader cache and even frame pacing (**FRAME CAP 30** gives perfectly even frames at high settings).

Install or upgrade:
1. Download `nfs5.zip` and its SHA-256 file.
2. From the ZIP, copy **`NFS5.sh`** and the **`nfs5`** folder into your **ports** directory, replacing existing files. Leave the other ZIP files (`port.json`, `README.md`, `gameinfo.xml`, `screenshot.png`) out of `ports`; they are PortMaster metadata. `saves/` and `configs/` are preserved.
3. On muOS, update `NFS5.sh` in `Roms/PORTS` if used; keep the payload in `ports/nfs5`.
4. For a new installation, put your installed game folder (or a ZIP of it) into `nfs5/source/` and launch **NFS5**. It needs `FEData/` and `GameData/`; `Porsche.exe` is used for the intro movies when present.

Requires AArch64 firmware with glibc 2.30+, SDL2, PortMaster and an OpenGL ES 3.0 driver that SDL2 can load. This is a PortMaster-compatible package, not a PortMaster catalogue submission. It contains no original game files, assets, saves or user settings.

License: the engine is **GPL-3.0-or-later** (source linked below); the ReNFS5 launcher and scripts are **MIT**. Third-party licenses are in `nfs5/licenses/`. The original game, its assets and trademarks belong to their owners (Electronic Arts, Porsche); this project is unofficial and not affiliated with them.
