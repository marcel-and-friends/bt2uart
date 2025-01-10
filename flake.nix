{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    esp-dev = {
      url = "github:mirrexagon/nixpkgs-esp-dev";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      esp-dev,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        overlays = [ (import "${esp-dev}/overlay.nix") ];
        pkgs = import nixpkgs { inherit system overlays; };
        esp32-toolchain = pkgs.esp-idf-esp32.override {
          toolsToInclude = [
            "esp-clang"
            "xtensa-esp-elf"
          ];
        };
      in
      {
        devShells.default =
          with pkgs;
          mkShell {
            packages = [
              cmake
              ninja
              esp32-toolchain
            ];
          };
      }
    );
}
