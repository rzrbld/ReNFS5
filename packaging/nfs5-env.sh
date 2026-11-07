#!/bin/bash
# Shared runtime environment for the NFS5 port. Sourced by NFS5.sh with
# GAMEDIR/NFS5_ROOT and NFS5_CONFIG already set. Explicit environment values
# always win over the saved launcher settings, so a single run can override:
#   NFS5_FRAME_CAP=60 ./NFS5.sh
#
# Settings keys come from config/launcher.cfg written by nfs5-launcher.
gamedir=${GAMEDIR:-${NFS5_ROOT:-}}
if [ -z "$gamedir" ] || [ ! -d "$gamedir" ]; then
    echo "nfs5-env.sh: GAMEDIR/NFS5_ROOT is not set" >&2
    return 1 2>/dev/null || exit 1
fi
# Player data layout beside the binaries: saves/ and configs/. The old
# userdata/{saves,configs} and flat config/ layouts are still honoured when they
# already exist, so older installs keep working (the runtime migrates them).
if [ -n "${NFS5_CONFIG:-}" ]; then
    cfg=$NFS5_CONFIG
elif [ -d "$gamedir/userdata/configs" ] && [ ! -d "$gamedir/configs" ]; then
    cfg=$gamedir/userdata/configs
elif [ -d "$gamedir/config" ] && [ ! -f "$gamedir/configs/launcher.cfg" ]; then
    cfg=$gamedir/config
else
    cfg=$gamedir/configs
fi
mkdir -p "$gamedir/saves" "$gamedir/configs" 2>/dev/null || true
settings="$cfg/launcher.cfg"

nfs5_setting() {
    sed -n "s/^$1=//p" "$settings" 2>/dev/null | head -n1
}

v=$(nfs5_setting frame_cap)
case "$v" in
    0|30|60) export NFS5_FRAME_CAP=${NFS5_FRAME_CAP:-$v} ;;
esac
v=$(nfs5_setting fps_counter)
case "$v" in
    0|1) export NFS5_FPS_OVERLAY=${NFS5_FPS_OVERLAY:-$v} ;;
esac
v=$(nfs5_setting detail)
case "$v" in
    low|medium|high) export NFS5_DETAIL=${NFS5_DETAIL:-$v} ;;
esac
v=$(nfs5_setting movies)
case "$v" in
    on|1) export NFS5_PLAY_MOVIES=${NFS5_PLAY_MOVIES:-1}; export NFS5_SKIP_MOVIES=${NFS5_SKIP_MOVIES:-0} ;;
    off|0) export NFS5_PLAY_MOVIES=${NFS5_PLAY_MOVIES:-0}; export NFS5_SKIP_MOVIES=${NFS5_SKIP_MOVIES:-1} ;;
esac
v=$(nfs5_setting resolution)
case "$v" in
    640x480|800x600|1024x768) export NFS5_VIDEO_MODE=${NFS5_VIDEO_MODE:-$v} ;;
esac
v=$(nfs5_setting aspect)
case "$v" in
    '4:3'|stretch) export NFS5_ASPECT=${NFS5_ASPECT:-$v} ;;
esac

# Guest threads run in parallel (lock only around host calls) when the binary
# is built with RECOMP_THREADED_CPU; NFS5_PARALLEL=0 restores one machine lock.
export NFS5_PARALLEL=${NFS5_PARALLEL:-1}
: "${NFS5_PLAY_MOVIES:=1}"
: "${NFS5_FRAME_CAP:=60}"
: "${NFS5_FPS_OVERLAY:=0}"
: "${NFS5_DETAIL:=medium}"
: "${NFS5_ASPECT:=4:3}"
export NFS5_PAD_MAP=${NFS5_PAD_MAP:-$cfg/controller.cfg}

# Paths handed to the game host; main.c also defaults these.
: "${NFS5_PAD_KEYS:=1}"
export NFS5_PAD_KEYS
export NFS5_ROOT=$gamedir
export NFS5_DATA=${NFS5_DATA:-$gamedir/data}
export NFS5_USERDATA=${NFS5_USERDATA:-$gamedir}
export NFS5_CONFIG=$cfg
