#!/usr/bin/env bash
# Run a command inside a disposable network namespace whose loopback carries an emulated RTT.
# Only the namespace's own lo is shaped; host interfaces are never touched.
#
#   rtt.sh [-r RTT_MS] command...
#   rtt.sh -r 50 env PROTOCOLS=quic ./suite.sh
set -eu

RTT_MS=50
while getopts "r:" opt; do
  case "$opt" in
    r) RTT_MS="$OPTARG" ;;
    *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || { echo "usage: rtt.sh [-r rtt-ms] command..." >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "rtt.sh needs root for ip netns" >&2; exit 2; }
[ "$RTT_MS" -ge 2 ] || { echo "rtt must be at least 2ms" >&2; exit 2; }

NS="tonbench-rtt-$$"
ip netns add "$NS"
trap 'ip netns del "$NS" 2>/dev/null' EXIT
ip netns exec "$NS" ip link set lo up
ip netns exec "$NS" tc qdisc add dev lo root netem delay "$((RTT_MS / 2))ms" limit 100000

MEASURED=$(ip netns exec "$NS" ping -c 3 -q 127.0.0.1 | awk -F/ '/rtt|round-trip/ {print $5}')
awk -v m="$MEASURED" -v t="$RTT_MS" 'BEGIN { exit (m > t * 0.8 && m < t * 1.2) ? 0 : 1 }' || {
  echo "measured RTT ${MEASURED}ms is not within 20% of ${RTT_MS}ms" >&2
  exit 1
}
echo "netns $NS: loopback RTT ${MEASURED}ms"
ip netns exec "$NS" "$@"
