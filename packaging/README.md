# ReNFS5: Recomp Unleashed

ReNFS5 is an unofficial community ARM64 Linux port of **Need for Speed: Porsche Unleashed**
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

## License and acknowledgements

Thanks to EA Canada / EA Seattle for Need for Speed: Porsche Unleashed, and to
PortMaster, pcrecomp, SDL, FFmpeg, miniz, Desert Storm and The ReThing.
Original renfs5 contributions use the MIT license. Dependency notices are in
`licenses/DEPENDENCIES.md`. The original game is not covered by that license;
provide your own game files. This project is unofficial and is not affiliated
with or endorsed by Electronic Arts or Porsche; their names and trademarks
belong to their respective owners.
