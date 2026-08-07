#!/usr/bin/env bash
# Run one transport workload with separate client and server processes.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)

usage() {
  cat <<'USAGE'
Usage: bench.sh [options]

  -T TYPE     query | message | saturate | idle            (default query)
  -I NAME     ton | quic-go | quinn                        (default ton)
  -P NAME     quic | rldp2 | adnl                          (default quic)
  -c N        client/generator processes                   (default 8)
  -C N        connections per generator                    (default 1)
  -n N        servers; N > 1 is one-client fan-out         (default 1)
  -i N        queries in flight per client                 (default 8)
  -s BYTES    request or message size                      (default 240)
  -r BYTES    response size; query defaults to request, otherwise 1
  -m RATE     aggregate messages/s per client              (default 4000; saturate 1000000)
  -B N        messages per timer event                     (default 1)
  -e SIDE     measured side: sender | receiver             (default from topology)
  -w SEC      warm-up excluded from measurements           (default 5)
  -d SEC      measured duration                            (default 20)
  -b PATH     implementation binary
  -l LABEL    result label
  -o DIR      empty output directory (default: mktemp)

Environment: BENCH, GO_BENCH, QUINN_BENCH, EXTRA, THREADS (default 1),
             NATIVE_TUNED (default 1), CONNECT_INFLIGHT (default 64),
             CONNECT_RATE (default 2000/s), READY_TIMEOUT (default 60 seconds),
             CC (default cubic; TON QUIC also supports bbr and reno).
USAGE
}

BENCH="${BENCH:-cmake-build-relwithdebinfo/bench-rldp}"
GO_BENCH="${GO_BENCH:-test/net/bench/quicbench-go/quicmsgbench-go}"
QUINN_BENCH="${QUINN_BENCH:-test/net/bench/quicbench-rs/target/release/quicmsgbench-rs}"
read -r -a EXTRA_ARGS <<< "${EXTRA:-}"
THREADS="${THREADS:-1}"
NATIVE_TUNED="${NATIVE_TUNED:-1}"
CONNECT_INFLIGHT="${CONNECT_INFLIGHT:-64}"
CONNECT_RATE="${CONNECT_RATE:-2000}"
READY_TIMEOUT="${READY_TIMEOUT:-60}"
CC="${CC:-cubic}"
TYPE=query
IMPLEMENTATION=ton
PROTO=quic
CLIENTS=8
CLIENTS_SET=0
CONNECTIONS=1
CONNECTIONS_SET=0
SERVERS=1
INFLIGHT=8
SIZE=240
RESPONSE_SIZE=""
RATE=4000
RATE_SET=0
BURST=1
WARMUP=5
DURATION=20
LABEL=""
OUTDIR=""
MEASURE_SIDE=""
BINARY_OVERRIDE=""

while getopts "T:I:P:c:C:n:i:s:r:m:B:e:w:d:b:l:o:h" opt; do
  case "$opt" in
    T) TYPE="$OPTARG" ;;
    I) IMPLEMENTATION="$OPTARG" ;;
    P) PROTO="$OPTARG" ;;
    c) CLIENTS="$OPTARG"; CLIENTS_SET=1 ;;
    C) CONNECTIONS="$OPTARG"; CONNECTIONS_SET=1 ;;
    n) SERVERS="$OPTARG" ;;
    i) INFLIGHT="$OPTARG" ;;
    s) SIZE="$OPTARG" ;;
    r) RESPONSE_SIZE="$OPTARG" ;;
    m) RATE="$OPTARG"; RATE_SET=1 ;;
    B) BURST="$OPTARG" ;;
    e) MEASURE_SIDE="$OPTARG" ;;
    w) WARMUP="$OPTARG" ;;
    d) DURATION="$OPTARG" ;;
    b) BINARY_OVERRIDE="$OPTARG" ;;
    l) LABEL="$OPTARG" ;;
    o) OUTDIR="$OPTARG" ;;
    h) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

case "$TYPE" in query|message|saturate|idle) ;; *) echo "unknown workload: $TYPE" >&2; exit 2 ;; esac
case "$IMPLEMENTATION" in ton|quic-go|quinn) ;; *) echo "unknown implementation: $IMPLEMENTATION" >&2; exit 2 ;; esac
case "$PROTO" in quic|rldp2|adnl) ;; *) echo "unknown protocol: $PROTO" >&2; exit 2 ;; esac
if [ "$IMPLEMENTATION" != ton ] && [ "$PROTO" != quic ]; then
  echo "$IMPLEMENTATION only implements QUIC" >&2
  exit 2
fi
if [ "$IMPLEMENTATION" = ton ] && { [ "$CLIENTS" -gt 1000 ] || [ "$SERVERS" -gt 1000 ]; }; then
  echo "TON permits at most 1000 client or server processes per run" >&2
  exit 2
fi
case "$NATIVE_TUNED" in 0|1) ;; *) echo "NATIVE_TUNED must be 0 or 1" >&2; exit 2 ;; esac
case "$CC" in cubic|bbr|reno) ;; *) echo "CC must be cubic, bbr, or reno" >&2; exit 2 ;; esac
if [ "$IMPLEMENTATION" != ton ] && [ "$CC" != cubic ]; then
  echo "$IMPLEMENTATION benchmark only exposes its CUBIC controller" >&2
  exit 2
fi
case "$IMPLEMENTATION" in
  ton) RUNNER="$BENCH" ;;
  quic-go) RUNNER="$GO_BENCH" ;;
  quinn) RUNNER="$QUINN_BENCH" ;;
esac
if [ -n "${BINARY_OVERRIDE:-}" ]; then RUNNER="$BINARY_OVERRIDE"; fi
[ -x "$RUNNER" ] || { echo "benchmark not executable: $RUNNER" >&2; exit 2; }
case "$RUNNER" in
  /*) ;;
  *) RUNNER="$(cd "$(dirname "$RUNNER")" && pwd)/$(basename "$RUNNER")" ;;
esac
[ "$CLIENTS" -ge 1 ] && [ "$CONNECTIONS" -ge 1 ] && [ "$SERVERS" -ge 1 ] || {
  echo "client, connection and server counts must be positive" >&2
  exit 2
}
if [ -z "$RESPONSE_SIZE" ]; then
  if [ "$TYPE" = query ]; then RESPONSE_SIZE=$SIZE; else RESPONSE_SIZE=1; fi
fi

if [ "$SERVERS" -gt 1 ]; then
  if [ "$CLIENTS_SET" -eq 1 ] && [ "$CLIENTS" -ne 1 ]; then
    echo "fan-out (-n > 1) uses exactly one client" >&2
    exit 2
  fi
  CLIENTS=1
  if [ "$CONNECTIONS_SET" -eq 1 ] && [ "$CONNECTIONS" -ne "$SERVERS" ]; then
    echo "fan-out needs one connection per destination: -C must equal -n" >&2
    exit 2
  fi
  CONNECTIONS=$SERVERS
  SIDE=sender
  TOPOLOGY="fanout-${SERVERS}"
  TOPOLOGY_PEERS=$SERVERS
elif [ "$TYPE" = idle ]; then
  SIDE=receiver
  TOPOLOGY_PEERS=$((CLIENTS * CONNECTIONS))
  TOPOLOGY="idle-${TOPOLOGY_PEERS}"
else
  SIDE=receiver
  TOPOLOGY_PEERS=$((CLIENTS * CONNECTIONS))
  TOPOLOGY="fanin-${TOPOLOGY_PEERS}"
fi
if [ -n "$MEASURE_SIDE" ]; then SIDE="$MEASURE_SIDE"; fi
case "$SIDE" in sender|receiver) ;; *) echo "measured side must be sender or receiver" >&2; exit 2 ;; esac
if [ "$SIDE" = sender ] && [ "$CLIENTS" -ne 1 ]; then
  echo "sender-side measurement needs exactly one client exporter" >&2
  exit 2
fi

case "$TYPE" in
  query)
    MODE_ARGS=(-n 100000000 -c "$INFLIGHT")
    LOAD="${INFLIGHT} in flight/client"
    ;;
  message)
    [ "$RATE" -gt 0 ] && [ "$BURST" -gt 0 ] || { echo "message rate and burst must be positive" >&2; exit 2; }
    MODE_ARGS=(--messages --rate "$RATE" --message-burst "$BURST")
    LOAD="${RATE} msg/s/client, bursts of ${BURST}"
    ;;
  saturate)
    # The message workload offered far above any ceiling: delivered rate is the result and
    # pacing attainment is expected to be low, so the report skips that validity rule.
    [ "$RATE_SET" -eq 1 ] || RATE=1000000
    [ "$RATE" -gt 0 ] && [ "$BURST" -gt 0 ] || { echo "saturate rate and burst must be positive" >&2; exit 2; }
    MODE_ARGS=(--messages --rate "$RATE" --message-burst "$BURST")
    LOAD="ceiling probe at ${RATE} msg/s offered, bursts of ${BURST}"
    ;;
  idle)
    [ "$SERVERS" -eq 1 ] || { echo "idle currently measures many clients against one server" >&2; exit 2; }
    # A completed round trip proves that the peer is usable before it is declared idle.
    MODE_ARGS=(-n 0 --stay-alive)
    LOAD="${TOPOLOGY_PEERS} established idle peers"
    ;;
esac

DRIVER_TYPE=$TYPE
[ "$TYPE" = saturate ] && DRIVER_TYPE=message
LABEL="${LABEL:-$IMPLEMENTATION-$PROTO-$TYPE-$TOPOLOGY-${SIZE}x${RESPONSE_SIZE}b}"
if [ -z "$OUTDIR" ]; then
  mkdir -p /tmp/ton-bench
  OUTDIR=$(mktemp -d "/tmp/ton-bench/${LABEL}.XXXXXX")
else
  mkdir -p "$OUTDIR"
  [ -z "$(ls -A "$OUTDIR")" ] || { echo "output directory is not empty: $OUTDIR" >&2; exit 2; }
fi
OUTDIR=$(cd "$OUTDIR" && pwd)

REPO_ROOT=$(cd "$HERE/../../.." && pwd)
GIT_REVISION=$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || printf unknown)
if ! git -C "$REPO_ROOT" diff --quiet || ! git -C "$REPO_ROOT" diff --cached --quiet; then
  GIT_REVISION="$GIT_REVISION-dirty"
fi
BINARY_SHA256=$(shasum -a 256 "$RUNNER" 2>/dev/null | awk '{print $1}')
[ -n "$BINARY_SHA256" ] || BINARY_SHA256=unknown
RECORDED_CC=$CC
if [ "$PROTO" != quic ]; then RECORDED_CC=NA; fi
RECORDED_TUNED=$NATIVE_TUNED
if [ "$IMPLEMENTATION" = ton ]; then RECORDED_TUNED=NA; fi
{
  printf 'field\tvalue\n'
  printf 'timestamp_utc\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'git_revision\t%s\n' "$GIT_REVISION"
  printf 'binary\t%s\n' "$RUNNER"
  printf 'binary_sha256\t%s\n' "$BINARY_SHA256"
  printf 'implementation\t%s\nprotocol\t%s\nworkload\t%s\ntopology\t%s\n' \
    "$IMPLEMENTATION" "$PROTO" "$TYPE" "$TOPOLOGY"
  printf 'generators\t%s\nconnections_per_generator\t%s\nservers\t%s\n' \
    "$CLIENTS" "$CONNECTIONS" "$SERVERS"
  printf 'request_bytes\t%s\nresponse_bytes\t%s\nrate_per_generator\t%s\nburst\t%s\ninflight\t%s\n' \
    "$SIZE" "$RESPONSE_SIZE" "$RATE" "$BURST" "$INFLIGHT"
  printf 'threads\t%s\nwarmup_seconds\t%s\nduration_seconds\t%s\n' "$THREADS" "$WARMUP" "$DURATION"
  printf 'native_tuned\t%s\ncongestion_control\t%s\nconnect_inflight\t%s\nconnect_rate\t%s\nextra\t%s\n' \
    "$RECORDED_TUNED" "$RECORDED_CC" "$CONNECT_INFLIGHT" "$CONNECT_RATE" "${EXTRA:-}"
  printf 'host\t%s\n' "$(uname -a)"
} > "$OUTDIR/manifest.tsv"

HASH=$(printf '%s' "$LABEL" | cksum | cut -d' ' -f1)
SPORT=$((20000 + HASH % 3000))
CPORT=$((25000 + HASH % 1000))
SPROM=$((10000 + HASH % 1000))
CPROM=$((12000 + HASH % 1000))
# Setup is outside the measured window but inside the driver's own deadline, and a client holding
# many sessions needs time to establish them all.
HARD_TIMEOUT=$(awk -v w="$WARMUP" -v d="$DURATION" -v r="$READY_TIMEOUT" 'BEGIN {print w + d + r + 120}')
[ $((SPORT + SERVERS - 1 + 1000)) -lt 25000 ] && [ $((CPORT + CLIENTS + 1000)) -lt 32768 ] || {
  echo "too many processes for the UDP port ranges" >&2
  exit 2
}
[ $((SPROM + SERVERS - 1)) -lt 12000 ] && [ $((CPROM + CLIENTS - 1)) -lt 20000 ] || {
  echo "too many processes for the exporter port ranges" >&2
  exit 2
}

PIDS=()
SERVER_PIDS=()
CLIENT_PIDS=()
cleanup() {
  [ "${#PIDS[@]}" -eq 0 ] || kill "${PIDS[@]}" 2>/dev/null || true
  sleep 0.2
  [ "${#PIDS[@]}" -eq 0 ] || kill -9 "${PIDS[@]}" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

scrape_group() {
  local base="$1" count="$2" output="$3" body ok
  : > "$output"
  for i in $(seq 0 $((count - 1))); do
    ok=0
    for ((attempt = 1; attempt <= 50; ++attempt)); do
      if body=$(curl -sf --max-time 5 "http://127.0.0.1:$((base + i))/metrics"); then
        printf '%s\n' "$body" >> "$output"
        ok=1
        break
      fi
      [ "$attempt" -eq 50 ] || sleep 0.2
    done
    if [ "$ok" -ne 1 ]; then
      echo "metrics scrape failed: 127.0.0.1:$((base + i))" >&2
      return 1
    fi
  done
  return 0
}
timed_scrape() {
  SCRAPE_BEFORE=$(now)
  scrape_group "$@"
  SCRAPE_AFTER=$(now)
}
driver_errors() {
  [ "$IMPLEMENTATION" = ton ] || { echo 0; return; }
  grep -hE 'Query [0-9]+( failed:|: bad response)' "$OUTDIR"/client-*.log 2>/dev/null | wc -l | tr -d ' '
}
cpu_of() {
  local pids="$1"
  if [ -r /proc/self/stat ]; then
    local ticks total=0 pid stat fields
    ticks=$(getconf CLK_TCK)
    for pid in ${pids//,/ }; do
      [ -r "/proc/$pid/stat" ] || continue
      stat=$(<"/proc/$pid/stat")
      fields=${stat##*) }
      set -- $fields
      total=$((total + ${12} + ${13}))
    done
    awk -v total="$total" -v ticks="$ticks" 'BEGIN {printf "%.6f\n", total/ticks}'
    return
  fi
  ps -o time= -p "$1" 2>/dev/null | awk -F'[:-]' '
    {n=NF; s=0; m=1; for(i=n;i>=1;i--){s += $i*m; m=(m==1)?60:((m==60)?3600:86400)} total+=s}
    END {printf "%.2f\n", total+0}'
}
rss_of() { ps -o rss= -p "$1" 2>/dev/null | awk '{s += $1} END {print s+0}'; }
now() { python3 -c 'import time; print(time.monotonic())'; }
alive() { for pid in "$@"; do kill -0 "$pid" 2>/dev/null || return 1; done; }
metric_sum() {
  local file="$1" family="$2"
  awk -v family="$family" '$1 == family || index($1, family "{") == 1 {sum += $NF} END {printf "%.0f\n", sum+0}' "$file"
}

echo "== $LABEL: $IMPLEMENTATION $PROTO $TYPE, $TOPOLOGY, $LOAD, ${SIZE}B/${RESPONSE_SIZE}B"

NATIVE_TUNING=()
if [ "$NATIVE_TUNED" -eq 1 ]; then NATIVE_TUNING=(--tuned); fi

for i in $(seq 0 $((SERVERS - 1))); do
  if [ "$IMPLEMENTATION" = ton ]; then
    (cd "$OUTDIR" && exec "$RUNNER" --server "--$PROTO" --no-limits --server-id "$i" \
      --prometheus-port "$((SPROM + i))" -a "127.0.0.1:$((SPORT + i))" \
      -q "$SIZE" -r "$RESPONSE_SIZE" -t "$THREADS" --cc "$CC" \
      ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}) \
      > "$OUTDIR/server-$i.log" 2>&1 &
  else
    (cd "$OUTDIR" && exec "$RUNNER" --server --addr "127.0.0.1:$((SPORT + i))" \
      --stats-addr "127.0.0.1:$((SPROM + i))" --workload "$DRIVER_TYPE" --size "$SIZE" \
      --response-size "$RESPONSE_SIZE" --threads "$THREADS" \
      ${NATIVE_TUNING[@]+"${NATIVE_TUNING[@]}"} \
      ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}) > "$OUTDIR/server-$i.log" 2>&1 &
  fi
  SERVER_PIDS+=("$!"); PIDS+=("$!")
done
sleep 2
alive "${SERVER_PIDS[@]}" || { echo "a server failed to start" >&2; tail -20 "$OUTDIR"/server-*.log; exit 1; }
scrape_group "$SPROM" "$SERVERS" /dev/null || { echo "a server exporter did not start" >&2; exit 1; }
SPIDS=$(IFS=,; echo "${SERVER_PIDS[*]}")
SERVER_RSS_BASE=$(rss_of "$SPIDS")

for i in $(seq 1 "$CLIENTS"); do
  if [ "$IMPLEMENTATION" = ton ]; then
    (cd "$OUTDIR" && exec "$RUNNER" --client "--$PROTO" --no-limits --client-id "$i" \
      --prometheus-port "$((CPROM + i - 1))" -a "127.0.0.1:$((CPORT + i))" \
      -s "127.0.0.1:$SPORT" --servers "$SERVERS" --connections "$CONNECTIONS" \
      --connect-inflight "$CONNECT_INFLIGHT" \
      -q "$SIZE" -r "$RESPONSE_SIZE" -t "$THREADS" \
      --timeout 60 --test-timeout "$HARD_TIMEOUT" --cc "$CC" "${MODE_ARGS[@]}" \
      ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}) \
      > "$OUTDIR/client-$i.log" 2>&1 &
  else
    (cd "$OUTDIR" && exec "$RUNNER" --client --addr "127.0.0.1:$((CPORT + i))" \
      --stats-addr "127.0.0.1:$((CPROM + i - 1))" --server-addr "127.0.0.1:$SPORT" \
      --servers "$SERVERS" --connections "$CONNECTIONS" --workload "$DRIVER_TYPE" --size "$SIZE" \
      --response-size "$RESPONSE_SIZE" --rate "$RATE" --burst "$BURST" --inflight "$INFLIGHT" \
      --connect-inflight "$CONNECT_INFLIGHT" --connect-rate "$CONNECT_RATE" \
      --threads "$THREADS" ${NATIVE_TUNING[@]+"${NATIVE_TUNING[@]}"} \
      ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}) \
      > "$OUTDIR/client-$i.log" 2>&1 &
  fi
  CLIENT_PIDS+=("$!"); PIDS+=("$!")
done
sleep 2
alive "${CLIENT_PIDS[@]}" || { echo "a client failed to start" >&2; tail -20 "$OUTDIR"/client-*.log; exit 1; }
scrape_group "$CPROM" "$CLIENTS" /dev/null || { echo "a client exporter did not start" >&2; exit 1; }
READY=0
EXPECTED_READY=$CLIENTS
if [ "$IMPLEMENTATION" != ton ]; then EXPECTED_READY=$TOPOLOGY_PEERS; fi
for _ in $(seq 1 $((READY_TIMEOUT * 2))); do
  if [ "$IMPLEMENTATION" = ton ]; then
    READY=$(grep -l "Ready: 1" "$OUTDIR"/client-*.log 2>/dev/null | wc -l | tr -d ' ')
    [ "$READY" -eq "$CLIENTS" ] && break
  elif scrape_group "$CPROM" "$CLIENTS" "$OUTDIR/readiness-client.txt" && \
       scrape_group "$SPROM" "$SERVERS" "$OUTDIR/readiness-server.txt"; then
    CLIENT_READY=$(metric_sum "$OUTDIR/readiness-client.txt" bench_connections_current)
    CLIENT_APPLICATION_READY=$(metric_sum "$OUTDIR/readiness-client.txt" bench_connections_ready)
    SERVER_READY=$(metric_sum "$OUTDIR/readiness-server.txt" bench_connections_current)
    if [ "$CLIENT_READY" -eq "$EXPECTED_READY" ] && \
       [ "$CLIENT_APPLICATION_READY" -eq "$EXPECTED_READY" ] && \
       [ "$SERVER_READY" -eq "$EXPECTED_READY" ]; then
      READY=$EXPECTED_READY
      break
    fi
  fi
  alive "${CLIENT_PIDS[@]}" || break
  sleep 0.5
done
[ "$READY" -eq "$EXPECTED_READY" ] || {
  echo "only $READY/$EXPECTED_READY clients/connections became ready" >&2
  tail -20 "$OUTDIR"/client-*.log
  exit 1
}

sleep "$WARMUP"
DRIVER_ERRORS0=$(driver_errors)
if [ "$SIDE" = sender ]; then
  timed_scrape "$CPROM" "$CLIENTS" "$OUTDIR/metrics-start.txt"
  MSTART0=$SCRAPE_BEFORE; MSTART1=$SCRAPE_AFTER
  timed_scrape "$SPROM" "$SERVERS" "$OUTDIR/peer-metrics-start.txt"
  PSTART0=$SCRAPE_BEFORE; PSTART1=$SCRAPE_AFTER
else
  timed_scrape "$SPROM" "$SERVERS" "$OUTDIR/metrics-start.txt"
  MSTART0=$SCRAPE_BEFORE; MSTART1=$SCRAPE_AFTER
  timed_scrape "$CPROM" "$CLIENTS" "$OUTDIR/peer-metrics-start.txt"
  PSTART0=$SCRAPE_BEFORE; PSTART1=$SCRAPE_AFTER
fi
CPIDS=$(IFS=,; echo "${CLIENT_PIDS[*]}")
SCPU0=$(cpu_of "$SPIDS"); CCPU0=$(cpu_of "$CPIDS")
SRSS0=$(rss_of "$SPIDS"); CRSS0=$(rss_of "$CPIDS")
T0=$(now)

sleep "$DURATION"
T1=$(now)
SCPU1=$(cpu_of "$SPIDS"); CCPU1=$(cpu_of "$CPIDS")
SRSS1=$(rss_of "$SPIDS"); CRSS1=$(rss_of "$CPIDS")
if [ "$SIDE" = sender ]; then
  timed_scrape "$CPROM" "$CLIENTS" "$OUTDIR/metrics-end.txt"
  MEND0=$SCRAPE_BEFORE; MEND1=$SCRAPE_AFTER
  timed_scrape "$SPROM" "$SERVERS" "$OUTDIR/peer-metrics-end.txt"
  PEND0=$SCRAPE_BEFORE; PEND1=$SCRAPE_AFTER
else
  timed_scrape "$SPROM" "$SERVERS" "$OUTDIR/metrics-end.txt"
  MEND0=$SCRAPE_BEFORE; MEND1=$SCRAPE_AFTER
  timed_scrape "$CPROM" "$CLIENTS" "$OUTDIR/peer-metrics-end.txt"
  PEND0=$SCRAPE_BEFORE; PEND1=$SCRAPE_AFTER
fi
DRIVER_ERRORS1=$(driver_errors)
alive "${PIDS[@]}" || { echo "a benchmark process exited during measurement" >&2; exit 1; }
DRIVER_ERRORS=$((DRIVER_ERRORS1 - DRIVER_ERRORS0))

if [ "$SIDE" = sender ]; then
  U0=$CCPU0; U1=$CCPU1; R0=$CRSS0; R1=$CRSS1; RB=$CRSS0; P0=$SCPU0; P1=$SCPU1
  PR0=$SRSS0; PR1=$SRSS1
else
  U0=$SCPU0; U1=$SCPU1; R0=$SRSS0; R1=$SRSS1; RB=$SERVER_RSS_BASE; P0=$CCPU0; P1=$CCPU1
  PR0=$CRSS0; PR1=$CRSS1
fi
ELAPSED=$(awk -v a="$T1" -v b="$T0" 'BEGIN {print a-b}')
METRICS_ELAPSED=$(awk -v a="$MSTART0" -v b="$MSTART1" -v c="$MEND0" -v d="$MEND1" \
  'BEGIN {print (c+d-a-b)/2}')
PEER_METRICS_ELAPSED=$(awk -v a="$PSTART0" -v b="$PSTART1" -v c="$PEND0" -v d="$PEND1" \
  'BEGIN {print (c+d-a-b)/2}')
REPORT_STATUS=0
REPORT_TUNED=$NATIVE_TUNED
if [ "$IMPLEMENTATION" = ton ]; then REPORT_TUNED=NA; fi
REPORT_CC=$CC
if [ "$PROTO" != quic ]; then REPORT_CC=NA; fi
python3 "$(dirname "$0")/report.py" --dir "$OUTDIR" --implementation "$IMPLEMENTATION" \
  --protocol "$PROTO" --workload "$TYPE" \
  --topology "$TOPOLOGY" --side "$SIDE" --peers "$TOPOLOGY_PEERS" \
  --clients "$CLIENTS" --connections "$CONNECTIONS" --size "$SIZE" --response-size "$RESPONSE_SIZE" \
  --rate "$RATE" --burst "$BURST" --inflight "$INFLIGHT" --threads "$THREADS" --tuned "$REPORT_TUNED" \
  --congestion-control "$REPORT_CC" \
  --warmup "$WARMUP" --git-revision "$GIT_REVISION" --binary-sha256 "$BINARY_SHA256" \
  --elapsed "$ELAPSED" --metrics-elapsed "$METRICS_ELAPSED" --peer-metrics-elapsed "$PEER_METRICS_ELAPSED" \
  --cpu-start "$U0" --cpu-end "$U1" \
  --peer-cpu-start "$P0" --peer-cpu-end "$P1" --peer-rss-start "$PR0" --peer-rss-end "$PR1" \
  --rss-baseline "$RB" --rss-start "$R0" --rss-end "$R1" \
  --driver-errors "$DRIVER_ERRORS" --label "$LABEL" || REPORT_STATUS=$?
echo "  artifacts    $OUTDIR"
exit "$REPORT_STATUS"
