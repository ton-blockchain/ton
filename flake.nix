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
    let
      ton = { host, pkgs ? host, stdenv ? pkgs.stdenv, staticGlibc ? false, staticMusl ? false }:
        with host.lib;
        stdenv.mkDerivation {
          pname = "ton";
          version = "dev";

          src = ./.;

          nativeBuildInputs = with host;
            [ cmake ninja pkg-config git ] ++ [ dpkg rpm ];
          buildInputs = with pkgs;
            # at some point nixpkgs' pkgsStatic will build with static glibc
            # then we can skip these manual overrides
            # and switch between pkgsStatic and pkgsStatic.pkgsMusl for static glibc and musl builds
            if !staticGlibc then [
              openssl
              zlib
              libmicrohttpd
            ] else [
              glibc.static
              (openssl.override { static = true; })
              (zlib.override { shared = false; })
              pkgsStatic.libmicrohttpd
            ];

          cmakeFlags = [ "-DTON_USE_ABSEIL=OFF" ]
            ++ optionals staticMusl [
              "-DCMAKE_CROSSCOMPILING=OFF" # pkgsStatic sets cross
            ]
            ++ optionals (staticGlibc || staticMusl) [
              "-DCMAKE_LINK_SEARCH_START_STATIC=ON"
              "-DCMAKE_LINK_SEARCH_END_STATIC=ON"
            ];
        };
    in with flake-utils.lib;
    eachSystem (with system; [ x86_64-linux x86_64-darwin ]) (system:
      let host = nixpkgs.legacyPackages.${system};
      in {
        defaultPackage = ton {
          inherit host;
        };
      }) // (let host = nixpkgs.legacyPackages.x86_64-linux;
      in {
        packages = {
          x86_64-linux-static.ton = ton {
            inherit host;
            stdenv = host.makeStatic host.stdenv;
            staticGlibc = true;
          };
          x86_64-linux-musl.ton = ton {
            inherit host;
            pkgs = nixpkgs.legacyPackages.x86_64-linux.pkgsStatic;
            staticMusl = true;
          };
        };
      });
}
