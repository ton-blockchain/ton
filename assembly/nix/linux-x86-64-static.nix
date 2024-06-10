# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz

{ pkgs ? import <nixpkgs> { system = builtins.currentSystem; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
, testing ? false
}:
let
  staticOptions = pkg: pkg.overrideAttrs(oldAttrs: {
    dontDisableStatic = true;
    enableSharedExecutables = false;
    configureFlags = (oldAttrs.configureFlags or []) ++ [ "--without-shared" "--disable-shared" "--disable-tests" ];
  });

  secp256k1Static = (staticOptions pkgs.secp256k1);
  libsodiumStatic = (staticOptions pkgs.libsodium);
  jemallocStatic = (staticOptions pkgs.jemalloc);

  microhttpdStatic = pkgs.libmicrohttpd.overrideAttrs(oldAttrs: {
    dontDisableStatic = true;
    enableSharedExecutables = false;
    configureFlags = (oldAttrs.configureFlags or []) ++ [ "--enable-static" "--disable-tests" "--disable-benchmark" "--disable-shared" "--disable-https" "--with-pic" ];
  });

in
stdenv.mkDerivation {
  pname = "ton";
  version = "dev-bin";

  src = ./.;

  nativeBuildInputs = with pkgs;
    [  cmake ninja git pkg-config ];

  buildInputs = with pkgs;
    [
      (openssl.override { static = true; }).dev
      microhttpdStatic.dev
      (zlib.override { shared = false; }).dev
      (lz4.override { enableStatic = true; enableShared = false; }).dev
      jemallocStatic
      secp256k1Static
      libsodiumStatic.dev
      glibc.static
    ];

  cmakeFlags = [
    "-DTON_USE_ABSEIL=OFF"
    "-DNIX=ON"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DCMAKE_LINK_SEARCH_START_STATIC=ON"
    "-DCMAKE_LINK_SEARCH_END_STATIC=ON"
    "-DTON_USE_JEMALLOC=ON"
  ];

  makeStatic = true;
  doCheck = testing;

  LDFLAGS = [
    "-static-libgcc" "-static-libstdc++" "-fPIC"
  ];
}
