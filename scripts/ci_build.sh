#!/usr/bin/env bash
# Build the ReNFS5 PortMaster package from a clean checkout (GitHub Actions and local):
#
#   bash scripts/ci_build.sh [vYYYYMMDD-HHMMSS]
#
# 1. the aarch64 cross image (packaging/Dockerfile.portmaster-cross: Debian bullseye, glibc 2.31)
# 2. the engine: the engine/ submodule (Open-Unleashed + the ReNFS5 handheld layer), SDL2-only
#    (--no-default-features), stripped
# 3. the launcher (C, SDL2)
# 4. the package (scripts/package_engine.sh): build/package-out/nfs5.zip + nfs5.zip.sha256
set -euo pipefail
cd "$(dirname "$0")/.."
version=${1:-v$(date -u +%Y%m%d-%H%M%S)}
image=${CROSS_IMAGE:-nfs5-pm-cross:bullseye}

[ -f engine/Cargo.toml ] || git submodule update --init engine
if ! docker image inspect "$image" >/dev/null 2>&1; then
    docker build -f packaging/Dockerfile.portmaster-cross -t "$image" packaging
fi

echo "== engine $(git -C engine rev-parse --short HEAD)"
mkdir -p build/cargo-registry build/package-out
docker run --rm -u "$(id -u):$(id -g)" \
    -v "$PWD/engine":/src -w /src \
    -v "$PWD/build/cargo-registry":/opt/cargo/registry \
    -e CARGO_HOME=/opt/cargo -e CARGO_TARGET_DIR=/src/target-pm \
    -e RUSTFLAGS="-C target-cpu=cortex-a53" \
    "$image" bash -euc '
        cargo build --release --locked -p open-unleashed --target aarch64-unknown-linux-gnu --no-default-features
        aarch64-linux-gnu-strip -o target-pm/nfs5.aarch64 target-pm/aarch64-unknown-linux-gnu/release/open-unleashed'

echo "== launcher"
CROSS_IMAGE=$image bash scripts/cross_build_launcher.sh aarch64

echo "== package $version"
rm -f "build/package-out/nfs5-engine-$version.zip" "build/package-out/nfs5-engine-$version.zip.sha256"
bash scripts/package_engine.sh --engine engine/target-pm/nfs5.aarch64 --version "$version"
cp "build/package-out/nfs5-engine-$version.zip" build/package-out/nfs5.zip
(cd build/package-out && sha256sum nfs5.zip > nfs5.zip.sha256 && cat nfs5.zip.sha256)
