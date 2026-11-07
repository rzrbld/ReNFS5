#!/usr/bin/env bash
# Deterministic source/link-input snapshot hash for the NFS5 cross build.
#
#   hash=$(bash scripts/cross_source_manifest.sh [OUTPUT_FILE])
#
# Hashes every input that can change the packaged binaries or their metadata:
# the Makefile, runtime and win32hle sources, launcher, packaging scripts and
# metadata, generated dispatch glue, translated C units, tool sources, docs
# that are packaged, and the generated link inputs (Mali stubs, SDL2 link-time
# libraries). Test-only *_selftest.c files are excluded because the Makefile
# does not link them.
#
# A git working tree, when present, also records HEAD and a hash of
# `git status --porcelain`; a non-git snapshot records "(not a git working tree)".
# The same script is run by scripts/build_target.sh (record) and
# scripts/package_port.sh (freshness check), so a package maps to exact sources.
set -euo pipefail
cd "$(dirname "$0")/.."

out=${1:-build/target/source-manifest.txt}
mkdir -p "$(dirname "$out")"
# Unique temp name: concurrent build/package invocations must never append into
# the same file (that produced duplicated/corrupted manifests during bring-up).
tmp="$out.tmp.$$"
trap 'rm -f -- "$tmp"' EXIT
: > "$tmp"

{
    echo "nfs5-source-manifest format=1"
    if git rev-parse --verify HEAD >/dev/null 2>&1; then
        echo "git_head=$(git rev-parse HEAD)"
        echo "git_status_sha256=$(git status --porcelain=v1 --untracked-files=normal | sha256sum | cut -d' ' -f1)"
    else
        echo "git_head=(not a git working tree)"
    fi
    echo "cross_image=${CROSS_IMAGE:-nfs5-cross:bookworm}"
} >> "$tmp"

roots=(
    Makefile
    THIRD_PARTY.md
    runtime
    third_party/pcrecomp/runtime
    third_party/pcrecomp/LICENSE
    third_party/pcrecomp/NOTICE
    third_party/pcrecomp/LICENSES
    launcher
    packaging
    tools
    scripts
    docs/packaging
    docs/evidence
    build/game
    build/translated
    third_party/target-libs
)
existing=()
for root in "${roots[@]}"; do
    [[ -e $root ]] && existing+=("$root")
done
[[ ${#existing[@]} -gt 0 ]] || { echo "cross_source_manifest: no inputs found" >&2; exit 1; }

while IFS= read -r -d '' file; do
    case "$file" in
        *_selftest.c|*.pyc|*__pycache__*) continue ;;
    esac
    sha=$(sha256sum -- "$file" | cut -d' ' -f1)
    printf '%s  %s\n' "$sha" "$file"
done < <(
    find "${existing[@]}" -type f \( \
            -name '*.c' -o -name '*.h' -o -name '*.py' -o -name '*.sh' -o -name '*.json' \
            -o -name '*.xml' -o -name '*.cfg' -o -name '*.md' -o -name '*.txt' \
            -o -name 'Makefile' -o -name 'LICENSE' -o -name 'NOTICE' -o -name '*.so' \
        \) ! -name '*_selftest.c' ! -name '*.pyc' -print0 | LC_ALL=C sort -z
) >> "$tmp"

mv "$tmp" "$out"
sha256sum "$out" | cut -d' ' -f1
