{ pkgs ? import <nixpkgs> { system = builtins.currentSystem; }
, stdenv ? pkgs.stdenv
, fetchgit ? pkgs.fetchgit
}:

stdenv.mkDerivation rec {
  name = "microhttpdmy";


  src = fetchgit {
    url = "https://git.gnunet.org/libmicrohttpd.git";
    rev = "refs/tags/v0.9.77";
    sha256 = "sha256-x+nfB07PbZwBlFc6kZZFYiRpk0a3QN/ByHB+hC8na/o=";
  };

  nativeBuildInputs = with pkgs; [ automake libtool autoconf texinfo ];

  buildInputs = with pkgs; [ ];

  configurePhase = ''
   ./autogen.sh
   ./configure --enable-static --disable-tests --disable-benchmark --disable-shared --disable-https --with-pic
  '';

  installPhase = ''
    make install DESTDIR=$out
  '';
}
