# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz
{
  pkgs ? import <nixpkgs> { system = builtins.currentSystem; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
}:
let
  staticOptions = pkg: pkg.overrideAttrs(oldAttrs: {
    dontDisableStatic = true;
    enableSharedExecutables = false;
    configureFlags = (oldAttrs.configureFlags or []) ++ [ "--without-shared" "--disable-shared" "--disable-tests" ];
  });

  secp256k1Static = (staticOptions pkgs.secp256k1);
  libsodiumStatic = (staticOptions pkgs.libsodium);

  microhttpdStatic = pkgs.libmicrohttpd.overrideAttrs(oldAttrs: {
    dontDisableStatic = true;
    enableSharedExecutables = false;
    configureFlags = (oldAttrs.configureFlags or []) ++ [ "--enable-static" "--disable-tests" "--disable-benchmark" "--disable-shared" "--disable-https" "--with-pic" ];
  });
in
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
      (openssl.override { static = true; }).dev
      microhttpdStatic.dev
      (zlib.override { shared = false; }).dev
      (lz4.override { enableStatic = true; enableShared = false; }).dev
      secp256k1Static
      libsodiumStatic.dev
    ];

  dontAddStaticConfigureFlags = false;
  doCheck = false;
  doInstallCheck = false;

  cmakeFlags = [
    "-DTON_USE_ABSEIL=ON"
    "-DNIX=ON"
    "-DTON_ONLY_TONLIB=ON"
  ];

  LDFLAGS = [
     "-static-libgcc" "-static-libstdc++" "-fPIC" "-fcommon"
  ];

  ninjaFlags = [
    "tonlibjson" "emulator"
  ];
}
