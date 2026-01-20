#!/bin/bash
echo "[build-all.sh] script=$0"
echo "[build-all.sh] cwd=$(pwd)"
echo "[build-all.sh] ANDROID_NDK_ROOT=$ANDROID_NDK_ROOT"

#echo "[build-all.sh] Building tonlib for x86..."
#echo
#ARCH="x86" ./build.sh || exit 1

echo "[build-all.sh] Building tonlib for x86_64..."
echo
ARCH="x86_64" ./build.sh || exit 1
#
#echo "[build-all.sh] Building tonlib for arm..."
#echo
#ARCH="arm" ./build.sh || exit 1
#
#echo "[build-all.sh] Building tonlib for arm64..."
#echo
#ARCH="arm64" ./build.sh || exit 1
