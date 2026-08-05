#!/usr/bin/env bash
# Stable cross-implementation transport workload matrix.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
export GO_BENCH="${GO_BENCH:-$HERE/quicbench-go/quicmsgbench-go}"
export QUINN_BENCH="${QUINN_BENCH:-$HERE/quicbench-rs/target/release/quicmsgbench-rs}"
# Default: every implementation whose binary is built. Set IMPLEMENTATIONS to override.
if [ -z "${IMPLEMENTATIONS:-}" ]; then
  IMPLEMENTATIONS=ton
  [ -x "$GO_BENCH" ] && IMPLEMENTATIONS="$IMPLEMENTATIONS quic-go" || echo "quic-go driver not built, skipping ($GO_BENCH)"
  [ -x "$QUINN_BENCH" ] && IMPLEMENTATIONS="$IMPLEMENTATIONS quinn" || echo "quinn driver not built, skipping ($QUINN_BENCH)"
fi
PROTOCOLS="${PROTOCOLS:-quic adnl rldp2}"
DURATION="${DURATION:-20}"
WARMUP="${WARMUP:-5}"
FANIN_RATE="${FANIN_RATE:-1000}"
DENSE_RATE="${DENSE_RATE:-6000}"
REPETITIONS="${REPETITIONS:-1}"
FAILED=0
RUNS=0

contains() { case " $1 " in *" $2 "*) return 0 ;; *) return 1 ;; esac; }

for implementation in $IMPLEMENTATIONS; do
  case "$implementation" in
    ton|quic-go|quinn) ;;
    *) echo "unknown implementation: $implementation" >&2; exit 2 ;;
  esac
done
for protocol in $PROTOCOLS; do
  case "$protocol" in
    quic|adnl|rldp2) ;;
    *) echo "unknown protocol: $protocol" >&2; exit 2 ;;
  esac
done
if ! contains "$IMPLEMENTATIONS" ton && \
   { contains "$PROTOCOLS" adnl || contains "$PROTOCOLS" rldp2; }; then
  echo "ADNL and RLDP2 require IMPLEMENTATIONS to include ton" >&2
  exit 2
fi
if ! contains "$PROTOCOLS" quic && \
   { contains "$IMPLEMENTATIONS" quic-go || contains "$IMPLEMENTATIONS" quinn; }; then
  echo "quic-go and quinn require PROTOCOLS to include quic" >&2
  exit 2
fi

run() {
  local scenario="$1" implementation="$2" protocol="$3"; shift 3
  local label="$scenario-$implementation-$protocol"
  local out="$ROOT/$label"
  RUNS=$((RUNS + 1))
  echo "######## $scenario / $implementation / $protocol"
  if "$HERE/bench.sh" -I "$implementation" -P "$protocol" -w "$WARMUP" -d "$DURATION" \
      -l "$label" -o "$out" "$@"; then
    :
  else
    FAILED=1
    echo "  run failed; artifacts: $out"
  fi
  if [ -f "$out/summary.tsv" ]; then
    if [ ! -f "$SUMMARY" ]; then head -1 "$out/summary.tsv" > "$SUMMARY"; fi
    tail -1 "$out/summary.tsv" >> "$SUMMARY"
  fi
  echo
}

run_selected() {
  local scenario="$1" adnl_supported="$2"; shift 2
  if contains "$PROTOCOLS" quic; then
    for implementation in $IMPLEMENTATIONS; do
      run "$scenario" "$implementation" quic "$@"
    done
  fi
  if contains "$IMPLEMENTATIONS" ton; then
    if [ "$adnl_supported" -eq 1 ] && contains "$PROTOCOLS" adnl; then
      run "$scenario" ton adnl "$@"
    fi
    if contains "$PROTOCOLS" rldp2; then
      run "$scenario" ton rldp2 "$@"
    fi
  fi
}

[ "$REPETITIONS" -ge 1 ] || { echo "REPETITIONS must be positive" >&2; exit 2; }
if [ -n "${OUTDIR:-}" ]; then
  ROOT="$OUTDIR"
  mkdir -p "$ROOT" || exit 2
  [ -z "$(ls -A "$ROOT")" ] || { echo "output directory is not empty: $ROOT" >&2; exit 2; }
else
  ROOT=$(mktemp -d /tmp/ton-transport-suite.XXXXXX)
fi
SUMMARY="$ROOT/summary.tsv"

for repetition in $(seq 1 "$REPETITIONS"); do
  SUFFIX=""
  if [ "$REPETITIONS" -gt 1 ]; then SUFFIX="-r$repetition"; fi

  # Scenarios are interleaved by implementation to reduce thermal and background-load bias.
  run_selected "idle-small$SUFFIX" 1 -T idle -c 32 -s 240
  run_selected "ihave-1x20$SUFFIX" 1 -T message -c 1 -n 20 -s 240 -m 4000 -B 20
  run_selected "ihave-40x20$SUFFIX" 1 -T message -c 1 -n 20 -s 240 -m "$DENSE_RATE" -B 800
  run_selected "bulk-common$SUFFIX" 1 -T query -c 2 -i 2 -s 7680 -r 1024
  run_selected "bulk-large$SUFFIX" 0 -T query -c 2 -i 2 -s 64 -r 262144
  run_selected "fanin$SUFFIX" 1 -T message -c 8 -s 240 -m "$FANIN_RATE" -B 1
  run_selected "sat-1x1$SUFFIX" 1 -T saturate -c 1 -n 1 -s 240 -B 100
  run_selected "sat-20x400$SUFFIX" 1 -T saturate -c 1 -n 20 -s 240 -B 400
done

[ "$RUNS" -ne 0 ] || { echo "selection has no supported implementation/transport rows" >&2; exit 2; }

echo "######## summary (medians of $REPETITIONS repetition(s))"
if [ -f "$SUMMARY" ]; then
  python3 "$HERE/summarize.py" "$SUMMARY" || { echo "summarize failed; raw table:"; cat "$SUMMARY"; }
fi
echo "full data: $SUMMARY"
echo "artifacts: $ROOT"
exit "$FAILED"
