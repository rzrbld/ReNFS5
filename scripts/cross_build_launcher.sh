#!/usr/bin/env bash
# Build the NFS5 launcher for the host (testing) or aarch64 (handheld).
#
#   bash scripts/cross_build_launcher.sh host
#   bash scripts/cross_build_launcher.sh aarch64
set -euo pipefail
cd "$(dirname "$0")/.."
target=${1:-host}
image=${CROSS_IMAGE:-nfs5-cross:bookworm}
export DOCKER_CONFIG=${DOCKER_CONFIG:-$PWD/work/docker-config}
mkdir -p build/launcher

sources=(launcher/nfs5_launcher.c launcher/install.c launcher/unpack.c launcher/sha256.c runtime/configuration.c third_party/miniz/miniz.c)

case $target in
host)
    read -r -a cflags <<< "$(pkg-config --cflags sdl2)"
    read -r -a libs <<< "$(pkg-config --libs sdl2)"
    cc -std=gnu11 -O2 -Wall -Wextra -Ilauncher -Iruntime -Ithird_party/miniz "${cflags[@]}" \
        "${sources[@]}" "${libs[@]}" -lm -lpthread -ldl \
        -o build/launcher/nfs5-launcher.host
    echo "built build/launcher/nfs5-launcher.host"
    ;;
aarch64)
    docker_bin=${DOCKER:-docker}
    if ! "$docker_bin" image inspect "$image" >/dev/null 2>&1; then
        echo "cross image $image missing; run: bash scripts/cross_prepare.sh" >&2
        exit 1
    fi
    "$docker_bin" run --rm --network none --read-only --tmpfs /tmp \
        --user "$(id -u):$(id -g)" -v "$PWD:/src:ro" -v "$PWD/build/launcher:/out:rw" \
        --workdir /src "$image" bash -eu -o pipefail -c '
            read -r -a cflags <<< "$(pkg-config --cflags sdl2)"
            read -r -a libs <<< "$(pkg-config --libs sdl2)"
            aarch64-linux-gnu-gcc -std=gnu11 -O2 -Wall -Wextra -mcpu=cortex-a53 \
                -Ilauncher -Iruntime -Ithird_party/miniz "${cflags[@]}" \
                launcher/nfs5_launcher.c launcher/install.c launcher/unpack.c launcher/sha256.c \
                runtime/configuration.c third_party/miniz/miniz.c \
                "${libs[@]}" -lm -lpthread -ldl \
                -o /out/nfs5-launcher.aarch64
        '
    echo "built build/launcher/nfs5-launcher.aarch64"
    ;;
*)
    echo "usage: $0 host|aarch64" >&2
    exit 2
    ;;
esac
