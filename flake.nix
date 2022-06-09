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
    let ton = { host, pkgs, static ? false }:
      pkgs.stdenv.mkDerivation {
        pname = "ton";
        version = "dev";

        src = ./.;

        nativeBuildInputs = with host;
          [ cmake pkg-config git ] ++
          [ dpkg ];
        buildInputs = with pkgs; [ openssl zlib ];

        cmakeFlags = [ "-DTON_USE_ABSEIL=OFF" ] ++
          host.lib.optionals static [ "-DCMAKE_CROSSCOMPILING=OFF"
                                      "-DCMAKE_LINK_SEARCH_START_STATIC=ON"
                                      "-DCMAKE_LINK_SEARCH_END_STATIC=ON" ];
    };
    in with flake-utils.lib; eachSystem
      (with system; [ x86_64-linux x86_64-darwin ])
      (system:
        let pkgs = nixpkgs.legacyPackages.${system};
        in {
          defaultPackage = ton { inherit pkgs; host = pkgs; };
        }
      ) //
      {
        packages.x86_64-linux-musl.ton =
          ton { pkgs = nixpkgs.legacyPackages.x86_64-linux.pkgsStatic;
                host = nixpkgs.legacyPackages.x86_64-linux;
                static = true; };
      };
}
