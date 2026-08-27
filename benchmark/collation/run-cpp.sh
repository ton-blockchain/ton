#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
build_dir=$source_root/cmake-build-bench

binary=${COLLATION_BENCH_BINARY:-}
corpus=${COLLATION_BENCH_CORPUS:-}
iterations=${COLLATION_BENCH_ITERATIONS:-15}
warmup=${COLLATION_BENCH_WARMUP:-1}
threads=${COLLATION_BENCH_THREADS:-1}
accounts=${COLLATION_BENCH_ACCOUNTS:-1000000}
transfers=${COLLATION_BENCH_TRANSFERS:-128}
workload=${COLLATION_BENCH_WORKLOAD:-jetton}
parallel=1
verbose=0

usage() {
  cat <<'EOF'
Usage: benchmark/collation/run-cpp.sh [options]

With no options: builds the benchmark if needed, generates the default corpus
on first use (one-time, several minutes), then measures collation,
collated-data validation, and preloaded-state validation in three separate
processes and prints one summary line per series.

Options (all optional):
  --corpus DIR           corpus directory (default: cmake-build-bench/collation-corpus-<accounts>x<transfers>; generated if missing)
  --binary PATH          benchmark executable (default: built in cmake-build-bench)
  --iterations N         measured repetitions per process (default 15)
  --warmup N             unmeasured repetitions per process (default 1)
  --threads N            actor scheduler threads (default 1)
  --accounts N           corpus accounts when generating (default 1000000)
  --transfers N          corpus transfers when generating (default 128)
  --workload W           jetton (4 tx/transfer, default) or transfer (plain wallet-to-wallet, 2 tx/transfer);
                         selects the corpus to generate/use, measured runs read it back from the corpus
  --parallel N           N scheduler threads + parallel account validation
                         (default 1, the single-core headline; collation stays serial)
  --verbose              per-iteration samples and per-stage attribution
  -h, --help             show this help
EOF
}

while (($#)); do
  case "$1" in
    --corpus)
      (($# >= 2)) || { echo "--corpus requires DIR" >&2; exit 2; }
      corpus=$2
      shift 2
      ;;
    --binary)
      (($# >= 2)) || { echo "--binary requires PATH" >&2; exit 2; }
      binary=$2
      shift 2
      ;;
    --iterations)
      (($# >= 2)) || { echo "--iterations requires N" >&2; exit 2; }
      iterations=$2
      shift 2
      ;;
    --warmup)
      (($# >= 2)) || { echo "--warmup requires N" >&2; exit 2; }
      warmup=$2
      shift 2
      ;;
    --threads)
      (($# >= 2)) || { echo "--threads requires N" >&2; exit 2; }
      threads=$2
      shift 2
      ;;
    --accounts)
      (($# >= 2)) || { echo "--accounts requires N" >&2; exit 2; }
      accounts=$2
      shift 2
      ;;
    --transfers)
      (($# >= 2)) || { echo "--transfers requires N" >&2; exit 2; }
      transfers=$2
      shift 2
      ;;
    --workload)
      (($# >= 2)) || { echo "--workload requires jetton|transfer" >&2; exit 2; }
      workload=$2
      shift 2
      ;;
    --parallel)
      (($# >= 2)) || { echo "--parallel requires N" >&2; exit 2; }
      parallel=$2
      shift 2
      ;;
    --verbose)
      verbose=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ "$iterations" =~ ^[1-9][0-9]*$ ]] || { echo "iterations must be positive" >&2; exit 2; }
[[ "$warmup" =~ ^[0-9]+$ ]] || { echo "warmup must be non-negative" >&2; exit 2; }
[[ "$threads" =~ ^[1-9][0-9]*$ ]] || { echo "threads must be positive" >&2; exit 2; }
[[ "$parallel" =~ ^[1-9][0-9]*$ ]] || { echo "parallel must be positive" >&2; exit 2; }
[[ "$workload" == "jetton" || "$workload" == "transfer" ]] || { echo "workload must be jetton or transfer" >&2; exit 2; }

parallel_validation_flag=()
if ((parallel > 1)); then
  threads=$parallel
  parallel_validation_flag=(--parallel-validation)
fi

if [[ -n "$binary" ]]; then
  [[ -x "$binary" ]] || { echo "benchmark binary is not executable: $binary" >&2; exit 2; }
else
  binary=$build_dir/benchmark/collation/collation-validation-bench
  if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    echo "configuring $build_dir (one-time)..."
    cmake -S "$source_root" -B "$build_dir" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG'
  fi
  # Incremental build on every run so edited sources are never silently
  # benchmarked stale; a no-change build returns in seconds.
  cmake --build "$build_dir" --target collation-validation-bench -j "$(getconf _NPROCESSORS_ONLN)"
fi

corpus_suffix=
[[ "$workload" == "jetton" ]] || corpus_suffix="-$workload"
[[ -n "$corpus" ]] || corpus=$build_dir/collation-corpus${corpus_suffix}-${accounts}x${transfers}
if [[ ! -d "$corpus" ]]; then
  echo "generating corpus at $corpus (one-time, several minutes at ${accounts}x${transfers})..."
  "$binary" \
    --corpus-out "$corpus" \
    --mode validate \
    --workload "$workload" \
    --accounts "$accounts" \
    --transfers "$transfers" \
    --warmup 0 \
    --iterations 1 \
    --threads "$threads"
fi

verbose_flag=()
if ((verbose)); then
  verbose_flag=(--verbose)
fi

results=()
block_tx=

run_series() {
  local label=$1
  shift
  local out mean
  out=$("$binary" \
    --corpus-in "$corpus" \
    "$@" \
    --warmup "$warmup" \
    --iterations "$iterations" \
    --threads "$threads" \
    ${verbose_flag[@]+"${verbose_flag[@]}"})
  printf '%s\n' "$out"
  # Arithmetic mean: the same statistic as Go's ns/op, so the two wrapper
  # tables are directly comparable. Median/MAD stay in the summary line above.
  mean=$(printf '%s\n' "$out" | sed -n 's/.*phase=summary.*[^_]mean_ms=\([0-9.]*\).*/\1/p' | head -n 1)
  [[ -n "$mean" ]] || { echo "no summary line for $label" >&2; exit 1; }
  if [[ -z "$block_tx" ]]; then
    block_tx=$(printf '%s\n' "$out" | sed -n 's/.*phase=config.* block_transactions=\([0-9]*\).*/\1/p' | head -n 1)
  fi
  results+=("$label|$mean")
}

run_series "collate" --mode collate
run_series "validate (collated)" --mode validate --validation-input collated \
  ${parallel_validation_flag[@]+"${parallel_validation_flag[@]}"}
run_series "validate (preloaded)" --mode validate --validation-input preloaded \
  ${parallel_validation_flag[@]+"${parallel_validation_flag[@]}"}

parallel_note=
((parallel == 1)) || parallel_note=", threads=$parallel"
echo
echo "results (mean of $iterations iterations, $block_tx transactions per block$parallel_note):"
for entry in "${results[@]}"; do
  awk -F'|' -v tx="$block_tx" \
    '{ printf "  %-22s %9.2f ms/block  %8.1f us/tx  %7.0f tx/s\n", $1, $2, $2 * 1000 / tx, tx * 1000 / $2 }' \
    <<<"$entry"
done
