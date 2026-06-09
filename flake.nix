{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  outputs =
    { nixpkgs, ... }:
    let
      system = "aarch64-darwin";
      pkgs = nixpkgs.legacyPackages."${system}";
      llvm = pkgs.llvmPackages_22;
    in
    {
      devShells."${system}".default = (pkgs.mkShell.override { stdenv = llvm.stdenv; }) {
        packages = [
          llvm.clang-tools
          pkgs.gnumake
          pkgs.cmake
          pkgs.bear
        ];
      };
    };
}
