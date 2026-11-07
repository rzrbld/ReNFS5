#!/usr/bin/env bash
# Run the desktop build against the extracted original files.
# Usage: scripts/run_desktop.sh [seconds] [extra env assignments...]
# Logs to build/run.log (stdout+stderr). Exits with the game's status or 124 on
# timeout. Headless: SDL_VIDEODRIVER defaults to offscreen when no DISPLAY.
set -u
cd "$(dirname "$0")/.."
secs=${1:-60}; shift || true
export NFS5_ROOT=${NFS5_ROOT:-$PWD/build/run}
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
  export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen}
fi
mkdir -p build
( timeout --signal=TERM "$secs" ./build/nfs5-desktop "$@" > build/run.log 2>&1; echo "exit=$?" >> build/run.log ) 
tail -n "${TAIL:-60}" build/run.log
