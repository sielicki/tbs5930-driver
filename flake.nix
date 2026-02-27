{
  description = "TBS5930 DVB media driver fork (out-of-tree)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs";
    flake-parts.url = "github:hercules-ci/flake-parts";
    kmod-ci.url = "github:sielicki/kmod-ci";
  };

  outputs =
    inputs:
    inputs.flake-parts.lib.mkFlake { inherit inputs; } {
      imports = [ inputs.kmod-ci.flakeModules.default ];

      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      perSystem = _: {
        kernelModuleCI.tbs5930 = {
          module = ./default.nix;
          overlay = true;
          defaultKernel = "linux_latest";
          enableNixosModule = true;
        };
      };
    };
}
