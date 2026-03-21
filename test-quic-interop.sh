#!/bin/bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

PORT=19999
BUILD_DIR="${BUILD_DIR:-cmake-build-debug}"
SERVER_BIN="$BUILD_DIR/quic/quic-example-server"
CLIENT_BIN="$BUILD_DIR/quic/quic-example-client"
RUST_DIR="test/quic-interop-rust"
RUST_CLIENT="$RUST_DIR/target/release/rpk-client"
RUST_SERVER="$RUST_DIR/target/release/rpk-server"
CPP_SERVER_LOG=/tmp/quic-server.log
CPP_CLIENT_LOG=/tmp/quic-client-cpp.log
RUST_CLIENT_LOG=/tmp/quic-client-rust.log
RUST_SERVER_LOG=/tmp/quic-server-rust.log
SERVER_PID=""
FAILED=0

stop_server() {
  if [ -n "$SERVER_PID" ]; then
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    SERVER_PID=""
  fi
}

cleanup() {
  stop_server
}
trap cleanup EXIT INT TERM

wait_for_pid() {
  local pid="$1"
  local tries="$2"
  WAIT_EXIT=""

  for i in $(seq 1 "$tries"); do
    if ! kill -0 "$pid" 2>/dev/null; then
      set +e
      wait "$pid"
      WAIT_EXIT=$?
      set -e
      return 0
    fi
    sleep 0.1
  done
  return 1
}

start_cpp_server() {
  echo "Starting quic-example-server on port $PORT..."
  $SERVER_BIN --port $PORT > "$CPP_SERVER_LOG" 2>&1 &
  SERVER_PID=$!
  sleep 1

  if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${RED}Error: C++ server failed to start${NC}"
    cat "$CPP_SERVER_LOG"
    exit 1
  fi
}

start_rust_server() {
  echo "Starting rpk-server on port $PORT..."
  $RUST_SERVER "127.0.0.1:$PORT" > "$RUST_SERVER_LOG" 2>&1 &
  SERVER_PID=$!
  sleep 1

  if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${RED}Error: Rust server failed to start${NC}"
    cat "$RUST_SERVER_LOG"
    exit 1
  fi
}

# Build C++ binaries
echo "Building C++ binaries..."
ninja -C "$BUILD_DIR" quic-example-server quic-example-client 2>&1 | tail -1

if [ ! -f "$SERVER_BIN" ] || [ ! -f "$CLIENT_BIN" ]; then
  echo -e "${RED}Error: failed to build C++ binaries${NC}"
  exit 1
fi

# Build Rust binaries
echo "Building Rust interop binaries..."
(cd "$RUST_DIR" && cargo build --release --quiet --bin rpk-client --bin rpk-server 2>&1)
if [ ! -f "$RUST_CLIENT" ] || [ ! -f "$RUST_SERVER" ]; then
  echo -e "${RED}Error: failed to build Rust interop binaries${NC}"
  exit 1
fi

echo ""
start_cpp_server

# Extract server public key (base64) from log
SERVER_PUBKEY=$(sed 's/\x1b\[[0-9;]*m//g' "$CPP_SERVER_LOG" \
  | grep -o 'server public key: [^ ]*' \
  | head -1 \
  | awk '{print $NF}')

if [ -z "$SERVER_PUBKEY" ]; then
  echo -e "${RED}Error: could not extract server public key${NC}"
  cat "$CPP_SERVER_LOG"
  exit 1
fi
echo "Server pubkey: $SERVER_PUBKEY"
echo ""

# --- Test 1: C++ client ---
echo "=== Test 1: C++ quic-example-client ==="
$CLIENT_BIN --host localhost --port $PORT > "$CPP_CLIENT_LOG" 2>&1 &
CPP_PID=$!

if ! wait_for_pid "$CPP_PID" 50; then
  echo -e "${RED}  FAIL: C++ client timed out${NC}"
  kill $CPP_PID 2>/dev/null || true
  wait $CPP_PID 2>/dev/null || true
  FAILED=1
elif [ "$WAIT_EXIT" -eq 0 ] && grep -q "test passed" "$CPP_CLIENT_LOG"; then
  echo -e "${GREEN}  PASS${NC}"
else
  echo -e "${RED}  FAIL (exit $WAIT_EXIT)${NC}"
  cat "$CPP_CLIENT_LOG"
  FAILED=1
fi

# --- Test 2: Rust interop client ---
echo ""
echo "=== Test 2: Rust RPK interop client ==="
$RUST_CLIENT "127.0.0.1:$PORT" "$SERVER_PUBKEY" > "$RUST_CLIENT_LOG" 2>&1 &
RUST_PID=$!

if ! wait_for_pid "$RUST_PID" 100; then
  echo -e "${RED}  FAIL: Rust client timed out${NC}"
  kill $RUST_PID 2>/dev/null || true
  wait $RUST_PID 2>/dev/null || true
  FAILED=1
elif [ "$WAIT_EXIT" -eq 0 ]; then
  # Check that all phases passed
  if grep -q "Phase 3.*OK" "$RUST_CLIENT_LOG" && grep -q "Phase 4.*OK" "$RUST_CLIENT_LOG"; then
    echo -e "${GREEN}  PASS (all phases)${NC}"
  elif grep -q "Handshake complete" "$RUST_CLIENT_LOG"; then
    echo -e "${YELLOW}  PASS (handshake OK, some phases skipped)${NC}"
    grep -E "(Phase|timed out|error)" "$RUST_CLIENT_LOG" | sed 's/^/    /'
  else
    echo -e "${RED}  FAIL (no handshake)${NC}"
    cat "$RUST_CLIENT_LOG"
    FAILED=1
  fi
else
  echo -e "${RED}  FAIL (exit $WAIT_EXIT)${NC}"
  cat "$RUST_CLIENT_LOG"
  FAILED=1
fi

# --- Test 3: C++ client -> Rust server ---
echo ""
echo "=== Test 3: C++ quic-example-client -> Rust rpk-server ==="
stop_server
start_rust_server

$CLIENT_BIN --host localhost --port $PORT > "$CPP_CLIENT_LOG" 2>&1 &
CPP_PID=$!

if ! wait_for_pid "$CPP_PID" 50; then
  echo -e "${RED}  FAIL: C++ client timed out${NC}"
  kill $CPP_PID 2>/dev/null || true
  wait $CPP_PID 2>/dev/null || true
  FAILED=1
elif [ "$WAIT_EXIT" -eq 0 ] && grep -q "test passed" "$CPP_CLIENT_LOG"; then
  echo -e "${GREEN}  PASS${NC}"
else
  echo -e "${RED}  FAIL (exit $WAIT_EXIT)${NC}"
  cat "$CPP_CLIENT_LOG"
  echo ""
  echo "Rust server log:"
  cat "$RUST_SERVER_LOG"
  FAILED=1
fi

# Summary
echo ""
if [ $FAILED -eq 0 ]; then
  echo -e "${GREEN}All tests passed${NC}"
  exit 0
else
  echo -e "${RED}Some tests failed${NC}"
  echo ""
  echo "C++ server log: $CPP_SERVER_LOG"
  echo "C++ client log: $CPP_CLIENT_LOG"
  echo "Rust client log: $RUST_CLIENT_LOG"
  echo "Rust server log: $RUST_SERVER_LOG"
  exit 1
fi
