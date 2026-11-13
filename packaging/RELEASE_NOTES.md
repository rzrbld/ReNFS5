ReNFS5: Open Unleashed, for ARM64 PortMaster handhelds, on the **[Open-Unleashed](https://github.com/Second-Boot/Open-Unleashed)** engine by **Second-Boot** — thanks to them. Supply your own installed copy of the game.

**Smoother races.** Measured on the RG40XX-H (H700) in a career-style race (Farmland, 7 opponents, chase view, the game's default graphics settings):
- **Mesh streaming**: the car parts the game updates every frame (about 95 at default car detail) no longer stall the Mali driver. Average 27.6 → 41.2 FPS; frames over 40 ms in 50 s: 250+ → 16.
- **Shader cache**: compiled shaders are kept in `nfs5/configs/shader-cache/` and loaded at start, so new effects no longer freeze the race for 60–170 ms when they first appear (only the first race after an update fills the cache).
- **Even frame pacing**: game time advances in whole screen refreshes, so motion matches what the screen shows. With the launcher's **FRAME CAP 30**, every frame lasts exactly two refreshes — recommended for the smoothest feel at high settings; uncapped is faster on average but uneven.
- Lens-flare checks no longer make the CPU wait for the GPU.

Also in this release (since the first Open Unleashed build): Vulkan goes through SDL2's loader as well; the RACE button is greyed out while disabled and the bottom-bar icons line up with their labels; a larger FPS counter; the name box never traps the pad (A clicks, Start confirms).

Install or upgrade:
1. Download `nfs5.zip` and its SHA-256 file.
2. From the ZIP, copy **`NFS5.sh`** and the **`nfs5`** folder into your **ports** directory, replacing existing files. Leave the other ZIP files (`port.json`, `README.md`, `gameinfo.xml`, `screenshot.png`) out of `ports`; they are PortMaster metadata. `saves/` and `configs/` are preserved.
3. On muOS, update `NFS5.sh` in `Roms/PORTS` if used; keep the payload in `ports/nfs5`.
4. For a new installation, put your installed game folder (or a ZIP of it) into `nfs5/source/` and launch **NFS5**. It needs `FEData/` and `GameData/`; `Porsche.exe` is used for the intro movies when present.

Requires AArch64 firmware with glibc 2.30+, SDL2, PortMaster and an OpenGL ES 3.0 driver that SDL2 can load. This is a PortMaster-compatible package, not a PortMaster catalogue submission. It contains no original game files, assets, saves or user settings.

License: the engine is **GPL-3.0-or-later** (source linked below); the ReNFS5 launcher and scripts are **MIT**. Third-party licenses are in `nfs5/licenses/`. The original game, its assets and trademarks belong to their owners (Electronic Arts, Porsche); this project is unofficial and not affiliated with them.
