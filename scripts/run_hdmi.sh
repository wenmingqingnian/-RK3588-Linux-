#!/bin/bash
# CarTerminal HDMI startup script for RK3588S.
# Usage: ./scripts/run_hdmi.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$(dirname "$SCRIPT_DIR")"

cd "$APP_DIR"

export DISPLAY=:0
export QT_XCB_GL_INTEGRATION=none
export QT_OPENGL=software

# Fix polluted XDG_RUNTIME_DIR if running as root
if [ "$(id -u)" = "0" ]; then
    export XDG_RUNTIME_DIR=/tmp/runtime-root
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 0700 "$XDG_RUNTIME_DIR"
fi

# Build if binary does not exist
if [ ! -f "./CarTerminal" ]; then
    echo "[run_hdmi] binary not found, building..."
    qmake CarTerminal.pro
    make -j"$(nproc)"
fi

echo "[run_hdmi] starting CarTerminal..."
exec ./CarTerminal "$@"
