#!/usr/bin/env bash
# Builds the ecps client through fan's xmake.
#
# Usage: ./compile_client.sh [release|debug] [clang|gcc]
# FAN_DIR points at the fan source tree (default: ../fan next to this repo).
# ECPS_WAYLAND_CAPTURE=0 captures through X11 instead of the xdg-desktop-portal + PipeWire
# (default: 1 on a Wayland session, 0 otherwise).
# The binary is copied to $FAN_DIR/ecps.exe.
set -euo pipefail

mode=${1:-release}
compiler=${2:-clang}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
fan_dir=$(realpath "${FAN_DIR:-$script_dir/../fan}")
main="$script_dir/client/ecps.cpp"

[[ -f "$fan_dir/xmake.lua" ]] || { echo "fan not found at $fan_dir (set FAN_DIR)" >&2; exit 1; }

default_wayland=0
[[ "${XDG_SESSION_TYPE:-}" == wayland ]] && default_wayland=1
wayland_screen=n
[[ "${ECPS_WAYLAND_CAPTURE:-$default_wayland}" == 1 ]] && wayland_screen=y

cd "$fan_dir"

xmake f -y -c -m "$mode" --compiler="$compiler" \
  --FAN_WINDOW=y --FAN_2D=y --FAN_GUI=y --FAN_PHYSICS_2D=y --FAN_JSON=y --FAN_NETWORK=y \
  --FAN_VIDEO=y --FAN_FMT=y --FAN_SHADERC=y --FAN_3D=n --FAN_AUDIO=n --FAN_AUDIO_STREAM=y \
  --FAN_WAYLAND_SCREEN="$wayland_screen" --FAN_REFLECTION=n \
  --main="$main" --ldflags="-lev -lssl -lcrypto"
xmake -y -j"$(nproc)" a.exe

# cp then mv, so a running ecps.exe gets replaced instead of failing with "Text file busy"
cp "build/linux/x86_64/$mode/a.exe" ecps.exe.new
mv -f ecps.exe.new ecps.exe
echo "built $fan_dir/ecps.exe"
