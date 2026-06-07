{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  outputs =
    { nixpkgs, ... }:
    let
      system = "aarch64-darwin";
      pkgs = nixpkgs.legacyPackages."${system}";
      llvm = pkgs.llvmPackages_21;
    in
    {
      devShells."${system}".default = pkgs.mkShell {
        buildInputs = [
          llvm.clang
          llvm.clang-tools
          pkgs.gnumake
          pkgs.bear
        ];
      };
    };
}
