#!/usr/bin/env bash
# build_ffmpeg_mini.sh - build the *minimal* embedded FFmpeg that the middleware
# movie player links in.  No host ffmpeg binary is used at runtime.
#
# The player needs three decoders/demuxers only:
#   demuxer=ea, decoder=eamad (EA Madcow video), decoder=adpcm_ea_r1 (audio).
# Everything else (programs, network, swscale, filters, asm) is disabled.
#
# Usage:
#   scripts/build_ffmpeg_mini.sh [--host|--aarch64|--all] [--clean]
# Output:
#   build/ffmpeg/host/     host x86_64 static libs+headers
#   build/ffmpeg/aarch64/  aarch64 static libs+headers (built in the cross image)
#
# Env:
#   FFMPEG_VER=7.1        source version (pinned; codeload.github.com mirror)
#   FFMPEG_URL=...        override source URL
#   HP2_CROSS_IMAGE=reconflict-cross:bookworm   docker image for aarch64
#   FFMPEG_ASM=1          enable host asm (needs nasm; device build keeps it off)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
VER=${FFMPEG_VER:-7.1}
URL=${FFMPEG_URL:-https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n$VER}
CROSS_IMAGE=${NFS5_FFMPEG_CROSS_IMAGE:-nfs5-ffmpeg-cross:bookworm}
SRC=$ROOT/work/ffmpeg-src
DL=$ROOT/work/ffmpeg-src/FFmpeg-n$VER

WHICH="--all"
CLEAN=0
for a in "$@"; do
    case "$a" in
        --host) WHICH="--host" ;;
        --aarch64) WHICH="--aarch64" ;;
        --all) WHICH="--all" ;;
        --clean) CLEAN=1 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "build_ffmpeg_mini.sh: unknown argument $a" >&2; exit 64 ;;
    esac
done

mkdir -p "$SRC"
if [ ! -d "$DL" ]; then
    echo "== fetch FFmpeg n$VER =="
    ( cd "$SRC" && curl -sSL --retry 3 -o "ffmpeg-n$VER.tar.gz" "$URL" && \
      tar xf "ffmpeg-n$VER.tar.gz" )
    echo "source: $(sha256sum "$SRC/ffmpeg-n$VER.tar.gz" | cut -d' ' -f1)  $URL" \
        > "$SRC/SOURCE.txt"
fi

# Out-of-tree builds refuse a source tree that still has an in-tree config.
# NOTE: ffbuild/ in the *source* tarball holds checked-in build rules
# (library.mak, common.mak ...) - only its generated config files may go.
clean_source_generated() {
    rm -f "$DL/config.h" "$DL/config.asm" "$DL/config.mak" "$DL/.config" 2>/dev/null || true
    rm -f "$DL"/ffbuild/config.* "$DL"/ffbuild/.config 2>/dev/null || true
}

CONFIG_ARGS=(
    --disable-everything --disable-programs --disable-doc --disable-network
    --disable-autodetect --disable-avdevice --disable-swscale --disable-swresample
    --disable-postproc --disable-avfilter --enable-static --disable-shared
    --enable-pic --enable-demuxer=ea --enable-decoder=eamad,adpcm_ea_r1
    --enable-protocol=file --disable-iconv --disable-zlib --disable-bzlib
    --disable-lzma --disable-sdl2 --disable-asm \
    --enable-small --disable-autodetect --disable-debug
)

build_host() {
    local out=$ROOT/build/ffmpeg/host
    echo "== ffmpeg-mini host build =="
    clean_source_generated
    if [ "$CLEAN" = 1 ]; then rm -rf "$out"; fi
    if [ -f "$out/lib/libavcodec.a" ]; then echo "host ffmpeg-mini: up to date"; return; fi
    local bdir=$ROOT/build/ffmpeg/build-host
    rm -rf "$bdir"; mkdir -p "$bdir"
    ( cd "$bdir" && "$DL/configure" --prefix="$out" "${CONFIG_ARGS[@]}" \
        > "$ROOT/work/logs/ffmpeg-mini-host.log" 2>&1 && \
      make -j"$(nproc)" >> "$ROOT/work/logs/ffmpeg-mini-host.log" 2>&1 && \
      make install >> "$ROOT/work/logs/ffmpeg-mini-host.log" 2>&1 )
    ls -l "$out/lib/libavformat.a" "$out/lib/libavcodec.a" "$out/lib/libavutil.a"
}

build_aarch64() {
    local out=$ROOT/build/ffmpeg/aarch64
    echo "== ffmpeg-mini aarch64 build (docker $CROSS_IMAGE) =="
    clean_source_generated
    if [ "$CLEAN" = 1 ]; then rm -rf "$out"; fi
    if [ -f "$out/lib/libavcodec.a" ]; then echo "aarch64 ffmpeg-mini: up to date"; return; fi
    mkdir -p "$out"
    if ! docker image inspect "$CROSS_IMAGE" >/dev/null 2>&1; then
        echo "== building cross toolchain image $CROSS_IMAGE (native gcc + aarch64 gcc) =="
        DOCKER_CONFIG=${DOCKER_CONFIG:-$ROOT/work/docker-config} \
        docker build --platform linux/amd64 -t "$CROSS_IMAGE" - > "$ROOT/work/logs/ffmpeg-cross-image.log" 2>&1 <<'DOCKERFILE'
FROM --platform=linux/amd64 debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends gcc-aarch64-linux-gnu libc6-dev-arm64-cross gcc libc6-dev make ca-certificates && rm -rf /var/lib/apt/lists/*
DOCKERFILE
    fi
    docker run --rm -v "$ROOT:/src" -w "/src" \
        -e OUT=/src/build/ffmpeg/aarch64 -e SRC=/src/work/ffmpeg-src/FFmpeg-n$VER \
        "$CROSS_IMAGE" bash -c '
        set -e
        rm -rf /src/build/ffmpeg/build-aarch64
        mkdir -p /src/build/ffmpeg/build-aarch64
        cd /src/build/ffmpeg/build-aarch64
        "$SRC/configure" --prefix="$OUT" \
            --enable-cross-compile --arch=aarch64 --target-os=linux \
            --cross-prefix=aarch64-linux-gnu- \
            --cc=aarch64-linux-gnu-gcc --host-cc=gcc \
            '"${CONFIG_ARGS[*]}"' > /src/work/logs/ffmpeg-mini-aarch64.log 2>&1
        make -j"$(nproc)" >> /src/work/logs/ffmpeg-mini-aarch64.log 2>&1
        make install >> /src/work/logs/ffmpeg-mini-aarch64.log 2>&1
        ls -l "$OUT/lib/libavformat.a" "$OUT/lib/libavcodec.a" "$OUT/lib/libavutil.a"
    '
}

[ "$WHICH" = "--host" ] || build_aarch64
[ "$WHICH" = "--aarch64" ] || build_host
echo "ffmpeg-mini done"
