{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs";
    flake-compat = {
      url = "github:edolstra/flake-compat";
      flake = false;
    };
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-compat, flake-utils }:
    with flake-utils.lib; eachSystem
      (with system; [ x86_64-linux x86_64-darwin ])
      (system:
        let pkgs = nixpkgs.legacyPackages.${system};
        in {
          defaultPackage = with pkgs; stdenv.mkDerivation {
            pname = "ton";
            version = "dev";

            src = ./.;

            nativeBuildInputs = [ cmake pkg-config git ];
            buildInputs = [ openssl zlib ];

            cmakeFlags = [ "-DTON_USE_ABSEIL=OFF" ];
          };
          devShell = with pkgs; mkShell {
            nativeBuildInputs = [ cmake pkg-config git ];
            buildInputs = [ openssl zlib ];
          };
        }
    );
}
