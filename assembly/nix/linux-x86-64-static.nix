# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.11.tar.gz
# copy linux-x86-64-static.nix to git root directory and execute:
# nix-build linux-x86-64-static.nix

{ pkgs ? import <nixpkgs> { system = builtins.currentSystem; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
}:
pkgs.stdenv.mkDerivation { # gcc
  pname = "ton";
  version = "dev-bin";

  src = ./.;

  nativeBuildInputs = with pkgs;
    [
      cmake ninja git pkg-config
    ];

  buildInputs = with pkgs;
    [
      pkgsStatic.openssl pkgsStatic.zlib pkgsStatic.libmicrohttpd.dev pkgsStatic.libsodium.dev pkgsStatic.secp256k1 glibc.static
    ];

  makeStatic = true;
  doCheck = true;

  configureFlags = [

  ];
  cmakeFlags = [
    "-DTON_USE_ABSEIL=OFF"
    "-DNIX=ON"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DCMAKE_LINK_SEARCH_START_STATIC=ON"
    "-DCMAKE_LINK_SEARCH_END_STATIC=ON"
  ];

  LDFLAGS = [
     "-static-libgcc" "-static-libstdc++" "-fPIC" "--enable-static-pie" "-static"
 ];

}
