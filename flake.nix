{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    nixpkgs-esp-dev = {
      url = "github:mirrexagon/nixpkgs-esp-dev";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      nixpkgs-esp-dev,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        overlays = [ (import "${nixpkgs-esp-dev}/overlay.nix") ];
        pkgs = import nixpkgs { inherit system overlays; };
        esp32-toolchain = pkgs.esp-idf-esp32.override {
          rev = "f5c3654a1c2d2a01f7f67def7a0dc48e691f63c0";
          sha256 = "sha256-cLBUuQS1Y4iLZT/kb5GI/X7JZfSqQzQUDy5ODC+O9wU=";

          toolsToInclude = [
            "esp-clang"
            "xtensa-esp-elf"
          ];
        };
      in
      {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            ninja
            esp32-toolchain
          ];
        };
      }
    );
}
