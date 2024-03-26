{ pkgs ? import <nixpkgs> { system = builtins.currentSystem; }
, stdenv ? pkgs.stdenv
, fetchFromGitHub ? pkgs.fetchFromGitHub
}:

stdenv.mkDerivation rec {
  name = "opensslmy";

  src = fetchFromGitHub {
    owner = "openssl";
    repo = "openssl";
    rev = "refs/tags/openssl-3.1.4";
    sha256 = "sha256-Vvf1wiNb4ikg1lIS9U137aodZ2JzM711tSWMJFYWtWI=";
  };

  nativeBuildInputs = with pkgs; [ perl ];

  buildInputs = with pkgs; [ ];

  postPatch = ''
    patchShebangs Configure
  '';

  configurePhase = ''
   ./Configure no-shared
  '';
  installPhase = ''
    make install DESTDIR=$out
  '';
}
