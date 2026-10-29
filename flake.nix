{
  description = "ECPS - Every Communication Program Sucks";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # fan engine and the header-only repos its install.sh would clone
    fan = { url = "github:6413/fan/c9b6b9c89e2ae3ae226453db318bd3632b72a35d"; flake = false; };
    WITCH = { url = "github:7244/WITCH/23b108bb11c260b440042733cef46b8a08711e17"; flake = false; };
    BCOL = { url = "github:7244/BCOL/48de3320c1de4e34f908b5546330e73e33953fbf"; flake = false; };
    BLL = { url = "github:7244/BLL/06ae42a7d8ec242a0fe5dd28d07bde3c4e52dd3a"; flake = false; };
    BVEC = { url = "github:7244/BVEC/79138022d089d7fb7ea16d9fb510c3b057a68f99"; flake = false; };
    BDBT = { url = "github:7244/BDBT/80db1c524c4dc2bac3577f82d94514062b45ed46"; flake = false; };
    bcontainer = { url = "github:7244/bcontainer/3cff64db520724769e1397bbd6a333747eaa10a2"; flake = false; };
    pixfconv = { url = "github:7244/pixfconv/be836d06621b294f1d3cbb50d3b86665ff9fcb5b"; flake = false; };
    PIXF = { url = "github:6413/PIXF/af94c9963077e617b86ee4677fe9601a398915c2"; flake = false; };
    VulkanMemoryAllocator = { url = "github:GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/3aa921224c154a0d2c43912bc88e1c42ce1f7607"; flake = false; };
    VulkanUtilityLibraries = { url = "github:KhronosGroup/Vulkan-Utility-Libraries/vulkan-sdk-1.4.335.0"; flake = false; };
    # must match the utility headers above (fan's xmake.lua pins the same SDK)
    VulkanHeaders = { url = "github:KhronosGroup/Vulkan-Headers/vulkan-sdk-1.4.335.0"; flake = false; };

    # versions fan's xmake.lua pins
    lunasvg = { url = "github:sammycage/lunasvg/v2.4.1"; flake = false; };
    box2d = { url = "github:erincatto/box2d/v3.1.1"; flake = false; };
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
