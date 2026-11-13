#!/bin/bash
# ReNFS5 - PortMaster launch script (the Open-Unleashed engine with the ReNFS5 handheld layer).
#
# Follows the PortMaster port template: the firmware's PortMaster control/mod
# scripts handle frontend handover and cleanup, gptokeyb (xbox360 mode) is the
# controller layer and the Start+Select quit, SDL picks the video and audio
# drivers. Starts the native launcher (verify/import + settings), then the game:
# the Open-Unleashed engine (GLES or Vulkan, the launcher's GRAPHICS setting).
#
# Set NFS5_ROOT to use a non-standard payload directory.

# PortMaster control/mod scripts permit unset variables.
set +u

# ---- PortMaster -------------------------------------------------------
# A controller DB the firmware set up for its apps (get_controls replaces it).
nfs5_fw_gcdb=${SDL_GAMECONTROLLERCONFIG_FILE:-}
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then
    controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
    controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
    controlfolder="$XDG_DATA_HOME/PortMaster"
else
    controlfolder="/roms/ports/PortMaster"
fi
if [ ! -f "$controlfolder/control.txt" ]; then
    # muOS and others keep PortMaster elsewhere when started outside the menu
    for candidate in /mnt/mmc/MUOS/PortMaster /mnt/sdcard/PortMaster /roms/tools/PortMaster \
        /roms2/tools/PortMaster /userdata/system/PortMaster /userdata/roms/ports/PortMaster; do
        if [ -f "$candidate/control.txt" ]; then controlfolder=$candidate; break; fi
    done
fi
if [ -f "$controlfolder/control.txt" ]; then
    export controlfolder
    # shellcheck disable=SC1090,SC1091
    source "$controlfolder/control.txt"
    # shellcheck disable=SC1090
    [ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt"
    get_controls
else
    echo "ReNFS5: PortMaster not found; running without it" >&2
fi

# ---- the nfs5/ payload ------------------------------------------------
launcher_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
GAMEDIR=
for candidate in "${NFS5_ROOT:-}" "${directory:+/${directory#/}/ports/nfs5}" "$launcher_dir/nfs5" \
    "$launcher_dir/../ports/nfs5" /mnt/sdcard/ports/nfs5 /mnt/mmc/ports/nfs5 /roms/ports/nfs5 \
    /roms2/ports/nfs5 /userdata/roms/ports/nfs5; do
    [ -n "$candidate" ] && [ -f "$candidate/nfs5-env.sh" ] || continue
    GAMEDIR=$(cd -- "$candidate" && pwd)
    break
done
if [ -z "$GAMEDIR" ]; then
    echo "ReNFS5: cannot find the nfs5 payload; extract nfs5.zip into the ports folder" >&2
    exit 1
fi
export GAMEDIR NFS5_ROOT="$GAMEDIR"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1
printf '[nfs5] payload=%s controlfolder=%s cfw=%s device=%s\n' "$GAMEDIR" "${controlfolder:-}" \
    "${CFW_NAME:-?}" "${DEVICE_NAME:-?}"

if [ "${DEVICE_ARCH:-$(uname -m)}" != aarch64 ]; then
    echo "ReNFS5: this package requires AArch64 firmware." >&2
    exit 1
fi

# ---- environment ------------------------------------------------------
export SDL_GAMECONTROLLERCONFIG="${sdl_controllerconfig:-${SDL_GAMECONTROLLERCONFIG:-}}"
# PortMaster has no mapping for some devices (e.g. Anbernic's stock RK3566/
# RK3568 OS): fall back to the firmware's own controller database, else SDL
# guesses a layout and the analog sticks read zero.
if [ -z "$(printf '%s' "$SDL_GAMECONTROLLERCONFIG" | tr -d '[:space:]')" ]; then
    for db in "$nfs5_fw_gcdb" /mnt/mod/ctrl/configs/gamecontrollerdb.txt \
        /usr/lib/gamecontrollerdb.txt /usr/share/gamecontrollerdb.txt \
        "${controlfolder:-/nonexistent}/gamecontrollerdb.txt"; do
        [ -n "$db" ] && [ -s "$db" ] || continue
        # a file, not the variable: a whole database exceeds the per-variable
        # environment limit; SDL and gptokeyb both read this hint
        unset SDL_GAMECONTROLLERCONFIG
        export SDL_GAMECONTROLLERCONFIG_FILE="$db"
        echo "[nfs5] controller mappings from $db"
        break
    done
fi
# The H700 firmware's generic same-GUID mapping omits the right stick; use the
# named built-in mapping when that exact controller is present.
if [ -f /usr/lib/gamecontrollerdb.txt ] && grep -q 'Name="Deeplay-keys"' /proc/bus/input/devices 2>/dev/null; then
    mapping=$(grep -m1 ',Deeplay-keys,' /usr/lib/gamecontrollerdb.txt)
    [ -z "$mapping" ] || export SDL_GAMECONTROLLERCONFIG="$mapping"
fi
if [ "${CFW_NAME:-}" = muOS ] && [ -d /run/muos/device/sdl ]; then
    [ -n "${SDL_ROTATION+x}" ] || export SDL_ROTATION="$(cat /run/muos/device/sdl/rotation 2>/dev/null)"
    [ -n "${SDL_HQ_SCALER+x}" ] || export SDL_HQ_SCALER="$(cat /run/muos/device/sdl/scaler 2>/dev/null)"
    [ -n "${SDL_BLITTER_DISABLED+x}" ] || export SDL_BLITTER_DISABLED="$(cat /run/muos/device/sdl/blitter_disabled 2>/dev/null)"
fi
# Audio and video: SDL picks the drivers the firmware provides (set
# SDL_AUDIODRIVER / SDL_VIDEODRIVER to override).

$ESUDO chmod +x "$GAMEDIR/nfs5.aarch64" "$GAMEDIR/nfs5-launcher.aarch64" 2>/dev/null || true
mkdir -p "$GAMEDIR/saves" "$GAMEDIR/configs" "$GAMEDIR/source"
: "${NFS5_DATA:=$GAMEDIR/data}"
# Player data lives beside the binaries: <gamedir>/saves and <gamedir>/configs.
# Installs made with the older <gamedir>/userdata layout are migrated by the
# launcher/runtime on first start (a legacy config/ directory is still honoured).
: "${NFS5_USERDATA:=$GAMEDIR}"
if [ -z "${NFS5_CONFIG:-}" ]; then
    if [ -d "$GAMEDIR/userdata/configs" ] && [ ! -d "$GAMEDIR/configs" ]; then
        NFS5_CONFIG=$GAMEDIR/userdata/configs
    elif [ -d "$GAMEDIR/config" ] && [ ! -f "$GAMEDIR/configs/launcher.cfg" ]; then
        NFS5_CONFIG=$GAMEDIR/config
    else
        NFS5_CONFIG=$GAMEDIR/configs
    fi
fi
export NFS5_DATA NFS5_USERDATA NFS5_CONFIG

# A second copy would compete for the CPU cores (NFS5_KILL_STALE=1: tests).
if pgrep -f "^$GAMEDIR/nfs5.aarch64" >/dev/null 2>&1; then
    if [ "${NFS5_KILL_STALE:-0}" = 1 ]; then
        pkill -9 -f "^$GAMEDIR/nfs5.aarch64" 2>/dev/null || true
        sleep 1
    else
        echo "[nfs5] ReNFS5 is already running; exit it first" >&2
        exit 0
    fi
fi

nfs5_game_started=0
nfs5_game_pid=
nfs5_finish() {
    local rc=$?
    if [ -n "$nfs5_game_pid" ]; then
        kill -TERM "$nfs5_game_pid" 2>/dev/null || true
        wait "$nfs5_game_pid" 2>/dev/null || true
    fi
    if [ "$nfs5_game_started" = 1 ]; then
        for p in $(pidof gptokeyb gptokeyb2); do $ESUDO kill -9 "$p" 2>/dev/null || true; done
    fi
    if declare -F nfs5_cpu_restore >/dev/null; then nfs5_cpu_restore; fi
    if [ "${nfs5_panel_swapped:-0}" = 1 ]; then
        one_panel_restore          # the top panel back on
        one_panel_dark top         # and BananaOS's state for a port, which run-port then undoes
    fi
    if [ "$nfs5_game_started" = 1 ] && declare -F pm_finish >/dev/null; then pm_finish || true; fi
    return "$rc"
}
trap nfs5_finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

if [ -f "$GAMEDIR/cpu-session.sh" ]; then
    # shellcheck disable=SC1090
    source "$GAMEDIR/cpu-session.sh"
    nfs5_cpu_begin
fi

# ---- 1. launcher: verify/import and settings --------------------------
# NFS5_AUTOTEST=1 skips the settings UI (the verify/import pass still runs).
nfs5_launcher_args=()
[ "${NFS5_AUTOTEST:-0}" = 1 ] && nfs5_launcher_args+=(--autotest)
"$GAMEDIR/nfs5-launcher.aarch64" --root "$GAMEDIR" "${nfs5_launcher_args[@]}"
launcher_rc=$?
case "$launcher_rc" in
    0) ;;
    10) echo '[nfs5] launcher exited by user'; exit 0 ;;
    *) echo "[nfs5] launcher failed (rc=$launcher_rc); see configs/launcher-worker.log" >&2; exit "$launcher_rc" ;;
esac

# ---- 2. game -----------------------------------------------------------
# shellcheck disable=SC1090
source "$GAMEDIR/nfs5-env.sh" || exit 1
nfs5_game_started=1
# GAME SCREEN (launcher setting, dual-screen handhelds): the display connector
# the game draws on. SDL_KMSDRM_FIRST is how BananaOS's SDL picks the panel
# (run-port sets it for every port); BananaOS lights only the panel a port
# uses, so for its bottom panel (DSI-1) swap which one is lit with its own
# one-panel helper and put that back on exit.
nfs5_panel_swapped=0
if [ -n "${NFS5_SCREEN:-}" ]; then
    export SDL_KMSDRM_FIRST="$NFS5_SCREEN"
    echo "[nfs5] game screen: $NFS5_SCREEN"
    if [ "${CFW_NAME:-}" = BananaOS ] && [ "$NFS5_SCREEN" = DSI-1 ] && [ -f /usr/libexec/banana/one-panel ]; then
        # shellcheck disable=SC1091
        . /usr/libexec/banana/one-panel
        one_panel_dark top; one_panel_restore    # the bottom panel on (run-port turned it off)
        one_panel_dark bottom                    # the top one off while the game runs
        nfs5_panel_swapped=1
    fi
fi
# Controls: gptokeyb (PortMaster's per-device controller layer) turns the
# device's controller into one virtual Xbox 360 pad, which the game uses
# instead of the physical one; it also provides the Start+Select quit.
# Prefer gptokeyb2 (-x): the old gptokeyb's xbox360 mode keeps its own
# virtual pad open (opened twice, closed once) and feeds its output back in,
# so after the first stick or button event the last few values cycle forever
# (phantom left-stick "down" = brake taps and brake lights in races, pointer
# drift in menus, gptokeyb at 25% CPU).
nfs5_gptk=""
if [ -n "${GPTOKEYB2:-}" ]; then
    nfs5_gptk=gptokeyb2
    $GPTOKEYB2 "nfs5.aarch64" -c "$GAMEDIR/nfs5.gptk" -x &
elif [ -n "${GPTOKEYB:-}" ]; then
    nfs5_gptk=gptokeyb
    $GPTOKEYB "nfs5.aarch64" -c "$GAMEDIR/nfs5.gptk" xbox360 &
fi
if [ -n "$nfs5_gptk" ]; then
    for _ in $(seq 1 30); do
        grep -q 'X-Box 360' /proc/bus/input/devices 2>/dev/null && break
        sleep 0.1
    done
    if grep -q 'X-Box 360' /proc/bus/input/devices 2>/dev/null; then
        export NFS5_PAD_ONLY=045e:028e
        echo "[nfs5] using gptokeyb's virtual Xbox 360 pad"
    else
        echo "[nfs5] gptokeyb virtual pad not found; using the device's controller directly"
    fi
fi
if declare -F pm_platform_helper >/dev/null; then
    pm_platform_helper "$GAMEDIR/nfs5.aarch64"
fi
"$GAMEDIR/nfs5.aarch64" &
nfs5_game_pid=$!
wait "$nfs5_game_pid"
rc=$?
if [ "$rc" -gt 128 ]; then
    echo "[nfs5] game exited with signal $((rc - 128)) ($(kill -l $((rc - 128)) 2>/dev/null || echo ?))" >&2
else
    echo "[nfs5] game exited rc=$rc" >&2
fi
nfs5_game_pid=
exit "$rc"
