#!/bin/bash
# RLDP benchmark - start server once, run benchmarks
set -e
cd "$(dirname "$0")/../.."

BENCH="${BUILD_DIR:-cmake-build-relwithdebinfo}/bench-rldp"
trap 'pkill -f "bench-rldp.*--server" 2>/dev/null' EXIT

"$BENCH" --server -v 1 &
sleep 1

"$BENCH" --client -n 50 -v 1 2>&1 | grep -E "complete|Queries|Time|QPS|Throughput|Latency|min:|avg:|p50:|p90:|p99:|max:"
