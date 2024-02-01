# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz
{
  pkgs ? import <nixpkgs> { system = builtins.currentSystem; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
}:
let
     microhttpdmy = (import ./microhttpd.nix) {};
in
with import microhttpdmy;
pkgs.llvmPackages_16.stdenv.mkDerivation {
  pname = "ton";
  version = "dev-lib";

  src = ./.;

  nativeBuildInputs = with pkgs;
    [
      cmake ninja git pkg-config
    ];

  buildInputs = with pkgs;
    [
      pkgsStatic.openssl microhttpdmy pkgsStatic.zlib pkgsStatic.libsodium.dev pkgsStatic.secp256k1
    ];

  dontAddStaticConfigureFlags = false;

  cmakeFlags = [
    "-DTON_USE_ABSEIL=OFF"
    "-DNIX=ON"
    "-DMHD_FOUND=1"
    "-DMHD_INCLUDE_DIR=${microhttpdmy}/usr/local/include"
    "-DMHD_LIBRARY=${microhttpdmy}/usr/local/lib/libmicrohttpd.a"
  ];

  LDFLAGS = [
     "-static-libgcc" "-static-libstdc++" "-fPIC" "-fcommon"
  ];

  ninjaFlags = [
    "tonlibjson" "emulator"
  ];
}
