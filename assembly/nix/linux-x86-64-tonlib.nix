# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.11.tar.gz
# copy linux-x86-64-tonlib.nix to git root directory and execute:
# nix-build linux-x86-64-tonlib.nix
{
  pkgs ? import <nixpkgs> { system = builtins.currentSystem; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
}:
let
      system = builtins.currentSystem;

          nixos1909 = (import (builtins.fetchTarball {
            url = "https://channels.nixos.org/nixos-19.09/nixexprs.tar.xz";
            sha256 = "1vp1h2gkkrckp8dzkqnpcc6xx5lph5d2z46sg2cwzccpr8ay58zy";
          }) { inherit system; });
          glibc227 = nixos1909.glibc // { pname = "glibc"; };
          stdenv227 = let
            cc = pkgs.wrapCCWith {
              cc = nixos1909.buildPackages.gcc-unwrapped;
              libc = glibc227;
              bintools = pkgs.binutils.override { libc = glibc227; };
            };
          in (pkgs.overrideCC pkgs.stdenv cc);

in
stdenv227.mkDerivation {
  pname = "ton";
  version = "dev-lib";

  src = ./.;

  nativeBuildInputs = with pkgs;
    [ cmake ninja git pkg-config ];

  buildInputs = with pkgs;
    [
      pkgsStatic.openssl pkgsStatic.zlib pkgsStatic.libmicrohttpd.dev pkgsStatic.libsodium.dev pkgsStatic.secp256k1
    ];

  dontAddStaticConfigureFlags = false;

  cmakeFlags = [
    "-DTON_USE_ABSEIL=OFF"
    "-DNIX=ON"
  ];

  LDFLAGS = [
     "-static-libgcc" "-static-libstdc++" "-fPIC"
  ];

  ninjaFlags = [
    "tonlibjson" "emulator"
  ];
}
