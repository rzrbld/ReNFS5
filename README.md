# renfs5

renfs5 is a community ARM64 Linux port of **Need for Speed: Porsche Unleashed**
(2000, v3.2), using static recompilation, OpenGL ES graphics and SDL2. It targets
Allwinner H700 handhelds running 64-bit muOS or compatible PortMaster firmware.

This is an experimental port. Desktop racing has been demonstrated; handheld
performance and complete gameplay compatibility are still being verified.

## Quick start

1. Extract `nfs5.zip` into your device's **ports** directory.
2. Put your own installed game folder, or a ZIP of that folder, into
   `nfs5/source/`. It must contain `Porsche.exe`, `Drivers/dx7z.dll`, `FEData/`
   and `GameData/`.
3. Launch **NFS5**. The native launcher imports and verifies the files, then
   opens the settings menu. Python is not required on the handheld.
4. Choose your settings and select **SAVE & LAUNCH**.

On muOS, copy `NFS5.sh` into `Roms/PORTS` if that is where your menu looks for
shortcuts. Keep the payload in `ports/nfs5`; do not create `ports/ports`.
Allow at least **1.5 GiB free** for import. Existing installed assets are reused.

CD images (`.iso`, `.bin` + `.cue`, `.mdf` + `.mds`) are supported only when they
contain an installed game tree. Images containing only a Windows installer
must first be installed on a PC. The translated executable pair is v3.2;
other versions are unverified and can be refused with `NFS5_STRICT_VERSION=1`.

Recommended starting settings: **640×480, 30 FPS cap, 4:3**.
The launcher includes **FPS COUNTER ON/OFF**, movies and controller mapping.
D-pad selects an option, Left/Right changes it, A confirms and B exits.

Requires AArch64 firmware, glibc 2.36+, Bash, SDL2, SDL2_ttf and an OpenGL ES
3.x driver that SDL2 can load. The game links no GPU library: every GLES call
is resolved through the firmware's SDL2, so it uses the driver SDL2 already
uses. Tested on H700 (Mali-G31); other drivers are untested.

## Saves and settings

Saves are in `nfs5/saves/`; settings are in `nfs5/configs/`.
Preserve both directories when updating. Keep `nfs5/log.txt` and
`nfs5/configs/launcher-worker.log` when reporting a problem.

## Build

Use a Linux development PC with GCC, Make, Python 3 (`pefile`, `capstone`,
`Pillow`), SDL2 development files and Docker. Supply your own v3.2 installed
copy; generated translations and original files stay local.

```sh
mkdir -p build
ln -s /absolute/path/to/your/installed/game build/game-input
make translate modules
bash scripts/cross_prepare.sh
JOBS=4 bash scripts/build_target.sh
bash scripts/package_port.sh --screenshot /path/to/your/gameplay.png
```

The screenshot must be a 640×480 gameplay capture. The package is written to
`build/package-out/nfs5.zip`. The handheld launcher imports resources; it does
not compile the game. Original inputs, generated translations and build outputs
are excluded from the public source tree.

## License and acknowledgements

Original renfs5 contributions use the [MIT license](LICENSE). Third-party
components retain their own licenses; see [THIRD_PARTY.md](THIRD_PARTY.md).
Thanks to EA Canada / EA Seattle for the original game, and to pcrecomp,
PortMaster, SDL, FFmpeg, miniz and the Desert Storm / The ReThing port work.

**Need for Speed: Porsche Unleashed**, its original code, artwork, audio and
trademarks belong to their respective rights holders. This project is unofficial
and is not affiliated with or endorsed by them. The MIT license does not cover
the original game or relicense third-party components. No original game,
executables, disc images or assets are included in the public source repository.
Users must supply their own game files.
