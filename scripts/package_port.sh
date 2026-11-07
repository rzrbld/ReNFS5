#!/usr/bin/env bash
# Build the PortMaster-compatible NFS5 package (no original game files).
#
#   bash scripts/package_port.sh [--version vYYYYMMDD-HHMMSS]
#                                [--output build/package-out/nfs5.zip]
#                                [--binary build/nfs5.aarch64]
#                                [--launcher build/launcher/nfs5-launcher.aarch64]
#                                [--screenshot file.png]
#
# Inputs must come from scripts/build_target.sh (real aarch64 ELF). The script
# fails rather than emitting a package without an aarch64 game binary.
set -euo pipefail
cd "$(dirname "$0")/.."

version=v$(date -u +%Y%m%d-%H%M%S)
output=build/package-out/nfs5.zip
binary=build/nfs5.aarch64.stripped
launcher=build/launcher/nfs5-launcher.aarch64
screenshot=
allow_stale=0
while [[ $# -gt 0 ]]; do
    case $1 in
        --version) version=$2; shift 2 ;;
        --output) output=$2; shift 2 ;;
        --binary) binary=$2; shift 2 ;;
        --launcher) launcher=$2; shift 2 ;;
        --screenshot) screenshot=$2; shift 2 ;;
        --allow-stale) allow_stale=1; shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "package_port: unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ $version =~ ^v[0-9]{8}-[0-9]{6}$ ]] || { echo "package_port: version must be vYYYYMMDD-HHMMSS" >&2; exit 2; }
[[ -f $binary ]] || binary=build/nfs5.aarch64
[[ -f $binary ]] || { echo "package_port: missing build/nfs5.aarch64[.stripped] (run scripts/build_target.sh)" >&2; exit 1; }
[[ -f $launcher ]] || { echo "package_port: missing $launcher (run scripts/build_target.sh)" >&2; exit 1; }
[[ ! -e $output ]] || { echo "package_port: $output exists; choose a new output" >&2; exit 1; }

# Serialize with build_target.sh so hashes and staged files are consistent.
mkdir -p build
exec 9>build/.nfs5-build.lock
flock -n 9 || { echo "package_port: another build_target/package_port is running in this checkout" >&2; exit 1; }

# ---- source freshness gate ---------------------------------------------
# build_target.sh records a hash of every build input before compiling. If the
# tree has changed since, this binary is stale: refuse to package it by default.
source_manifest_sha256=$(bash scripts/cross_source_manifest.sh "$PWD/build/target/package-source-manifest.txt")
recorded_source_hash=$(head -n1 build/target/source-manifest.hash 2>/dev/null || true)
build_timestamp_utc=$(head -n1 build/target/build-timestamp.txt 2>/dev/null || true)
cross_image_id=$("${DOCKER:-docker}" image inspect -f '{{.Id}}' "${CROSS_IMAGE:-nfs5-cross:bookworm}" 2>/dev/null || echo unknown)
if [[ -z $recorded_source_hash || -z $build_timestamp_utc ]]; then
    [[ $allow_stale == 1 ]] || {
        echo "package_port: build/target/source-manifest.hash or build-timestamp.txt missing." >&2
        echo "package_port: run scripts/build_target.sh first (or --allow-stale for a throwaway test package)." >&2
        exit 1
    }
    recorded_source_hash=${recorded_source_hash:-unknown}
    build_timestamp_utc=${build_timestamp_utc:-unknown}
fi
if [[ $recorded_source_hash != "$source_manifest_sha256" ]]; then
    if [[ $allow_stale != 1 ]]; then
        echo "package_port: stale binary: game was built from source snapshot $recorded_source_hash" >&2
        echo "package_port: current source snapshot is $source_manifest_sha256" >&2
        echo "package_port: rerun scripts/build_target.sh before packaging (or --allow-stale for a throwaway test)." >&2
        exit 1
    fi
    echo "package_port: WARNING: packaging a STALE binary (--allow-stale): built $recorded_source_hash, tree $source_manifest_sha256" >&2
    stale_note=1
else
    stale_note=0
fi
echo "source snapshot: $source_manifest_sha256 (built $build_timestamp_utc)"

stage=$PWD/build/package
rm -rf "$stage"
ports=$stage/ports
payload=$ports/nfs5
mkdir -p "$payload"/{saves,configs,source,licenses}

# ---- payload binaries and scripts ---------------------------------------
install -m 0755 "$binary" "$payload/nfs5.aarch64"
install -m 0755 "$launcher" "$payload/nfs5-launcher.aarch64"
for name in nfs5-env.sh cpu-session.sh; do
    install -m 0755 "packaging/$name" "$payload/$name"
done
install -m 0644 packaging/README.md "$payload/README.md"
install -m 0644 packaging/port.json "$payload/port.json"
install -m 0644 packaging/gameinfo.xml "$payload/gameinfo.xml"
install -m 0644 packaging/config/launcher.cfg "$payload/configs/launcher.cfg"
cat > "$payload/saves/README.txt" <<'EOT'
Game saves created by the port are written here (beside the game binaries).
Configuration lives in ../configs/. Both are safe to back up or copy between
devices; they are never part of the port package itself.
EOT
install -m 0644 packaging/source/PLACE-YOUR-GAME-FILES-HERE.txt \
    "$payload/source/PLACE-YOUR-GAME-FILES-HERE.txt"
install -m 0644 LICENSE "$payload/licenses/PORT-MIT.txt"
install -m 0644 LICENSE "$payload/LICENSE"
# Honest limitations and controls: shipped with the port so device users see
# the same status the repository documents.
for doc in KNOWN_ISSUES.md CONTROLS.md MOVIES.md; do
    install -m 0644 "packaging/$doc" "$payload/$doc"
done
# pcrecomp and its bundled component notices.
install -m 0644 third_party/pcrecomp/LICENSE "$payload/licenses/LICENSE.pcrecomp.txt"
install -m 0644 third_party/pcrecomp/NOTICE "$payload/licenses/NOTICE.pcrecomp.txt"
for f in third_party/pcrecomp/LICENSES/*; do
    [ -f "$f" ] || continue
    install -m 0644 "$f" "$payload/licenses/$(basename "$f")"
done
install -m 0644 third_party/miniz/LICENSE "$payload/licenses/miniz.txt"
install -m 0644 scripts/build_ffmpeg_mini.sh "$payload/licenses/build_ffmpeg_mini.sh"
install -m 0644 packaging/licenses/FFmpeg-LGPL.txt "$payload/licenses/FFmpeg-LGPL.txt"
if [[ -f THIRD_PARTY.md ]]; then
    install -m 0644 THIRD_PARTY.md "$payload/licenses/DEPENDENCIES.md"
fi

# ---- Ports shortcut and installed metadata copies ----------------------
install -m 0755 packaging/NFS5.sh "$ports/NFS5.sh"
for name in port.json README.md gameinfo.xml; do
    install -m 0644 "packaging/$name" "$ports/$name"
done

# ---- screenshot ---------------------------------------------------------
if [[ -z $screenshot ]]; then
    # PortMaster requires a gameplay-style 4:3 screenshot. Prefer a captured
    # in-race frame from the port; refuse packaging without gameplay evidence.
    if [[ -f docs/evidence/race/chase_cam_player_car.png ]]; then
        screenshot=$PWD/docs/evidence/race/chase_cam_player_car.png
    fi
fi
if [[ -z $screenshot ]]; then
    echo "package_port: provide a gameplay screenshot with --screenshot (no launcher fallback)" >&2
    exit 1
elif [[ $screenshot != /* ]]; then
    screenshot=$PWD/$screenshot
fi
[[ -f $screenshot ]] || { echo "package_port: screenshot missing" >&2; exit 1; }
install -m 0644 "$screenshot" "$payload/screenshot.png"
install -m 0644 "$screenshot" "$ports/screenshot.png"

# ---- build metadata + manifest -----------------------------------------
toolchain_info=$(grep -E '^(aarch64-linux-gnu-gcc|libc6-dev-arm64-cross|libsdl2-dev:arm64|libsdl2-ttf-dev:arm64|SDL2 runtime baseline):' third_party/target-libs/build-info.txt 2>/dev/null | tr '\n' '|' || true)
python3 - "$PWD" "$payload" "$version" "$binary" "$launcher" \
    "$recorded_source_hash" "$build_timestamp_utc" "$cross_image_id" "$stale_note" "$toolchain_info" <<'PY'
import hashlib, json, sys
from pathlib import Path
(root, payload, version, binary, launcher) = (sys.argv[1], Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4]), Path(sys.argv[5]))
(source_manifest, build_timestamp, cross_image, stale_note, toolchain_info) = sys.argv[6:11]
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
build = {
    "package": "nfs5",
    "version": version,
    "architecture": "aarch64",
    "min_glibc": "2.36",
    "game_elf": "nfs5.aarch64",
    "game_sha256": sha(binary),
    "game_size": binary.stat().st_size,
    "launcher_sha256": sha(launcher),
    "launcher_size": launcher.stat().st_size,
    "requires_player_game_files": True,
    "build_stage": "desktop-playable-bringup",
    "gameplay_status": "desktop: boots the real game, plays intro movies, navigates the full front-end and plays a Quick Race with a drivable car (keyboard throttle/brake/steer, camera cycling, car visible in cockpit/chase), textured track, HUD and audio; device unverified",
    "boots_to_frontend": True,
    "reaches_menus_or_race": True,
    "device_30fps_verified": False,
    "limitations_file": "KNOWN_ISSUES.md",
    "source_manifest_sha256": source_manifest,
    "build_timestamp_utc": build_timestamp,
    "cross_image_id": cross_image,
    "toolchain": [item for item in toolchain_info.split("|") if item],
    "stale_build": bool(int(stale_note)),
    "game_files": {
        "Porsche.exe": "96af10308b12840c7a0770ff87443a8516c3b756f34d2bc276988cc64f56b98c",
        "Drivers/dx7z.dll": "0433cc93106603e9e0a81168e2fa44c40e3121a71b7ae9cd6f3b1f7551f3fc18",
        "version.txt": "3.2",
    },
}
(payload / "BUILD.json").write_text(json.dumps(build, indent=2) + "\n")
manifest = {"format": 1, "files": {}}
for path in sorted(payload.rglob("*")):
    if path.is_file() and path.name != "MANIFEST.json":
        rel = "nfs5/" + str(path.relative_to(payload))
        manifest["files"][rel] = {"size": path.stat().st_size, "sha256": sha(path)}
(payload / "MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(f"payload files: {len(manifest['files'])}")
PY

# ---- validation (before packaging) -------------------------------------
python3 - "$PWD" "$ports" <<'PY'
import json, subprocess, sys
from pathlib import Path
from PIL import Image
root, ports = Path(sys.argv[1]), Path(sys.argv[2])
payload = ports / "nfs5"
errors = []
def need(cond, msg):
    if not cond:
        errors.append(msg)
def elf_ok(path):
    data = Path(path).read_bytes()
    return data[:6] == b"\x7fELF\x02\x01" and int.from_bytes(data[18:20], "little") == 183
for name in ("NFS5.sh", "port.json", "README.md", "gameinfo.xml", "screenshot.png"):
    need((ports / name).is_file(), f"missing top-level {name}")
for name in ("nfs5.aarch64", "nfs5-launcher.aarch64", "nfs5-env.sh",
             "cpu-session.sh", "README.md", "port.json", "gameinfo.xml", "screenshot.png",
             "BUILD.json", "MANIFEST.json", "LICENSE", "configs/launcher.cfg", "saves/README.txt",
             "KNOWN_ISSUES.md", "CONTROLS.md", "MOVIES.md",
             "source/PLACE-YOUR-GAME-FILES-HERE.txt"):
    need((payload / name).is_file(), f"missing payload nfs5/{name}")
known_issues = (payload / "KNOWN_ISSUES.md").read_text()
need("UNVERIFIED" in known_issues, "KNOWN_ISSUES.md does not mark unverified areas")
need("30 FPS" in known_issues, "KNOWN_ISSUES.md does not state the 30 FPS status")
for name in ("nfs5.aarch64", "nfs5-launcher.aarch64"):
    need(elf_ok(payload / name), f"nfs5/{name} is not ELF64 AArch64")
for name in ("saves", "configs"):
    need((payload / name).is_dir(), f"missing payload nfs5/{name} directory")
meta = json.loads((ports / "port.json").read_text())
need(meta.get("version") == 4 and meta.get("name") == "nfs5.zip", "bad port.json version/name")
need(set(meta.get("items", [])) == {"NFS5.sh", "nfs5/"}, "port.json items mismatch")
attr = meta.get("attr", {})
need(attr.get("arch") == ["aarch64"], "port.json arch not aarch64")
need(attr.get("min_glibc") == "2.36", "port.json min_glibc not 2.36")
need(attr.get("rtr") is False and attr.get("exp") is True, "metadata experimental/rtr flags")
need(attr.get("availability") == "commercial", "metadata availability")
gl = json.loads((payload / "BUILD.json").read_text())
need(gl["game_sha256"] == __import__("hashlib").sha256((payload / "nfs5.aarch64").read_bytes()).hexdigest(),
     "BUILD.json game hash mismatch")
for tag, want in (("path", "./NFS5.sh"), ("image", "./nfs5/screenshot.png")):
    import xml.etree.ElementTree as ET
    game = ET.parse(ports / "gameinfo.xml").getroot().find("game")
    need(game is not None and game.findtext(tag) == want, f"gameinfo.xml bad {tag}")
img = Image.open(ports / "screenshot.png")
need(img.size == (640, 480), f"screenshot is {img.size}, need 640x480")
# The game must resolve GLES through SDL2 (NFS5_SDL_GLES), not link a vendor driver.
needed = subprocess.run(["readelf", "-d", str(payload / "nfs5.aarch64")], check=True,
                        capture_output=True, text=True).stdout
for lib in ("libmali", "libEGL", "libGLESv2"):
    need(lib not in needed, f"nfs5.aarch64 links {lib}; build with NFS5_SDL_GLES")
for path in payload.rglob("*"):
    if not path.is_file():
        continue
    rel = str(path.relative_to(payload))
    low = path.name.lower()
    if probe := next((bad for bad in ("libmali", "libglesv2", "libegl", "libsdl2")
                      if bad in low), None):
        errors.append(f"package contains firmware/link library {rel} ({probe})")
    if path.suffix.lower() in (".exe", ".dll", ".iso", ".zip", ".7z", ".rar", ".so"):
        errors.append(f"package contains forbidden file type {rel}")
    if path.suffix == ".py":
        errors.append(f"python file in package: {rel}")
    if "translated/" in rel or rel.startswith("build/"):
        errors.append(f"package contains build/generated content: {rel}")
for name in ("NFS5.sh", "nfs5/nfs5-env.sh", "nfs5/cpu-session.sh"):
    subprocess.run(["bash", "-n", str(ports / name)], check=True)
    need(b"\r\n" not in (ports / name).read_bytes(), f"CRLF in {name}")
manifest = json.loads((payload / "MANIFEST.json").read_text())
for rel, info in manifest["files"].items():
    file = ports / rel
    need(file.is_file() and file.stat().st_size == info["size"], f"manifest size mismatch {rel}")
if errors:
    print("PACKAGE VALIDATION FAILED", file=sys.stderr)
    for e in errors:
        print(" -", e, file=sys.stderr)
    sys.exit(1)
print("PASS: layout, metadata, AArch64 ELF, screenshot, scripts, manifest, no game files")
PY

# ---- zip ----------------------------------------------------------------
mkdir -p "$(dirname "$output")"
python3 - "$PWD" "$ports" "$output" <<'PY'
import sys, zipfile
from pathlib import Path
root, ports, output = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
files = []
for path in sorted(ports.rglob("*")):
    if not path.is_file():
        continue
    rel = str(path.relative_to(ports))
    mode = 0o755 if path.suffix in (".sh",) or path.name.endswith(".aarch64") else 0o644
    files.append((rel, path, mode))
with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for rel, path, mode in files:
        info = zipfile.ZipInfo(rel, (1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = (0o100000 | mode) << 16
        z.writestr(info, path.read_bytes())
with zipfile.ZipFile(output) as z:
    assert z.testzip() is None
print(f"wrote {output}: {len(files)} files, {output.stat().st_size} bytes")
PY
(cd "$(dirname "$output")" && sha256sum "$(basename "$output")" > "$(basename "$output").sha256")

# ---- package report: maps the zip to exact sources and build inputs ------
report="${output%.zip}.report.txt"
package_sha256=$(cut -d' ' -f1 "$output.sha256")
game_sha256=$(sha256sum "$binary" | cut -d' ' -f1)
launcher_sha256=$(sha256sum "$launcher" | cut -d' ' -f1)
{
    echo "NFS5 port package report"
    echo "package: $(basename "$output")"
    echo "package_sha256: $package_sha256"
    echo "package_version: $version"
    echo "source_manifest_sha256: $recorded_source_hash"
    echo "current_tree_manifest_sha256: $source_manifest_sha256"
    echo "game_build_timestamp_utc: $build_timestamp_utc"
    echo "game_binary_sha256: $game_sha256"
    echo "game_binary_size: $(stat -c %s "$binary")"
    echo "launcher_sha256: $launcher_sha256"
    echo "build_stage: desktop-playable-bringup"
    echo "gameplay_status: desktop plays front-end + Quick Race with drivable car (keyboard, camera cycling, car visible), textured world, HUD, audio; device unverified"
    echo "device_30fps_verified: no (UNVERIFIED on device)"
    echo "limitations_file: nfs5/KNOWN_ISSUES.md"
    echo "cross_image_id: $cross_image_id"
    echo "stale_build: $stale_note"
    echo "toolchain:"
    grep -E '^(aarch64-linux-gnu-gcc|libc6-dev-arm64-cross|libsdl2-dev:arm64|libsdl2-ttf-dev:arm64|libgles2-mesa-dev:arm64|SDL2 runtime baseline):' third_party/target-libs/build-info.txt 2>/dev/null | sed 's/^/  /' || true
    echo "files:"
    (cd "$ports" && find . -type f | LC_ALL=C sort | sed 's|^\./|  |')
} > "$report"

echo
echo "== staged package =="
(cd "$ports" && find . -type f | sort | sed 's/^/  /')
echo
echo "== zip =="
cat "$output.sha256"
echo
echo "== package report =="
cat "$report"
echo
echo "PortMaster upstream submission is a separate step; this archive is the installable port."
