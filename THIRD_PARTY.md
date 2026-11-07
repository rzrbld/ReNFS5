# Third-party components

The [MIT license](LICENSE) covers original renfs5 contributions. Each dependency
retains its own license and source notices.

| Component | Use | License location |
| --- | --- | --- |
| pcrecomp | Static translation and Win32 compatibility runtime | `third_party/pcrecomp/LICENSE`, `NOTICE`, `LICENSES/` |
| miniz | Native ZIP extraction | `third_party/miniz/LICENSE` |
| stb_image_write | Image output | License notice in `runtime/render/stb_image_write.h` |
| FFmpeg | Embedded EA-MAD movie decoding | `packaging/licenses/FFmpeg-LGPL.txt`; source version and configuration in `scripts/build_ffmpeg_mini.sh` |
| SDL2 / SDL2_ttf | Window, controllers, audio and text | Supplied by firmware; not bundled in the port |
| Mali / EGL | Handheld graphics driver | Supplied by firmware; not bundled in the port |

FFmpeg is built locally from its upstream sources as a minimal static decoder.
The package includes its license and a copy of the build script.

Launcher styling and shell-session patterns draw on the same porter's Desert
Storm and The ReThing work. Notices in adapted source files are retained.

The original Need for Speed: Porsche Unleashed game remains the property of its
rights holders. Its code and assets are not covered by the renfs5 MIT license.
