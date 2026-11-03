{
  description = "ECPS - Every Communication Program Sucks";

  inputs = {
    # every input is a commit, not a branch or tag: resolving those costs a GitHub API call
    # per run, and without a lock file (--no-write-lock-file) that hits the 60/hour limit
    # nixos-unstable as of 2026-11-03
    nixpkgs.url = "github:NixOS/nixpkgs/0bb2eedc20e4739f29e8045034f2828ad7054f8f";

    # fan engine and the header-only repos its install.sh would clone
    fan = { url = "github:6413/fan/8e469f1bb4da37e2dbe430b692d39eb8a84fa7d5"; flake = false; };
    WITCH = { url = "github:7244/WITCH/b94748428c247168cc08e4b8199c411936ce62cd"; flake = false; };
    BCOL = { url = "github:7244/BCOL/48de3320c1de4e34f908b5546330e73e33953fbf"; flake = false; };
    BLL = { url = "github:7244/BLL/06ae42a7d8ec242a0fe5dd28d07bde3c4e52dd3a"; flake = false; };
    BVEC = { url = "github:7244/BVEC/79138022d089d7fb7ea16d9fb510c3b057a68f99"; flake = false; };
    BDBT = { url = "github:7244/BDBT/80db1c524c4dc2bac3577f82d94514062b45ed46"; flake = false; };
    bcontainer = { url = "github:7244/bcontainer/3cff64db520724769e1397bbd6a333747eaa10a2"; flake = false; };
    pixfconv = { url = "github:7244/pixfconv/be836d06621b294f1d3cbb50d3b86665ff9fcb5b"; flake = false; };
    PIXF = { url = "github:6413/PIXF/af94c9963077e617b86ee4677fe9601a398915c2"; flake = false; };
    VulkanMemoryAllocator = { url = "github:GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/3aa921224c154a0d2c43912bc88e1c42ce1f7607"; flake = false; };
    # vulkan-sdk-1.4.335.0
    VulkanUtilityLibraries = { url = "github:KhronosGroup/Vulkan-Utility-Libraries/c010c19e796035e92fb3b0462cb887518a41a7c1"; flake = false; };
    # must match the utility headers above (fan's xmake.lua pins the same SDK), vulkan-sdk-1.4.335.0
    VulkanHeaders = { url = "github:KhronosGroup/Vulkan-Headers/2fa203425eb4af9dfc6b03f97ef72b0b5bcb8350"; flake = false; };

    # versions fan's xmake.lua pins: lunasvg v2.4.1, box2d v3.1.1
    lunasvg = { url = "github:sammycage/lunasvg/4166d0cccfc059b39d5ecfc372524375e59448f9"; flake = false; };
    box2d = { url = "github:erincatto/box2d/8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3"; flake = false; };
  };

  outputs = { self, nixpkgs, ... }@inputs:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      ecps = pkgs.callPackage ./nix/package.nix {
        llvmPackages = pkgs.llvmPackages_latest;
        gccForLibs = pkgs.gcc15.cc;
        ecpsSrc = self;
        fanSrc = inputs.fan;
        lunasvgSrc = inputs.lunasvg;
        box2dSrc = inputs.box2d;
        vulkanHeadersSrc = inputs.VulkanHeaders;
        thirdParty = {
          inherit (inputs) WITCH BCOL BLL BVEC BDBT bcontainer pixfconv PIXF
            VulkanMemoryAllocator VulkanUtilityLibraries;
        };
      };
      ecps-server = pkgs.callPackage ./nix/server.nix {
        llvmPackages = pkgs.llvmPackages_latest;
        gccForLibs = pkgs.gcc15.cc;
        ecpsSrc = self;
        fanSrc = inputs.fan;
        thirdParty = { inherit (inputs) WITCH BCOL BLL BVEC BDBT bcontainer; };
      };
    in {
      packages.${system} = {
        inherit ecps ecps-server;
        default = ecps;
      };

      apps.${system} = {
        default = {
          type = "app";
          program = "${ecps}/bin/ecps";
        };
        server = {
          type = "app";
          program = "${ecps-server}/bin/ecps-server";
        };
      };

      overlays.default = final: prev: { inherit (self.packages.${system}) ecps ecps-server; };
    };
}
