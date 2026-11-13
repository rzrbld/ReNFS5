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
# VIBRATION: the pad's rumble in races, off unless the launcher turned it on
v=$(nfs5_setting vibration)
case "$v" in
    1|on) export NFS5_RUMBLE=${NFS5_RUMBLE:-1} ;;
    *) export NFS5_RUMBLE=${NFS5_RUMBLE:-0} ;;
esac
# Always fullscreen on a handheld, also under a compositor (Weston etc.).
export NFS5_FULLSCREEN=${NFS5_FULLSCREEN:-1}
# Under a compositor the game window can lose keyboard focus (paused menu,
# gptokeyb's virtual keyboard, the movie window); SDL then drops gamepad
# input. The game is the only thing on screen, so always take it.
export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=${SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS:-1}
# (the old launcher DETAIL setting is ignored: rendering always uses the
# game's own filtering; NFS5_DETAIL=low|high remains a developer switch)
v=$(nfs5_setting screen)
case "$v" in
    ''|default) ;;
    *[!A-Za-z0-9_-]*) ;;
    *) export NFS5_SCREEN=${NFS5_SCREEN:-$v} ;;
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

v=$(nfs5_setting graphics)
case "$v" in
    gles|vulkan) export NFS_GFX=${NFS_GFX:-$v} ;;
esac

# ---- the Open-Unleashed engine --------------------------------------------
# The imported game tree (GameData, FEData, Porsche.exe for the intro movies' tables).
export NFS5_GAME_PATH=${NFS5_GAME_PATH:-${NFS5_DATA:-$gamedir/data}}
# The game's SaveData: the port's saves/ (the recomp's place too; the log goes beside it). The
# first start seeds it with the players the imported game brought (data/SaveData).
export NFS5_SAVE_DIR=${NFS5_SAVE_DIR:-$gamedir/saves}
# the engine's compiled-shader cache (loaded at start: no shader compiles mid-race)
export NFS5_SHADER_CACHE=${NFS5_SHADER_CACHE:-$gamedir/configs/shader-cache}
# GL work on its own thread (the scene of the next frame is built meanwhile); 0 turns it off
export NFS_RENDER_THREAD=${NFS_RENDER_THREAD:-1}
mkdir -p "$NFS5_SAVE_DIR" 2>/dev/null || true
if [ ! -f "$NFS5_SAVE_DIR/player.lst" ] && [ -d "$NFS5_GAME_PATH/SaveData" ]; then
    for f in "$NFS5_GAME_PATH"/SaveData/*; do
        [ -f "$f" ] && [ ! -e "$NFS5_SAVE_DIR/${f##*/}" ] && cp -p "$f" "$NFS5_SAVE_DIR/"
    done
    echo "[nfs5] saves seeded from the imported game's SaveData"
fi
export NFS_GFX=${NFS_GFX:-gles}
export NFS_FULLSCREEN=${NFS_FULLSCREEN:-${NFS5_FULLSCREEN:-1}}
# FRAME CAP: 0 = uncapped
case "${NFS5_FRAME_CAP:-0}" in
    30|60) export NFS_FPS_CAP=${NFS_FPS_CAP:-$NFS5_FRAME_CAP} ;;
esac
# MOVIES off = the patch's NoMovies (read by the engine with nfs5.ini)
[ "${NFS5_PLAY_MOVIES:-1}" = 0 ] && export NFS5_SKIP_MOVIES=1
# muOS runs PipeWire with its socket in /run; a session without XDG_RUNTIME_DIR (an ssh start)
# would leave both SDL2's and ALSA's PipeWire clients without it ("Host is down")
if [ -z "${XDG_RUNTIME_DIR:-}" ] && [ -S /run/pipewire-0 ]; then
    export XDG_RUNTIME_DIR=/run PIPEWIRE_RUNTIME_DIR=${PIPEWIRE_RUNTIME_DIR:-/run}
fi
# Movies paced by the real clock: handheld audio paths can play faster than real time (muOS on
# the H700 consumes 44.1 kHz audio ~16% fast), and a picture synced to the sound would run fast
export NFS5_MOVIE_CLOCK=${NFS5_MOVIE_CLOCK:-wall}
# Races drive with the pad (gptokeyb's virtual Xbox pad): its layout at every race start
export NFS5_PAD_LAYOUT=${NFS5_PAD_LAYOUT:-1}
# Audio: SDL2 first (the firmware's driver), ALSA directly (cpal) if SDL2 cannot open one.
export NFS_AUDIO=${NFS_AUDIO:-auto}
export RUST_BACKTRACE=${RUST_BACKTRACE:-1}

# Guest threads run in parallel (lock only around host calls) when the binary
# is built with RECOMP_THREADED_CPU; NFS5_PARALLEL=0 restores one machine lock.
export NFS5_PARALLEL=${NFS5_PARALLEL:-1}
: "${NFS5_PLAY_MOVIES:=1}"
: "${NFS5_FRAME_CAP:=0}"
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
