let
  nixpkgs = fetchTarball "https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz";
  pkgs = (import nixpkgs {}).pkgsCross.aarch64-multiplatform;
in
pkgs.callPackage (
    { mkShell, cmake, ninja, git, pkg-config  }:
  pkgs.stdenv.mkDerivation {
  pname = "ton";
  version = "dev-bin";

  src = ./.;

  nativeBuildInputs = with pkgs;
    [
      cmake ninja git pkg-config
    ];

  buildInputs = with pkgs;
    [
      pkgsStatic.openssl pkgsStatic.libmicrohttpd pkgsStatic.zlib pkgsStatic.libsodium.dev pkgsStatic.secp256k1 glibc.static
    ];

  makeStatic = true;
#  doCheck = testing;

  cmakeFlags = [
    "-DTON_USE_ABSEIL=OFF"
    "-DNIX=ON"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DCMAKE_LINK_SEARCH_START_STATIC=ON"
    "-DCMAKE_LINK_SEARCH_END_STATIC=ON"
    "-DCMAKE_CTEST_ARGUMENTS=--timeout;1800"
    "-DCMAKE_SYSTEM_NAME=Linux"
    "-DCMAKE_CROSSCOMPILING=TRUE"
  ];

  LDFLAGS = [
     "-static-libgcc" "-static-libstdc++" "-static"
  ];
}) {}
