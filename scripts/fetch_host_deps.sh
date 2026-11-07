#!/usr/bin/env bash
# Fetch the one host development dependency that this distro does not ship
# preinstalled (SDL2_ttf development headers), without root, into
# build/host-deps. Ubuntu/Debian only; other distros provide libsdl2-ttf-dev
# system-wide and this script is then a no-op.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
out=$repo/build/host-deps
root=$out/root
if pkg-config --exists SDL2_ttf 2>/dev/null; then
  echo "SDL2_ttf already available system-wide"; exit 0
fi
mkdir -p "$out/debs" "$root"
( cd "$out/debs" && { ls libsdl2-ttf-dev_*.deb >/dev/null 2>&1 || apt-get download libsdl2-ttf-dev libsdl2-ttf-2.0-0; } )
for d in "$out"/debs/*.deb; do dpkg-deb -x "$d" "$root"; done
test -f "$root/usr/include/SDL2/SDL_ttf.h"
echo "SDL2_ttf headers -> $root/usr/include, libs -> $root/usr/lib/x86_64-linux-gnu"
