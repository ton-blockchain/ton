#!/bin/bash
ARCH="x86" ./build.sh || exit 1
ARCH="x86_64" ./build.sh || exit 1
ARCH="arm" ./build.sh || exit 1
ARCH="arm64" ./build.sh || exit 1
