#/bin/bash

nix-build --version
test $? -eq 0 || { echo "Nix is not installed!"; exit 1; }

with_tests=false


while getopts 't' flag; do
  case "${flag}" in
    t) with_tests=true ;;
    *) break
       ;;
  esac
done

cp assembly/nix/linux-x86-64* .
cp assembly/nix/microhttpd.nix .
cp assembly/nix/openssl.nix .
export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz

if [ "$with_tests" = true ]; then
  nix-build linux-x86-64-static.nix --arg testing true
else
  nix-build linux-x86-64-static.nix
fi

mkdir artifacts
cp ./result/bin/* artifacts/
chmod +x artifacts/*
rm -rf result
nix-build linux-x86-64-tonlib.nix
cp ./result/lib/libtonlibjson.so.0.5 artifacts/libtonlibjson.so
cp ./result/lib/libemulator.so artifacts/
cp -r crypto/fift/lib artifacts/
cp -r crypto/smartcont artifacts/
