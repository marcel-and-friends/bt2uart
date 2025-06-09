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
          toolsToInclude = [
            "esp-clang"
            "xtensa-esp-elf"
            "esp-rom-elfs"
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
