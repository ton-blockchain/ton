#!/bin/bash
echo ANDROID_NDK_ROOT = $ANDROID_NDK_ROOT

echo Building tonlib for x86...
echo
ARCH="x86" ./build.sh || exit 1

echo Building tonlib for x86_64...
echo
ARCH="x86_64" ./build.sh || exit 1

echo Building tonlib for arm...
echo
ARCH="arm" ./build.sh || exit 1

echo Building tonlib for arm64...
echo
ARCH="arm64" ./build.sh || exit 1
