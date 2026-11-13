# Third-party components

The [MIT license](LICENSE) covers original renfs5 contributions. Each dependency
retains its own license and source notices.

| Component | Use | License location |
| --- | --- | --- |
| Open-Unleashed (Second-Boot) + ReNFS5 changes | The game engine (`nfs5.aarch64`), `engine/` submodule | GPL-3.0-or-later: `engine/LICENSE`; its crates in `nfs5/licenses/THIRD-PARTY-LICENSES.txt` |
| pcrecomp | Static translation and Win32 compatibility runtime (earlier recomp releases) | `third_party/pcrecomp/LICENSE`, `NOTICE`, `LICENSES/` |
| miniz | Native ZIP extraction | `third_party/miniz/LICENSE` |
| stb_image_write | Image output | License notice in `runtime/render/stb_image_write.h` |
| SDL2 / SDL2_ttf | Window, controllers, audio and text | Supplied by firmware; not bundled in the port |
| GLES / Vulkan driver | Handheld graphics driver, loaded through SDL2 | Supplied by firmware; not bundled in the port |

Launcher styling and shell-session patterns draw on the same porter's Desert
Storm and The ReThing work. Notices in adapted source files are retained.

The original Need for Speed: Porsche Unleashed game remains the property of its
rights holders. Its code and assets are not covered by the renfs5 MIT license.
