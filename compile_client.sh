#!/usr/bin/env bash
# Builds the ecps client through fan's xmake.
#
# Usage: ./compile_client.sh [release|debug] [clang|gcc]
# FAN_DIR points at the fan source tree (default: ../fan next to this repo).
# ECPS_WAYLAND_CAPTURE=0 captures through X11 instead of the xdg-desktop-portal + PipeWire
# (default: 1 on a Wayland session, 0 otherwise).
# The binary is copied to build/ecps.exe in this repo.
set -euo pipefail

mode=${1:-release}
compiler=${2:-clang}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
fan_dir=$(realpath "${FAN_DIR:-$script_dir/../fan}")
main="$script_dir/client/ecps.cpp"
out_dir="$script_dir/build"

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
  --FAN_DEBUG_INFO=y --main="$main" --ldflags="-lev -lssl -lcrypto"
xmake -y -j"$(nproc)" a.exe

# cp then mv, so a running ecps.exe gets replaced instead of failing with "Text file busy"
mkdir -p "$out_dir"
# the binary loads assets and state relative to the working directory, so link them from fan
link_from_fan() {
  for name in "$@"; do
    if [[ -e "$fan_dir/$name" ]]; then
      ln -sfn "$fan_dir/$name" "$out_dir/$name"
    fi
  done
}
link_from_fan shaders fonts icons images config.json known_hosts imgui.ini fan_settings.json gui_values.json pipeline_cache.bin
cp "build/linux/x86_64/$mode/a.exe" "$out_dir/ecps.exe.new"
# symbols go to ecps.exe.debug next to it, gdb and coredumpctl find them through the debuglink
objcopy --only-keep-debug "$out_dir/ecps.exe.new" "$out_dir/ecps.exe.debug"
strip --strip-all "$out_dir/ecps.exe.new"
(cd "$out_dir" && objcopy --add-gnu-debuglink="ecps.exe.debug" "ecps.exe.new")
mv -f "$out_dir/ecps.exe.new" "$out_dir/ecps.exe"
echo "built $out_dir/ecps.exe"
