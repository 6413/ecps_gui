{
  lib,
  llvmPackages,
  gccForLibs,
  overrideCC,
  xmake,
  pkg-config,
  mold,
  libuv,
  libev,
  openssl,
  # sources, pinned by the flake
  ecpsSrc,
  fanSrc,
  thirdParty,
}:

let
  # fan uses `import std`, which needs clang >= 20 and libstdc++ >= 15
  clangStdenv = overrideCC llvmPackages.stdenv (llvmPackages.clang.override { inherit gccForLibs; });
in
clangStdenv.mkDerivation {
  pname = "ecps-server";
  version = "0-unstable";

  src = ecpsSrc;

  nativeBuildInputs = [ xmake pkg-config mold ];

  # _FORTIFY_SOURCE makes glibc's clang path wrap fprintf & co. in static inline
  # overloads, which libstdc++'s std module can't export
  hardeningDisable = [ "fortify" "fortify3" ];

  buildInputs = [ libuv libev openssl ];

  # fan's xmake.lua and the ecps sources sit next to each other, like the dev layout
  unpackPhase = ''
    runHook preUnpack
    cp -r --no-preserve=mode ${fanSrc} fan
    cp -r --no-preserve=mode $src ecps_gui
    runHook postUnpack
  '';
  sourceRoot = ".";

  # the client's fan patch only touches graphics, video and window code, which the
  # headless build leaves out
  patchPhase = ''
    runHook prePatch
    inc=fan/third_party/fan/include
    mkdir -p $inc
    ${lib.concatMapStrings (name: ''
      if [ -d ${thirdParty.${name}}/${name} ]; then
        mkdir -p $inc/${name} && cp -r ${thirdParty.${name}}/${name}/. $inc/${name}/
      else
        cp -r ${thirdParty.${name}} $inc/${name}
      fi
    '') [ "WITCH" "BCOL" "BLL" "BVEC" "BDBT" "bcontainer" ]}
    chmod -R u+w fan
    runHook postPatch
  '';

  buildPhase = ''
    runHook preBuild
    export HOME=$TMPDIR XMAKE_ROOT=y

    # same toolchain setup as the client (package.nix): xmake runs clang-scan-deps
    # directly, bypassing the cc-wrapper
    mkdir -p $TMPDIR/bin
    ln -s ${llvmPackages.clang-unwrapped}/bin/clang-scan-deps $TMPDIR/bin/
    export PATH=$TMPDIR/bin:$PATH

    # libstdc++.modules.json with absolute paths, so xmake can find `import std`
    std_cc=$(echo ${gccForLibs}/include/c++/*/bits/std.cc)
    [ -f "$std_cc" ] || { echo "libstdc++ std module source not found in ${gccForLibs}" >&2; exit 1; }
    mkdir -p $TMPDIR/stdmod
    cat > $TMPDIR/stdmod/libstdc++.modules.json <<EOF
    {"version": 1, "revision": 1, "modules": [
      {"logical-name": "std", "source-path": "$std_cc", "is-std-library": true},
      {"logical-name": "std.compat", "source-path": "$(dirname $std_cc)/std.compat.cc", "is-std-library": true}
    ]}
    EOF
    export NIX_CFLAGS_COMPILE="$NIX_CFLAGS_COMPILE -B$TMPDIR/stdmod -isystem$(dirname $(dirname $std_cc))/backward"
    clang++ -print-file-name=libstdc++.modules.json

    wrapper_flags=""
    for f in libcxx-cxxflags cc-cxxflags cc-cflags libc-cflags; do
      if [ -f $NIX_CC/nix-support/$f ]; then
        wrapper_flags="$wrapper_flags $(cat $NIX_CC/nix-support/$f)"
      fi
    done
    export ECPS_EXTRA_CFLAGS="$NIX_CFLAGS_COMPILE $wrapper_flags -resource-dir=$(clang++ -print-resource-dir)"
    # libev has no pkg-config file
    export ECPS_EXTRA_LDFLAGS="-L${libev}/lib"

    bash ecps_gui/nix/build-ecps-server.sh fan ecps_gui clang
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 $(find fan/build -type f -name a.exe -path '*release*' | head -n1) $out/bin/ecps-server
    runHook postInstall
  '';

  meta = {
    description = "ECPS relay server (headless, no window or GPU dependencies)";
    homepage = "https://github.com/6413/ecps_gui";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
    mainProgram = "ecps-server";
  };
}
