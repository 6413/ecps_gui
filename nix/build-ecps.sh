#!/usr/bin/env bash
# Builds the ecps client inside a fan source tree using only libraries already
# installed in the environment (pkg-config / compiler search paths). xmake's
# package manager is disabled so the build works offline, e.g. in the Nix sandbox.
#
# Usage: build-ecps.sh <writable-fan-src> <ecps-src> [gcc|clang]
# Extra flags can be passed with ECPS_EXTRA_CFLAGS / ECPS_EXTRA_LDFLAGS.
set -euo pipefail

fan_src=$1
ecps_src=$(realpath "$2")
compiler=${3:-gcc}
jobs=${NIX_BUILD_CORES:-$(nproc)}

cd "$fan_src"

# Every dependency fan pulls through add_requires comes from the environment instead.
sed -i '1i function add_requires(...) end\nfunction add_requireconfs(...) end\nfunction add_packages(...) end' xmake.lua

# install.sh would git clone the third-party headers; the caller provides them.
for dir in WITCH BCOL BLL BVEC BDBT bcontainer pixfconv PIXF vulkan; do
  [[ -d third_party/fan/include/$dir ]] || { echo "missing third_party/fan/include/$dir" >&2; exit 1; }
done
[[ -f third_party/fan/include/vk_mem_alloc.h ]] || { echo "missing vk_mem_alloc.h" >&2; exit 1; }
touch third_party/fan/.core.stamp third_party/fan/.gfx.stamp

pkgs="libuv glfw3 libpng libwebp freetype2 fmt shaderc zlib libavformat libavcodec libswscale libavutil"
cflags="$(pkg-config --cflags $pkgs) ${ECPS_EXTRA_CFLAGS:-}"
ldflags="$(pkg-config --libs $pkgs) -llunasvg -lbox2d ${ECPS_EXTRA_LDFLAGS:-}"

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
    echo "build-ecps.sh: path without a flag in compiler flags: $flag" >&2
    echo "cflags: $cflags" >&2
    exit 1
  fi
done

xmake f -y -c -m release --compiler="$compiler" --toolchain="$compiler" \
  --FAN_WINDOW=y --FAN_2D=y --FAN_GUI=y --FAN_PHYSICS_2D=y --FAN_JSON=y \
  --FAN_NETWORK=y --FAN_VIDEO=y --FAN_FMT=y --FAN_SHADERC=y \
  --FAN_3D=n --FAN_AUDIO=n --FAN_WAYLAND_SCREEN=n --FAN_REFLECTION=n \
  --main="$ecps_src/client/ecps.cpp" \
  --cxflags="$cflags" --ldflags="$ldflags"
xmake -y -j"$jobs" a.exe
