{
  lib,
  llvmPackages,
  gccForLibs,
  overrideCC,
  stdenv,
  cmake,
  xmake,
  pkg-config,
  mold,
  makeWrapper,
  copyDesktopItems,
  makeDesktopItem,
  libuv,
  glfw,
  libpng,
  libwebp,
  freetype,
  fmt,
  shaderc,
  zlib,
  ffmpeg-full,
  vulkan-headers,
  vulkan-loader,
  gtk3,
  openssl,
  curl,
  xorg,
  libxkbcommon,
  wayland,
  # sources, pinned by the flake
  ecpsSrc,
  fanSrc,
  thirdParty,
  lunasvgSrc,
  box2dSrc,
}:

let
  # fan uses `import std`, which needs clang >= 20 and libstdc++ >= 15
  clangStdenv = overrideCC llvmPackages.stdenv (llvmPackages.clang.override { inherit gccForLibs; });

  # fan's imgui freetype backend targets the lunasvg 2.x API
  lunasvg = stdenv.mkDerivation {
    pname = "lunasvg";
    version = "2.4.1";
    src = lunasvgSrc;
    nativeBuildInputs = [ cmake ];
    cmakeFlags = [ "-DBUILD_SHARED_LIBS=ON" ];
  };

  box2d = stdenv.mkDerivation {
    pname = "box2d";
    version = "3.1.1";
    src = box2dSrc;
    nativeBuildInputs = [ cmake ];
    cmakeFlags = [
      "-DBUILD_SHARED_LIBS=ON"
      "-DBOX2D_SAMPLES=OFF"
      "-DBOX2D_BENCHMARKS=OFF"
      "-DBOX2D_DOCS=OFF"
      "-DBOX2D_UNIT_TESTS=OFF"
    ];
  };

  assetDirs = [ "shaders" "fonts" "icons" "images" "audio" "effects" ];
in
clangStdenv.mkDerivation {
  pname = "ecps";
  version = "0-unstable";

  src = ecpsSrc;

  nativeBuildInputs = [ xmake pkg-config mold makeWrapper copyDesktopItems ];

  buildInputs = [
    libuv glfw libpng libwebp freetype fmt shaderc zlib ffmpeg-full
    vulkan-headers vulkan-loader gtk3 openssl curl
    xorg.libX11 xorg.libxcb xorg.libXrandr xorg.libXinerama xorg.libXcursor xorg.libXi
    libxkbcommon wayland lunasvg box2d
  ];

  # fan's xmake.lua and the ecps sources sit next to each other, like the dev layout
  unpackPhase = ''
    runHook preUnpack
    cp -r --no-preserve=mode ${fanSrc} fan
    cp -r --no-preserve=mode $src ecps_gui
    runHook postUnpack
  '';
  sourceRoot = ".";

  patchPhase = ''
    runHook prePatch
    patch -d fan -p1 < ecps_gui/nix/fan-ecps.patch

    # same layout install.sh produces with git clone
    inc=fan/third_party/fan/include
    mkdir -p $inc/vulkan $inc/VulkanMemoryAllocator/include
    ${lib.concatMapStrings (name: ''
      if [ -d ${thirdParty.${name}}/${name} ]; then
        mkdir -p $inc/${name} && cp -r ${thirdParty.${name}}/${name}/. $inc/${name}/
      else
        cp -r ${thirdParty.${name}} $inc/${name}
      fi
    '') [ "WITCH" "BCOL" "BLL" "BVEC" "BDBT" "bcontainer" "pixfconv" "PIXF" ]}
    cp ${thirdParty.VulkanMemoryAllocator}/include/vk_mem_alloc.h $inc/
    cp ${thirdParty.VulkanMemoryAllocator}/include/vk_mem_alloc.h $inc/VulkanMemoryAllocator/include/
    cp ${thirdParty.VulkanUtilityLibraries}/include/vulkan/*.h $inc/vulkan/
    chmod -R u+w fan
    runHook postPatch
  '';

  buildPhase = ''
    runHook preBuild
    export HOME=$TMPDIR XMAKE_ROOT=y

    # xmake runs clang-scan-deps directly, bypassing the cc-wrapper, so hand it the
    # include paths and resource dir the wrapper would normally add
    mkdir -p $TMPDIR/bin
    ln -s ${llvmPackages.clang-unwrapped}/bin/clang-scan-deps $TMPDIR/bin/
    export PATH=$TMPDIR/bin:$PATH

    # xmake finds `import std` through libstdc++.modules.json, whose relative
    # source paths break because nix splits gcc's headers and libs into separate
    # outputs. Write one with absolute paths; -B makes -print-file-name find it.
    std_cc=$(echo ${gccForLibs}/include/c++/*/bits/std.cc)
    [ -f "$std_cc" ] || { echo "libstdc++ std module source not found in ${gccForLibs}" >&2; exit 1; }
    mkdir -p $TMPDIR/stdmod
    cat > $TMPDIR/stdmod/libstdc++.modules.json <<EOF
    {"version": 1, "revision": 1, "modules": [
      {"logical-name": "std", "source-path": "$std_cc", "is-std-library": true},
      {"logical-name": "std.compat", "source-path": "$(dirname $std_cc)/std.compat.cc", "is-std-library": true}
    ]}
    EOF
    export NIX_CFLAGS_COMPILE="$NIX_CFLAGS_COMPILE -B$TMPDIR/stdmod"
    clang++ -print-file-name=libstdc++.modules.json

    # same order the wrapper uses: C++ stdlib headers, compiler flags, then libc
    # (glibc's headers like features.h come from libc-cflags via -idirafter)
    wrapper_flags=""
    for f in libcxx-cxxflags cc-cxxflags cc-cflags libc-cflags; do
      if [ -f $NIX_CC/nix-support/$f ]; then
        wrapper_flags="$wrapper_flags $(cat $NIX_CC/nix-support/$f)"
      fi
    done
    export ECPS_EXTRA_CFLAGS="$NIX_CFLAGS_COMPILE $wrapper_flags -resource-dir=$(clang++ -print-resource-dir)"

    bash ecps_gui/nix/build-ecps.sh fan ecps_gui clang
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    share=$out/share/ecps
    mkdir -p $out/bin $out/libexec $share
    cp $(find fan/build -type f -name a.exe -path '*release*' | head -n1) $out/libexec/ecps
    for d in ${toString assetDirs}; do
      [ -d fan/$d ] && cp -r fan/$d $share/
    done
    cp ecps_gui/client/webrtc.html $share/
    install -Dm644 fan/icons/ecps_logo.png $out/share/icons/hicolor/256x256/apps/ecps.png

    # fan loads assets relative to the working directory and writes its settings
    # there, so run from a writable per-user dir that links the read-only assets
    cat > $out/bin/ecps <<EOF
    #!${stdenv.shell}
    dir="\''${XDG_DATA_HOME:-\$HOME/.local/share}/ecps"
    mkdir -p "\$dir"
    for f in $share/*; do ln -sfn "\$f" "\$dir/"; done
    cd "\$dir"
    exec $out/libexec/ecps "\$@"
    EOF
    chmod +x $out/bin/ecps
    wrapProgram $out/bin/ecps --prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath [ vulkan-loader ]}
    runHook postInstall
  '';

  desktopItems = [
    (makeDesktopItem {
      name = "ecps";
      desktopName = "ECPS";
      comment = "Every Communication Program Sucks - screen sharing client";
      exec = "ecps";
      icon = "ecps";
      categories = [ "Network" "VideoConference" ];
    })
  ];

  meta = {
    description = "ECPS screen sharing / communication client built on the fan engine";
    homepage = "https://github.com/6413/ecps_gui";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
    mainProgram = "ecps";
  };
}
