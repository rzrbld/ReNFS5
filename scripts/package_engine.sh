#!/usr/bin/env bash
# Build the PortMaster package of the Open-Unleashed engine variant (no original game files).
#
#   bash scripts/package_engine.sh --engine <stripped aarch64 open-unleashed> [--screenshot <640x480 png>]
#   (scripts/ci_build.sh runs the whole build: cross image, engine, launcher, this)
#
# The engine for PortMaster is built in packaging/Dockerfile.portmaster-cross (bullseye, glibc 2.31) with
# --no-default-features (SDL2-only audio); the engine is the engine/ submodule:
#   CROSS_IMAGE=nfs5-pm-cross:bullseye TARGET_DIR=target-pm ./build-aarch64.sh --no-default-features
#                                  [--engine-src ~/repos/renfs5-ou] [--version vYYYYMMDD-HHMMSS]
#
# The payload keeps the port's layout: ports/NFS5.sh and ports/nfs5/ with the launcher
# (verify/import + settings) and the game, `nfs5.aarch64` — here the engine. The engine is GPLv3:
# its licence and the source location ship in licenses/.
set -euo pipefail
cd "$(dirname "$0")/.."

version=v$(date -u +%Y%m%d-%H%M%S)
engine=
engine_src=engine
screenshot=packaging/screenshot.png
launcher=build/launcher/nfs5-launcher.aarch64
source_url=
while [[ $# -gt 0 ]]; do
    case $1 in
        --engine) engine=$2; shift 2 ;;
        --engine-src) engine_src=$2; shift 2 ;;
        --screenshot) screenshot=$2; shift 2 ;;
        --version) version=$2; shift 2 ;;
        *) echo "package_engine: unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -f $engine && -f $screenshot && -f $launcher ]] || { echo "package_engine: need --engine, --screenshot and $launcher" >&2; exit 1; }
file -b "$engine" | grep -q 'ARM aarch64' || { echo "package_engine: $engine is not an aarch64 ELF" >&2; exit 1; }
needed=$(docker run --rm -v "$(realpath "$engine")":/b:ro nfs5-pm-cross:bullseye aarch64-linux-gnu-readelf -d /b | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')
for lib in $needed; do
    case $lib in
        libSDL2-2.0.so.0|libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|libgcc_s.so.1|ld-linux-aarch64.so.1) ;;
        *) echo "package_engine: the engine links $lib: PortMaster builds go through SDL2 only" >&2; exit 1 ;;
    esac
done
echo "engine links: $needed"
output=build/package-out/nfs5-engine-$version.zip
[[ ! -e $output ]] || { echo "package_engine: $output exists" >&2; exit 1; }

stage=$PWD/build/package-engine
rm -rf "$stage"
ports=$stage/ports
payload=$ports/nfs5
mkdir -p "$payload"/{saves,configs,source,licenses}
install -m 0755 "$engine" "$payload/nfs5.aarch64"
install -m 0755 "$launcher" "$payload/nfs5-launcher.aarch64"
for name in nfs5-env.sh cpu-session.sh; do install -m 0755 "packaging/$name" "$payload/$name"; done
install -m 0644 packaging/nfs5.gptk "$payload/nfs5.gptk"
install -m 0644 packaging/README.md "$payload/README.md"
install -m 0644 packaging/port.json "$payload/port.json"
install -m 0644 packaging/gameinfo.xml "$payload/gameinfo.xml"
install -m 0644 packaging/config/launcher.cfg "$payload/configs/launcher.cfg"
install -m 0644 packaging/source/PLACE-YOUR-GAME-FILES-HERE.txt "$payload/source/PLACE-YOUR-GAME-FILES-HERE.txt"
printf 'Your saves and the engine log (open-unleashed.log) are kept here.\n' > "$payload/saves/README.txt"
install -m 0644 LICENSE "$payload/licenses/PORT-MIT.txt"
install -m 0644 "$engine_src/LICENSE" "$payload/licenses/ENGINE-GPL-3.0.txt"
# every crate the engine links (the PortMaster build's features: no cpal / ALSA), with its licence
(cd "$engine_src" && python3 packaging/third_party_licenses.py "$payload/licenses/THIRD-PARTY-LICENSES.txt" --target aarch64-unknown-linux-gnu --no-default-features >/dev/null)
install -m 0644 third_party/miniz/LICENSE "$payload/licenses/miniz.txt"
engine_rev=$(git -C "$engine_src" rev-parse --short HEAD 2>/dev/null || echo unknown)
engine_full=$(git -C "$engine_src" rev-parse HEAD 2>/dev/null || echo renfs5-h700)
source_url=${ENGINE_SOURCE_URL:-"Source: https://github.com/rzrbld/Open-Unleashed/tree/$engine_full"}
cat > "$payload/licenses/ENGINE-SOURCE.txt" <<EOT
nfs5.aarch64 is the Open-Unleashed engine (GPL-3.0-or-later), with the ReNFS5 handheld changes
(SDL2 platform layer, native OpenGL ES renderer, SDL2 controllers and audio).
Engine revision: $engine_rev
$source_url
Upstream: https://github.com/Second-Boot/Open-Unleashed
EOT
install -m 0755 packaging/NFS5.sh "$ports/NFS5.sh"
for name in port.json README.md gameinfo.xml; do install -m 0644 "packaging/$name" "$ports/$name"; done
install -m 0644 "$screenshot" "$payload/screenshot.png"
install -m 0644 "$screenshot" "$ports/screenshot.png"
cat > "$payload/BUILD.json" <<EOT
{"package": "nfs5", "variant": "open-unleashed-engine", "version": "$version", "architecture": "aarch64",
 "engine_revision": "$engine_rev", "game_elf": "nfs5.aarch64",
 "game_sha256": "$(sha256sum "$engine" | cut -d' ' -f1)", "launcher_sha256": "$(sha256sum "$launcher" | cut -d' ' -f1)"}
EOT
mkdir -p "$(dirname "$output")"
out_abs=$PWD/$output
(cd "$ports" && zip -qr -X "$out_abs" .)
sha256sum "$output" | tee "$output.sha256"
echo "package: $output"
