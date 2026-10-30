#!/usr/bin/env bash
# Builds the headless ecps server inside a fan source tree using only libraries
# already installed in the environment, like build-ecps.sh does for the client.
# The server only imports fan.event, fan.print and fan.network.crypto (a core module
# on OpenSSL), so fan is built with every optional feature off: no window, Vulkan,
# shaderc or GUI.
#
# Usage: build-ecps-server.sh <writable-fan-src> <ecps-src> [gcc|clang]
# Extra flags can be passed with ECPS_EXTRA_CFLAGS / ECPS_EXTRA_LDFLAGS.
set -euo pipefail

fan_src=$1
ecps_src=$(realpath "$2")
compiler=${3:-gcc}
jobs=${NIX_BUILD_CORES:-$(nproc)}

cd "$fan_src"

# Every dependency fan pulls through add_requires comes from the environment instead.
sed -i '1i function add_requires(...) end\nfunction add_requireconfs(...) end\nfunction add_packages(...) end' xmake.lua

# WITCH's NET and EV headers; install.sh would git clone them.
for dir in WITCH BCOL BLL BVEC BDBT bcontainer; do
  [[ -d third_party/fan/include/$dir ]] || { echo "missing third_party/fan/include/$dir" >&2; exit 1; }
done
touch third_party/fan/.core.stamp

# fan.event runs on libuv; WITCH EV (the server's TCP/UDP loop) uses libev on unix,
# which ships no pkg-config file
pkgs="libuv openssl"
cflags="$(pkg-config --cflags $pkgs) ${ECPS_EXTRA_CFLAGS:-}"
ldflags="$(pkg-config --libs $pkgs) -lev ${ECPS_EXTRA_LDFLAGS:-}"

# xmake dedups repeated flag tokens, which turns "-isystem a -isystem b" into
# "-isystem a b"; join path flags with their argument so nothing gets dropped
join_flags() {
  tr -s '[:space:]' ' ' <<< "$1" |
    sed -E 's/(^| )(-cxx-isystem|-isystem-after|-isystem|-idirafter|-iquote|-I|-L|-B) +/\1\2/g'
}
cflags=$(join_flags "$cflags")
ldflags=$(join_flags "$ldflags")

# a path left on its own would be read by clang-scan-deps as an input file
for flag in $cflags $ldflags; do
  if [[ $flag == /* ]]; then
    echo "build-ecps-server.sh: path without a flag in compiler flags: $flag" >&2
    echo "cflags: $cflags" >&2
    exit 1
  fi
done

xmake f -y -c -m release --compiler="$compiler" --toolchain="$compiler" \
  --FAN_WINDOW=n --FAN_2D=n --FAN_GUI=n --FAN_PHYSICS_2D=n --FAN_JSON=n \
  --FAN_NETWORK=n --FAN_VIDEO=n --FAN_FMT=n --FAN_SHADERC=n \
  --FAN_3D=n --FAN_AUDIO=n --FAN_WAYLAND_SCREEN=n --FAN_REFLECTION=n \
  --main="$ecps_src/server/ecps_server.cpp" \
  --cxflags="$cflags" --ldflags="$ldflags"
xmake -y -j"$jobs" a.exe
