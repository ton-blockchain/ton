{
  inputs = { nixpkgs.url = "github:nixos/nixpkgs"; };
  inputs.flake-compat = {
    url = "github:edolstra/flake-compat";
    flake = false;
  };

  outputs = { self, nixpkgs, flake-compat }:
    let pkgs = nixpkgs.legacyPackages.x86_64-linux;
    in {
      devShell.x86_64-linux =
        with pkgs; mkShell {
          nativeBuildInputs = [ cmake pkg-config git ];
          buildInputs = [ openssl zlib ];
        };
   };
}
