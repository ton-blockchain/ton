#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/../../.." >/dev/null && pwd)"
BUILD_DIR="$SOURCE_DIR/build"
INSTALL_PREFIX="$BUILD_DIR/install"
CONSUMER_BUILD_DIR="$BUILD_DIR/test/tonlib/integration"

if [ -d "$CONSUMER_BUILD_DIR" ]; then
  rm -rf "$CONSUMER_BUILD_DIR"
fi
mkdir -p "$CONSUMER_BUILD_DIR"

cleanup() {
  rm -rf "$CONSUMER_BUILD_DIR"
}
trap cleanup EXIT

echo "=== Building consumer project ==="
cmake -S "$SCRIPT_DIR" -B "$CONSUMER_BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$INSTALL_PREFIX"

cmake --build "$CONSUMER_BUILD_DIR" -j

echo "=== Running consumer tests ==="
"$CONSUMER_BUILD_DIR/tonlibjson_consumer"
echo "tonlibjson_consumer: OK"

echo "=== Integration test passed ==="
