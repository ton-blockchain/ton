#/bin/bash

nix-build --version
test $? -eq 0 || { echo "Nix is not installed!"; exit 1; }

cp assembly/nix/macos-* .
export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz
nix-build macos-static.nix
mkdir artifacts
cp ./result-bin/bin/* artifacts/
chmod +x artifacts/*
rm -rf result-bin
nix-build macos-tonlib.nix
cp ./result/lib/libtonlibjson.dylib artifacts/
cp ./result/lib/libemulator.dylib artifacts/
cp -r crypto/fift/lib artifacts/
cp -r crypto/smartcont artifacts/