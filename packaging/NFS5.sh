#!/bin/bash
# Need for Speed: Porsche Unleashed - static AArch64 community port.
# Starts the native launcher (verify/import + settings) and then the game.
#
# Works from the ports root (PortMaster extract) and from a muOS Roms/PORTS
# shortcut. Set NFS5_ROOT to use a non-standard payload directory.
# PortMaster control/mod scripts and helper functions permit unset variables.
# Match The ReThing: do not run them under nounset, including inherited bash -u.
set +u

launcher_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# ---- locate the nfs5/ payload ------------------------------------------
gamedir=
if [ -n "${NFS5_ROOT:-}" ] && [ -f "$NFS5_ROOT/nfs5-env.sh" ]; then
    gamedir=$NFS5_ROOT
else
    for candidate in "$launcher_dir/nfs5" "$launcher_dir/../ports/nfs5" \
        "${directory:+/${directory#/}/ports/nfs5}" \
        /mnt/sdcard/ports/nfs5 /mnt/mmc/ports/nfs5 /roms/ports/nfs5 \
        /roms2/ports/nfs5 /userdata/ports/nfs5; do
        [ -n "$candidate" ] && [ -f "$candidate/nfs5-env.sh" ] || continue
        gamedir=$candidate
        break
    done
fi
if [ -z "$gamedir" ]; then
    printf 'NFS5: cannot find the nfs5 payload (nfs5-env.sh).\n' >&2
    printf 'Extract nfs5.zip into the ports folder, or set NFS5_ROOT.\n' >&2
    exit 1
fi
gamedir=$(cd -- "$gamedir" && pwd) || exit 1
export NFS5_ROOT="$gamedir"
export GAMEDIR="$gamedir"

# ---- PortMaster control integration (optional) -------------------------
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -z "${controlfolder:-}" ]; then
    for candidate in /opt/system/Tools/PortMaster /opt/tools/PortMaster \
        "$XDG_DATA_HOME/PortMaster" /mnt/mmc/MUOS/PortMaster /mnt/sdcard/PortMaster \
        /roms/ports/PortMaster /roms/tools/PortMaster /roms2/tools/PortMaster \
        /userdata/system/PortMaster /userdata/roms/ports/PortMaster; do
        if [ -f "$candidate/control.txt" ]; then controlfolder=$candidate; break; fi
    done
fi
if [ -f "${controlfolder:-}/control.txt" ]; then
    export controlfolder
    # shellcheck disable=SC1090
    source "$controlfolder/control.txt"
    [ ! -f "$controlfolder/mod_${CFW_NAME:-}.txt" ] || source "$controlfolder/mod_${CFW_NAME}.txt"
    if declare -F get_controls >/dev/null; then get_controls; fi
fi

# ---- firmware video/input/audio environment ----------------------------
export SDL_GAMECONTROLLERCONFIG="${sdl_controllerconfig:-${SDL_GAMECONTROLLERCONFIG:-}}"
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
# muOS ALSA default is a PipeWire plugin; SSH sessions need its socket path.
if [ -S /run/pipewire-0 ]; then export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run}; fi
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-alsa}

cd "$gamedir" || exit 1
exec > >(tee "$gamedir/log.txt") 2>&1
printf '[nfs5] payload=%s\n' "$gamedir"

if [ "${DEVICE_ARCH:-$(uname -m)}" != aarch64 ]; then
    echo "NFS5: this package requires AArch64 firmware." >&2
    exit 1
fi
chmod +x "$gamedir/nfs5.aarch64" "$gamedir/nfs5-launcher.aarch64" 2>/dev/null || true
mkdir -p "$gamedir/saves" "$gamedir/configs" "$gamedir/source"
: "${NFS5_DATA:=$gamedir/data}"
# Player data lives beside the binaries: <gamedir>/saves and <gamedir>/configs.
# Installs made with the older <gamedir>/userdata layout are migrated by the
# launcher/runtime on first start (a legacy config/ directory is still honoured).
: "${NFS5_USERDATA:=$gamedir}"
if [ -z "${NFS5_CONFIG:-}" ]; then
    if [ -d "$gamedir/userdata/configs" ] && [ ! -d "$gamedir/configs" ]; then
        NFS5_CONFIG=$gamedir/userdata/configs
    elif [ -d "$gamedir/config" ] && [ ! -f "$gamedir/configs/launcher.cfg" ]; then
        NFS5_CONFIG=$gamedir/config
    else
        NFS5_CONFIG=$gamedir/configs
    fi
fi
export NFS5_DATA NFS5_USERDATA NFS5_CONFIG

# A stale copy would compete for the 4 CPU cores: with NFS5_KILL_STALE=1 (the
# automated tests) kill it; otherwise refuse to start a second copy.
if pgrep -f "$gamedir/nfs5.aarch64" >/dev/null 2>&1; then
    if [ "${NFS5_KILL_STALE:-0}" = 1 ]; then
        pkill -9 -f "$gamedir/nfs5.aarch64" 2>/dev/null || true
        sleep 1
    else
        echo "[nfs5] NFS5 is already running; exit it first" >&2
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
    if declare -F nfs5_cpu_restore >/dev/null; then nfs5_cpu_restore; fi
    if declare -F nfs5_muos_resume >/dev/null; then nfs5_muos_resume; fi
    if [ "$nfs5_game_started" = 1 ]; then
        if declare -F pm_finish >/dev/null; then pm_finish || true; fi
    elif declare -F pm_message_end >/dev/null; then
        pm_message_end || true
    fi
    return "$rc"
}
trap nfs5_finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

# muOS: an SSH launch bypasses the normal menu handoff; suspend its frontend
# only for as long as this port runs.
if [ -f "$gamedir/muos-session.sh" ] && [ "${NFS5_NO_MUOS_SESSION:-0}" != 1 ]; then
    # shellcheck disable=SC1090
    source "$gamedir/muos-session.sh"
    if ! nfs5_muos_suspend; then
        # Leftover suspension file from a run that was killed (SSH tests, crash):
        # recover it and retry once, then carry on unsuspended rather than
        # refusing to start the game.
        echo '[nfs5] recovering a stale muOS frontend suspension'
        nfs5_muos_owned=1
        nfs5_muos_resume
        if ! nfs5_muos_suspend; then
            echo '[nfs5] warning: could not suspend the frontend; continuing' >&2
            nfs5_muos_owned=0
        fi
    fi
fi
if [ -f "$gamedir/cpu-session.sh" ]; then
    # shellcheck disable=SC1090
    source "$gamedir/cpu-session.sh"
    nfs5_cpu_begin
fi

# ---- 1. launcher: verify/import and settings ---------------------------
# NFS5_AUTOTEST=1 skips the settings UI (the verify/import pass still runs):
# used by the automated device tests so the shell path can be measured exactly.
nfs5_launcher_args=()
[ "${NFS5_AUTOTEST:-0}" = 1 ] && nfs5_launcher_args+=(--autotest)
"$gamedir/nfs5-launcher.aarch64" --root "$gamedir" "${nfs5_launcher_args[@]}"
launcher_rc=$?
case "$launcher_rc" in
    0) ;;
    10) echo '[nfs5] launcher exited by user'; exit 0 ;;
    *) echo "[nfs5] launcher failed (rc=$launcher_rc); see config/launcher-worker.log" >&2; exit "$launcher_rc" ;;
esac

# ---- 2. game -----------------------------------------------------------
# shellcheck disable=SC1090
source "$gamedir/nfs5-env.sh" || exit 1
if declare -F pm_platform_helper >/dev/null; then
    pm_platform_helper "$gamedir/nfs5.aarch64"
fi
nfs5_game_started=1
"$gamedir/nfs5.aarch64" &
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
