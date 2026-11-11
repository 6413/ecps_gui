#!/usr/bin/env bash
# Builds the ecps server through fan's xmake.
#
# Usage: ./compile_server.sh [release|debug] [clang|gcc]
# FAN_DIR points at the fan source tree (default: ../fan next to this repo).
# The binary is copied to build/ecps_server.exe in this repo.
set -euo pipefail

mode=${1:-release}
compiler=${2:-clang}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
fan_dir=$(realpath "${FAN_DIR:-$script_dir/../fan}")
main="$script_dir/server/ecps_server.cpp"
out_dir="$script_dir/build"

[[ -f "$fan_dir/xmake.lua" ]] || { echo "fan not found at $fan_dir (set FAN_DIR)" >&2; exit 1; }

cd "$fan_dir"

xmake f -y -c -m "$mode" --compiler="$compiler" \
  --FAN_WINDOW=n --FAN_2D=n --FAN_GUI=n --FAN_PHYSICS_2D=n --FAN_JSON=y --FAN_NETWORK=y \
  --FAN_VIDEO=n --FAN_FMT=n --FAN_3D=n --FAN_AUDIO=n --FAN_AUDIO_STREAM=n --FAN_WAYLAND_SCREEN=n \
  --FAN_REFLECTION=n --FAN_SHADERC=n \
  --FAN_DEBUG_INFO=y --main="$main" --ldflags="-lev -lssl -lcrypto"
xmake -y -j"$(nproc)" a.exe

# cp then mv, so a running ecps_server.exe gets replaced instead of failing with "Text file busy"
mkdir -p "$out_dir"
# the binary loads assets and state relative to the working directory, so link them from fan
link_from_fan() {
  for name in "$@"; do
    if [[ -e "$fan_dir/$name" ]]; then
      ln -sfn "$fan_dir/$name" "$out_dir/$name"
    fi
  done
}
link_from_fan ecps_server.key
cp "build/linux/x86_64/$mode/a.exe" "$out_dir/ecps_server.exe.new"
# symbols go to ecps_server.exe.debug next to it, gdb and coredumpctl find them through the debuglink
objcopy --only-keep-debug "$out_dir/ecps_server.exe.new" "$out_dir/ecps_server.exe.debug"
strip --strip-all "$out_dir/ecps_server.exe.new"
(cd "$out_dir" && objcopy --add-gnu-debuglink="ecps_server.exe.debug" "ecps_server.exe.new")
mv -f "$out_dir/ecps_server.exe.new" "$out_dir/ecps_server.exe"
echo "built $out_dir/ecps_server.exe"
