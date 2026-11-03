#!/usr/bin/env bash
# Builds the ecps server through fan's xmake.
#
# Usage: ./compile_server.sh [release|debug] [clang|gcc]
# FAN_DIR points at the fan source tree (default: ../fan next to this repo).
# The binary is copied to $FAN_DIR/ecps_server.exe.
set -euo pipefail

mode=${1:-release}
compiler=${2:-clang}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
fan_dir=$(realpath "${FAN_DIR:-$script_dir/../fan}")
main="$script_dir/server/ecps_server.cpp"

[[ -f "$fan_dir/xmake.lua" ]] || { echo "fan not found at $fan_dir (set FAN_DIR)" >&2; exit 1; }

cd "$fan_dir"

xmake f -y -c -m "$mode" --compiler="$compiler" \
  --FAN_WINDOW=n --FAN_2D=n --FAN_GUI=n --FAN_PHYSICS_2D=n --FAN_JSON=y --FAN_NETWORK=y \
  --FAN_VIDEO=n --FAN_FMT=n --FAN_3D=n --FAN_AUDIO=n --FAN_AUDIO_STREAM=n --FAN_WAYLAND_SCREEN=n \
  --FAN_REFLECTION=n --FAN_SHADERC=n \
  --main="$main" --ldflags="-lev -lssl -lcrypto"
xmake -y -j"$(nproc)" a.exe

# cp then mv, so a running ecps_server.exe gets replaced instead of failing with "Text file busy"
cp "build/linux/x86_64/$mode/a.exe" ecps_server.exe.new
mv -f ecps_server.exe.new ecps_server.exe
echo "built $fan_dir/ecps_server.exe"
