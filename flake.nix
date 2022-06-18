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
      ton = { host, pkgs ? host, staticGlibc ? false, staticMusl ? false }:
        with host.lib;
        pkgs.stdenv.mkDerivation {
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
            ] else [
              glibc.static
              (openssl.override { static = true; })
              (zlib.override { shared = false; })
            ];

          cmakeFlags = [ "-DTON_USE_ABSEIL=OFF" ]
            ++ optionals (staticMusl || staticGlibc) [
              "-DCMAKE_CROSSCOMPILING=OFF"
              "-DCMAKE_LINK_SEARCH_START_STATIC=ON"
              "-DCMAKE_LINK_SEARCH_END_STATIC=ON"
              "-DBUILD_SHARED_LIBS=OFF"
            ];
          LDFLAGS =
            optional staticGlibc "-static-libgcc -static-libstdc++ -static";
        };
    in with flake-utils.lib;
    eachSystem (with system; [ x86_64-linux x86_64-darwin ]) (system:
      let pkgs = nixpkgs.legacyPackages.${system};
      in {
        defaultPackage = ton {
          inherit pkgs;
          host = pkgs;
        };
      }) // (let host = nixpkgs.legacyPackages.x86_64-linux;
      in {
        packages.x86_64-linux-static.ton = ton {
          inherit host;
          staticGlibc = true;
        };
        packages.x86_64-linux-musl.ton = ton {
          inherit host;
          pkgs = nixpkgs.legacyPackages.x86_64-linux.pkgsStatic;
          staticMusl = true;
        };
      });
}
