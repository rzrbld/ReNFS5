#!/usr/bin/env bash
# Cross-build the NFS5 game and launcher for the RG40XX-H / H700 (aarch64).
#
#   bash scripts/build_target.sh                 # isolated object tree (default)
#   NFS5_CROSS_INPLACE=1 bash scripts/build_target.sh  # build in the repo tree
#   JOBS=8 bash scripts/build_target.sh
#
# Requires the generated game sources (`make game` / translate) and the public
# cross image from scripts/cross_prepare.sh. The cross build never reuses host
# x86-64 objects: by default it links a private object tree under
# build/cross-src so a desktop build in progress is not disturbed.
set -euo pipefail
cd "$(dirname "$0")/.."

image=${CROSS_IMAGE:-nfs5-cross:bookworm}
docker_bin=${DOCKER:-docker}
export DOCKER_CONFIG=${DOCKER_CONFIG:-$PWD/work/docker-config}
mkdir -p "$DOCKER_CONFIG"
jobs=${JOBS:-$(nproc 2>/dev/null || echo 4)}
# Performance build configuration measured on the RG40XX-H (docs/performance):
#  RECOMP_THREADED_CPU  per-thread guest CPU state (NFS5_PARALLEL guest threads)
#  RECOMP_LOCAL_REGS    lifted bodies keep guest registers in host registers
#  RECOMP_MEM_BASE      guest memory at a fixed host address (constant base)
#  -fno-pie / -no-pie   direct addressing of the runtime's globals
#  RECOMP_NO_CALL_FLAGS no x86 flag copies at calls/returns except where
#                       tools/flag_liveness.py finds them live (+7-11% on H700)
#  NFS5_SDL_GLES        GLES resolved through SDL2 (runtime/render/gles_loader.c):
#                       no libmali/libEGL/libGLESv2 at link time, the game uses
#                       whatever GLES driver the firmware's SDL2 loads
# Override with NFS5_EXTRA_DEFS= / NFS5_EXTRA_LDFLAGS= for a plain build. Changing
# these needs a clean object tree (the Makefile does not track flags).
extra_defs=${NFS5_EXTRA_DEFS--fno-pie -DRECOMP_THREADED_CPU -DRECOMP_LOCAL_REGS -DRECOMP_MEM_BASE=0x100000000ull -DRECOMP_NO_CALL_FLAGS -DRECOMP_INLINE_LOOKUP -DNFS5_SDL_GLES -mcpu=cortex-a53}
extra_ldflags=${NFS5_EXTRA_LDFLAGS--no-pie}
# Profile-guided build (scripts/pgo.sh drives it): NFS5_PGO=gen builds a
# counting binary that writes its profile on the device; NFS5_PGO=use builds
# the optimised one from build/pgo-data. Both compile in the same staged tree
# (/src/build/cross-src), so the profile files match the objects by path.
# -fprofile-partial-training keeps code the training never ran optimised as
# usual instead of for size. -fprofile-correction: NFS5 runs guest threads in
# parallel and the counters are not atomic, so a few come out inconsistent.
pgo_dir=/src/build/pgo-data
case ${NFS5_PGO:-} in
    "") ;;
    gen) extra_defs="$extra_defs -fprofile-generate=$pgo_dir -fprofile-update=single -DNFS5_PGO_GEN"
         extra_ldflags="$extra_ldflags -fprofile-generate=$pgo_dir" ;;
    use) [[ -d build/pgo-data ]] || { echo "build_target: NFS5_PGO=use needs build/pgo-data (run scripts/pgo.sh)" >&2; exit 1; }
         extra_defs="$extra_defs -fprofile-use=$pgo_dir -fprofile-partial-training -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch" ;;
    *) echo "build_target: NFS5_PGO must be gen or use" >&2; exit 1 ;;
esac

fail() { echo "build_target: $*" >&2; exit 1; }

# Serialize against another concurrent cross build/package in the same
# checkout: the isolated tree and build/target metadata are single-owner.
mkdir -p build
exec 9>build/.nfs5-build.lock
flock -n 9 || fail "another build_target/package_port is already running in this checkout"

[[ -f Makefile ]] || fail "Makefile missing"
[[ -f build/game/main_dispatch.c && -f build/game/modules.c ]] ||
    fail "generated game glue missing; run 'make game' first (host translation step)"
[[ -f build/translated/porsche/functions_0000.c ]] ||
    fail "translated sources missing; run 'make game' first"
compgen -G "build/translated/*/functions_*.c" >/dev/null || fail "no translated function units"

"$docker_bin" image inspect "$image" >/dev/null 2>&1 ||
    fail "cross image $image missing; run: bash scripts/cross_prepare.sh"

# Snapshot every build input BEFORE compiling. The package script hashes the
# tree again and refuses to package a binary that no longer matches it, so a
# package always maps to the sources it was built from.
mkdir -p build/target
build_timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
source_manifest_sha256=$(bash scripts/cross_source_manifest.sh "$PWD/build/target/source-manifest.txt")
printf '%s\n' "$source_manifest_sha256" > build/target/source-manifest.hash
printf '%s\n' "$build_timestamp_utc" > build/target/build-timestamp.txt
echo "source snapshot: $source_manifest_sha256 at $build_timestamp_utc" >&2
[[ -d third_party/target-libs/mali-link ]] ||
    fail "link inputs missing; run: bash scripts/cross_prepare.sh"

# Minimal embedded FFmpeg for the in-process EA-MAD movie player (LGPL, static).
# Built once per arch by scripts/build_ffmpeg_mini.sh; the isolated cross tree
# symlinks it so the same Makefile finds build/ffmpeg/aarch64.
if [[ ! -f build/ffmpeg/aarch64/lib/libavcodec.a ]]; then
    echo "== building embedded ffmpeg-mini for aarch64 (first time) ==" >&2
    bash scripts/build_ffmpeg_mini.sh --aarch64 || fail "ffmpeg-mini aarch64 build failed"
fi

# Absolute stub paths, not -l: pkg-config's -L/usr/lib/aarch64-linux-gnu comes
# earlier on the link line, so -lGLESv2 would resolve to Mesa's libGLESv2.so.2
# instead of our libmali.so.0 stub.
gles_libs=""   # NFS5_SDL_GLES: no GPU library at link time

if [[ ${NFS5_CROSS_INPLACE:-0} == 1 ]]; then
    workdir=/src
    echo "== cross build in the source tree (desktop objects are reused by mtime!) ==" >&2
    "$docker_bin" run --rm --network none --read-only --tmpfs /tmp \
        --user "$(id -u):$(id -g)" -v "$PWD:/src:rw" --workdir "$workdir" "$image" \
        bash -eu -o pipefail -c "make -j$jobs OUT=build/nfs5.aarch64 CROSS_COMPILE=aarch64-linux-gnu- GLES_LIBS='$gles_libs' RUNTIME_RPATH= EXTRA_DEFS='$extra_defs' EXTRA_LDFLAGS='$extra_ldflags'"
    gg_output=$PWD/build/nfs5.aarch64
else
    stage=$PWD/build/cross-src
    rm -rf "$stage"
    mkdir -p "$stage/build"
    cp Makefile "$stage/Makefile"
    for d in runtime third_party tools launcher; do ln -s "../../$d" "$stage/$d"; done
    ln -s "../../docs" "$stage/docs"
    ln -s "../../../build/translated" "$stage/build/translated"
    ln -s "../../../build/game" "$stage/build/game"
    ln -s "../../../build/ffmpeg" "$stage/build/ffmpeg"
    echo "== cross build: isolated tree $stage ==" >&2
    "$docker_bin" run --rm --network none --read-only --tmpfs /tmp \
        --user "$(id -u):$(id -g)" -v "$PWD:/src:rw" --workdir /src/build/cross-src "$image" \
        bash -eu -o pipefail -c "make -j$jobs OUT=build/nfs5.aarch64 CROSS_COMPILE=aarch64-linux-gnu- GLES_LIBS='$gles_libs' RUNTIME_RPATH= EXTRA_DEFS='$extra_defs' EXTRA_LDFLAGS='$extra_ldflags'"
    gg_output=$stage/build/nfs5.aarch64
fi

[[ -f "$gg_output" ]] || fail "game binary was not produced: $gg_output"
mkdir -p build
cp "$gg_output" build/nfs5.aarch64
echo "game binary: build/nfs5.aarch64"
echo "== strip debug sections for the package copy (keep the symbol table for device crash addresses) ==" >&2
cp build/nfs5.aarch64 build/nfs5.aarch64.stripped
"$docker_bin" run --rm --network none --read-only --tmpfs /tmp \
    --user "$(id -u):$(id -g)" -v "$PWD:/src:rw" "$image" \
    aarch64-linux-gnu-strip --strip-debug /src/build/nfs5.aarch64.stripped
echo "package binary: build/nfs5.aarch64.stripped ($(stat -c %s build/nfs5.aarch64.stripped) bytes)"

echo "== launcher (aarch64) ==" >&2
bash scripts/cross_build_launcher.sh aarch64

# Detect teammates changing sources while the build ran: the recorded manifest
# hash is the pre-build snapshot; a mismatch means the tree moved mid-build.
source_manifest_after=$(bash scripts/cross_source_manifest.sh "$PWD/build/target/source-manifest-after.txt")
source_changed_during_build=0
if [[ $source_manifest_after != "$source_manifest_sha256" ]]; then
    source_changed_during_build=1
    echo "WARNING: sources changed during the cross build ($source_manifest_sha256 -> $source_manifest_after)" >&2
    echo "WARNING: rerun the build before packaging; the freshness check will reject this binary" >&2
fi

# ---- record and check the actual ELF we produced ----
mkdir -p build/target
python3 - "$PWD/build/nfs5.aarch64" "$PWD/build/launcher/nfs5-launcher.aarch64" \
    "$PWD/build/target/build-report.txt" > build/target/build-report.txt <<'PYREPORT'
import hashlib, subprocess, sys
from pathlib import Path

game_path, launcher_path, _out = sys.argv[1], sys.argv[2], sys.argv[3]

def run(*args):
    return subprocess.run(args, capture_output=True, text=True).stdout

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def image_id():
    return subprocess.run(
        ["docker", "image", "inspect", "-f", "{{.Id}}", "nfs5-cross:bookworm"],
        capture_output=True, text=True, env=__import__("os").environ).stdout.strip()

def toolchain():
    info = Path("third_party/target-libs/build-info.txt")
    wanted = ("aarch64-linux-gnu-gcc:", "libc6-dev-arm64-cross:", "libsdl2-dev:arm64:",
              "libsdl2-ttf-dev:arm64:", "libgles2-mesa-dev:arm64:", "SDL2 runtime baseline:")
    return [line for line in info.read_text().splitlines() if line.startswith(wanted)]

report = ["NFS5 cross-build report (verification evidence; no device was used)"]
report.append(f"cross image: nfs5-cross:bookworm {image_id()}")
for line in toolchain():
    report.append(f"toolchain: {line}")
report.append("host readelf: " + run("readelf", "--version").splitlines()[0])

def symbol_summary(path):
    # readelf -Ws row: Num: Value Size Type Bind Vis Ndx Name
    # An undefined reference to a weak symbol has Bind WEAK: those may legally
    # resolve to 0 at runtime (null-guarded host hooks / diagnostics). Only
    # undefined GLOBAL project symbols are real missing symbols.
    text = run("readelf", "-Ws", "--wide", path)
    und = set()
    weak = set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[6] == "UND":
            und.add(fields[7])
            if fields[4] == "WEAK":
                weak.add(fields[7])
    classify = {"mali": [], "SDL2": [], "SDL2_ttf": [], "runtime": [], "project": []}
    for sym in sorted(und):
        if sym.startswith(("gl", "egl")):
            classify["mali"].append(sym)
        elif sym.startswith("SDL_"):
            classify["SDL2"].append(sym)
        elif sym.startswith("TTF_"):
            classify["SDL2_ttf"].append(sym)
        elif sym.startswith(("nfs", "nfs5", "recomp", "hle_", "win32hle")):
            classify["project"].append(sym)
        else:
            classify["runtime"].append(sym)
    return und, weak, classify

ok = True
for label, path in (("game", game_path), ("launcher", launcher_path)):
    if not Path(path).is_file():
        report.append(f"{label}: MISSING {path}")
        ok = False
        continue
    data = Path(path).read_bytes()
    elf_class = "ELF64" if data[4] == 2 else "NOT-ELF64"
    machine = "AArch64" if "AArch64" in run("readelf", "-h", path) else "NOT-AARCH64"
    dynamic = run("readelf", "-d", "--wide", path)
    needed = [line.split("[")[1].split("]")[0] for line in dynamic.splitlines() if "NEEDED" in line]
    soname = [line.split("[")[1].split("]")[0] for line in dynamic.splitlines() if "SONAME" in line]
    und, weak, classify = symbol_summary(path)
    report.append("")
    report.append(f"== {label}: {path}")
    report.append(f"{label}: file: " + run("file", path).strip())
    report.append(f"{label}: class {elf_class} architecture {machine}")
    report.append(f"{label}: sha256 {sha(path)}")
    report.append(f"{label}: size {Path(path).stat().st_size} bytes")
    report.append(f"{label}: needed {','.join(needed) if needed else '(none)'}")
    report.append(f"{label}: soname {','.join(soname) if soname else '(none)'}")
    report.append(f"{label}: undefined-symbols total={len(und)} "
                  f"mali={len(classify['mali'])} SDL2={len(classify['SDL2'])} "
                  f"SDL2_ttf={len(classify['SDL2_ttf'])} runtime={len(classify['runtime'])}")
    if classify["mali"]:
        report.append(f"{label}: mali-entry-points {','.join(classify['mali'])}")
    if classify["SDL2_ttf"]:
        report.append(f"{label}: sdl2_ttf-entry-points {','.join(classify['SDL2_ttf'])}")
    # Weakly-referenced project symbols (declared __attribute__((weak)) in the
    # runtime or its headers) may legally resolve to 0; every such call site is
    # null-guarded. Undefined GLOBAL project symbols are real missing symbols
    # and fail the build.
    project_undef = set(classify["project"])
    optional = sorted(project_undef & weak)
    missing = sorted(project_undef - weak)
    report.append(f"{label}: project-internal-undefined "
                  f"{'FAIL ' + ','.join(missing) if missing else 'none'}")
    if optional:
        report.append(f"{label}: optional-weak-hooks-unresolved {','.join(optional)} (guarded; resolved to 0)")
    # No GPU library may be linked: the game resolves GLES through SDL2
    # (NFS5_SDL_GLES, runtime/render/gles_loader.c) and the launcher uses SDL only.
    bad = [lib for lib in needed
           if lib.startswith(("libGLESv2.so", "libEGL.so", "libmali.so", "libGL.so"))]
    report.append(f"{label}: packaged-driver-leak {'FAIL ' + ','.join(bad) if bad else 'none'}")
    if elf_class != "ELF64" or machine != "AArch64":
        ok = False
    if missing:
        ok = False
    if bad:
        ok = False
    if label == "game":
        if "libSDL2-2.0.so.0" not in needed:
            report.append(f"{label}: FAIL libSDL2-2.0.so.0 not in NEEDED")
            ok = False
report.append("")
report.append("result: " + ("PASS" if ok else "FAIL"))
print("\n".join(report))
PYREPORT
cat >> build/target/build-report.txt <<EOF
source: manifest_sha256 ${source_manifest_sha256}
source: manifest_file build/target/source-manifest.txt
source: build_timestamp_utc ${build_timestamp_utc}
source: changed_during_build ${source_changed_during_build}
EOF
printf 'game_stripped: %s sha256 %s size %s\n' "build/nfs5.aarch64.stripped" \
    "$(sha256sum build/nfs5.aarch64.stripped | cut -d' ' -f1)" \
    "$(stat -c %s build/nfs5.aarch64.stripped)" >> build/target/build-report.txt
cat build/target/build-report.txt

# Fail hard if the binary does not target the device.
grep -q 'architecture AArch64' build/target/build-report.txt || fail "game binary is not AArch64"
if grep -q 'packaged-driver-leak FAIL' build/target/build-report.txt; then
    fail "binary links a GPU library; GLES must be resolved through SDL2 (NFS5_SDL_GLES)"
fi
grep -q '^result: PASS' build/target/build-report.txt || fail "link verification failed (see build/target/build-report.txt)"
echo "cross build complete: build/nfs5.aarch64 + build/launcher/nfs5-launcher.aarch64"
echo "next: bash scripts/package_port.sh"
