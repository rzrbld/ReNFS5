ReNFS5: Open Unleashed, for ARM64 PortMaster handhelds, on the **[Open-Unleashed](https://github.com/Second-Boot/Open-Unleashed)** engine by **Second-Boot** — thanks to them. Supply your own installed copy of the game.

**LAN multiplayer fixes** (tested between an RG40XX-H and a PC running the engine):
- **The host can start the race**: in the host's lobby the **RACE** button stayed disabled, so no LAN race could start from the menus. The engine let the menu-layer cover disable the bottom bar's buttons; in the original game it does not (the bar decides BACK and OPTIONS by the layers itself).
- **Each player's own picture**: the LAN loading screen and the race leader picture showed the game's default face for everyone. Each player now sends its profile's picture, built exactly as the original game writes it (`SaveData\pic16.fsh`, byte-identical).
- **Pointer over dialogs**: on dialog boxes (exit, disconnect, game name, …) the mouse pointer was drawn under the box; it is now drawn on top.
- Includes the previous releases: vibration off by default (launcher **VIBRATION**), render thread, smooth collisions, mesh streaming, shader cache and even frame pacing.

How to play LAN: **Multiplayer → LAN games**, the host **create...**, the others **join** and **ready**, the host **RACE**. Players must be on the same subnet (UDP broadcast on port 9442).

Install or upgrade:
1. Download `nfs5.zip` and its SHA-256 file.
2. From the ZIP, copy **`NFS5.sh`** and the **`nfs5`** folder into your **ports** directory, replacing existing files. Leave the other ZIP files (`port.json`, `README.md`, `gameinfo.xml`, `screenshot.png`) out of `ports`; they are PortMaster metadata. `saves/` and `configs/` are preserved.
3. On muOS, update `NFS5.sh` in `Roms/PORTS` if used; keep the payload in `ports/nfs5`.
4. For a new installation, put your installed game folder (or a ZIP of it) into `nfs5/source/` and launch **NFS5**. It needs `FEData/` and `GameData/`; `Porsche.exe` is used for the intro movies when present.

Requires AArch64 firmware with glibc 2.30+, SDL2, PortMaster and an OpenGL ES 3.0 driver that SDL2 can load. This is a PortMaster-compatible package, not a PortMaster catalogue submission. It contains no original game files, assets, saves or user settings.

License: the engine is **GPL-3.0-or-later** (source linked below); the ReNFS5 launcher and scripts are **MIT**. Third-party licenses are in `nfs5/licenses/`. The original game, its assets and trademarks belong to their owners (Electronic Arts, Porsche); this project is unofficial and not affiliated with them.
