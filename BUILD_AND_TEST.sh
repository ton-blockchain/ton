#!/bin/bash

# Build and test gettx tool
#set -e

# Ensure we're in the git repository root
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Initialize submodules and build directory on first run
if [[ ! -d "build" ]]; then
  echo "=== Initializing git submodules ===" >> /tmp/gettx_test_output.txt
  git submodule update --init --recursive >> /tmp/gettx_test_output.txt 2>&1
  if [[ "$?" != "0" ]]; then
    echo "*** Git submodule initialization failed"
    exit 1
  fi
  echo "=== Creating build directory ===" >> /tmp/gettx_test_output.txt
  mkdir build
fi

cd build

# Run cmake configuration if needed
if [[ ! -f "CMakeCache.txt" ]]; then
  echo "=== Running cmake configuration ===" >> /tmp/gettx_test_output.txt
  cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 >> /tmp/gettx_test_output.txt 2>&1
  if [[ "$?" != "0" ]]; then
    tail --l 30 /tmp/gettx_test_output.txt
    echo "*** Cmake configuration failed, details available at /tmp/gettx_test_output.txt"
    exit 1
  fi
fi

echo "=== Building gettx ===" > /tmp/gettx_test_output.txt
cmake --build . --target gettx -j8 >> /tmp/gettx_test_output.txt 2>&1
if [[ "$?" != "0" ]]; then
  tail --l 30 /tmp/gettx_test_output.txt
  echo "*** Building gettx failed, details available at /tmp/gettx_test_output.txt"
  exit 1
fi

echo "" >> /tmp/gettx_test_output.txt
echo "=== Running gettx test ===" >> /tmp/gettx_test_output.txt
# Testing with transaction from TRANSACTION_EXAMPLES.txt:
# Transaction: FYLEbPNbsXAiXZNjVSRERJ0/zybCh+Z3JTbH97K/gAg=
# lt: 2000001, workchain: -1
# address: Ef8zMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzM0vF (base64)
./tools/gettx/gettx tx \
  --workchain -1 \
  --address Ef80UXx731GHxVr0-LYf3DIViMerdo3uJLAG3ykQZFjXz2kW \
  --lt 2000001 \
  --hash 69XTCXdLrEOtYJB76hrAi5rx2fJ80kFl8MwefrLjNYU= \
  --count 1 \
  --db-path ../db \
  --format json >> /tmp/gettx_test_output.txt 2>&1
if [[ "$?" != "0" ]]; then
  tail --l 30 /tmp/gettx_test_output.txt
  echo "*** Running gettx tx failed, details available at /tmp/gettx_test_output.txt"
  exit 1
fi
tail --l 30 /tmp/gettx_test_output.txt
echo "*** Running gettx tx test finished successfully."

echo "" >> /tmp/gettx_test_output.txt
echo "=== Testing gettx block subcommand ===" >> /tmp/gettx_test_output.txt
# Test block retrieval - get masterchain block 2 and all its shard transactions
./tools/gettx/gettx block \
  --seqno 2 \
  --db-path ../db >> /tmp/gettx_test_output.txt 2>&1
if [[ "$?" != "0" ]]; then
  tail --l 30 /tmp/gettx_test_output.txt
  echo "*** Running gettx block failed, details available at /tmp/gettx_test_output.txt"
  exit 1
fi
tail --l 30 /tmp/gettx_test_output.txt
echo "*** Running gettx block test finished successfully."
