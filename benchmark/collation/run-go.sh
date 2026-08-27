#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
TON_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)
DEFAULT_GTON_REF=origin/validator
DEFAULT_GTON_REPO=https://github.com/xssnick/gton
DEFAULT_GTON_DIR=$(cd -- "$TON_ROOT/.." && pwd -P)/gton-validator
HARNESS="$SCRIPT_DIR/gton/bench_disk_corpus_test.go"

corpus_dir=${COLLATION_BENCH_CORPUS:-}
accounts=${COLLATION_BENCH_ACCOUNTS:-1000000}
transfers=${COLLATION_BENCH_TRANSFERS:-128}
workload=${COLLATION_BENCH_WORKLOAD:-jetton}
gton_dir=$DEFAULT_GTON_DIR
gton_ref=$DEFAULT_GTON_REF
iterations=15
parallel=1
output_dir=
temp_checkout=

usage() {
  cat <<'EOF'
Usage: benchmark/collation/run-go.sh [options]

Run gton's shared-corpus correctness gate and collation/validation benchmarks
without modifying the source checkout. With no options it uses the same
default corpus as run-cpp.sh (generating it if missing) and the head of gton's
validator branch (cloning and fetching the checkout automatically).

Options (all optional):
  --corpus DIR       Strict v1 shared disk corpus (default: the run-cpp.sh corpus)
  --workload W       jetton (4 tx/transfer, default) or transfer (plain wallet-to-wallet, 2 tx/transfer);
                     only selects which corpus is generated, the harness reads the workload from its manifest
  --gton-dir DIR     Local xssnick/gton checkout (default: sibling gton-validator,
                     cloned automatically if missing)
  --gton-ref REF     Commit/ref to archive (default: origin/validator, fetched on
                     each run; pass a commit hash to pin a revision)
  --iterations N     Measured repetitions per benchmark process (default: 15)
  --parallel N       Run with GOMAXPROCS=N (default: 1, the single-core headline)
  --output-dir DIR   Fresh result directory for logs and candidate/
  -h, --help         Show this help

The runner archives the selected gton commit into a temporary directory,
installs only the vendored _test.go harness there, and removes that temporary
checkout on exit. It never writes to the caller's gton checkout or corpus.
EOF
}

die() {
  echo "run-go: $*" >&2
  exit 1
}

need_value() {
  [[ $# -ge 2 && -n "$2" ]] || die "$1 requires a value"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
  --corpus)
    need_value "$@"
    corpus_dir=$2
    shift 2
    ;;
  --workload)
    need_value "$@"
    workload=$2
    shift 2
    ;;
  --gton-dir)
    need_value "$@"
    gton_dir=$2
    shift 2
    ;;
  --gton-ref)
    need_value "$@"
    gton_ref=$2
    shift 2
    ;;
  --iterations)
    need_value "$@"
    iterations=$2
    shift 2
    ;;
  --parallel)
    need_value "$@"
    parallel=$2
    shift 2
    ;;
  --output-dir)
    need_value "$@"
    output_dir=$2
    shift 2
    ;;
  -h | --help)
    usage
    exit 0
    ;;
  *)
    die "unknown argument: $1"
    ;;
  esac
done

[[ "$workload" == "jetton" || "$workload" == "transfer" ]] || die "--workload must be jetton or transfer"
corpus_suffix=
[[ "$workload" == "jetton" ]] || corpus_suffix="-$workload"
if [[ -z "$corpus_dir" ]]; then
  corpus_dir=$TON_ROOT/cmake-build-bench/collation-corpus${corpus_suffix}-${accounts}x${transfers}
  if [[ ! -d "$corpus_dir" ]]; then
    cpp_binary=$TON_ROOT/cmake-build-bench/benchmark/collation/collation-validation-bench
    [[ -x "$cpp_binary" ]] ||
      die "default corpus is missing; run benchmark/collation/run-cpp.sh once or pass --corpus DIR"
    echo "run-go: generating corpus at $corpus_dir (one-time, several minutes at ${accounts}x${transfers})..."
    "$cpp_binary" --corpus-out "$corpus_dir" --mode validate --workload "$workload" \
      --accounts "$accounts" --transfers "$transfers" --warmup 0 --iterations 1
  fi
fi
[[ "$iterations" =~ ^[1-9][0-9]*$ ]] || die "--iterations must be a positive integer"
[[ "$parallel" =~ ^[1-9][0-9]*$ ]] || die "--parallel must be a positive integer"
[[ -f "$HARNESS" ]] || die "vendored harness is missing: $HARNESS"

for command_name in git go tar mktemp; do
  command -v "$command_name" >/dev/null 2>&1 || die "$command_name is not installed"
done

[[ -d "$corpus_dir" ]] || die "corpus directory does not exist: $corpus_dir"
corpus_dir=$(cd -- "$corpus_dir" && pwd -P)
[[ -f "$corpus_dir/meta.json" && -f "$corpus_dir/manifest.sha256" ]] ||
  die "corpus lacks meta.json or manifest.sha256: $corpus_dir"

if [[ ! -d "$gton_dir" && "$gton_dir" == "$DEFAULT_GTON_DIR" ]]; then
  echo "run-go: cloning $DEFAULT_GTON_REPO (validator branch) into $gton_dir (one-time)..."
  git clone --branch validator "$DEFAULT_GTON_REPO" "$gton_dir"
fi
[[ -d "$gton_dir" ]] || die "gton checkout does not exist: $gton_dir"
gton_dir=$(cd -- "$gton_dir" && pwd -P)
git -C "$gton_dir" rev-parse --git-dir >/dev/null 2>&1 || die "not a gton git checkout: $gton_dir"
if [[ "$gton_ref" == "$DEFAULT_GTON_REF" ]]; then
  git -C "$gton_dir" fetch --quiet origin validator ||
    echo "run-go: fetch failed, using the locally known origin/validator" >&2
fi
gton_commit=$(git -C "$gton_dir" rev-parse --verify "${gton_ref}^{commit}") ||
  die "gton ref is unavailable locally: $gton_ref"
module_line=$(git -C "$gton_dir" show "$gton_commit:go.mod" 2>/dev/null | sed -n '1p')
[[ "$module_line" == "module github.com/xssnick/gton" ]] ||
  die "selected ref is not the xssnick/gton module: $gton_commit"

if [[ -n "$output_dir" ]]; then
  output_parent=$(dirname -- "$output_dir")
  output_name=$(basename -- "$output_dir")
  [[ -d "$output_parent" ]] || die "output parent does not exist: $output_parent"
  output_parent=$(cd -- "$output_parent" && pwd -P)
  output_dir="$output_parent/$output_name"
  [[ ! -e "$output_dir" ]] || die "output directory already exists: $output_dir"
  case "$output_dir/" in
  "$corpus_dir"/*)
    die "output directory must be outside the corpus: $output_dir"
    ;;
  esac
  mkdir -- "$output_dir"
  candidate_dir="$output_dir/candidate"
  # Provenance comes from the corpus manifest, not the CLI flag: --workload
  # only selects which corpus to generate, while --corpus may point anywhere.
  corpus_workload=$(sed -n 's/.*"workload":{"name":"\([^"]*\)".*/\1/p' "$corpus_dir/meta.json" | head -n 1)
  {
    echo "corpus=$corpus_dir"
    echo "workload=${corpus_workload:-unknown}"
    echo "gton_source=$gton_dir"
    echo "gton_ref=$gton_ref"
    echo "gton_commit=$gton_commit"
    echo "iterations=$iterations"
    echo "parallel=$parallel"
  } >"$output_dir/run-info.txt"
fi

cleanup() {
  if [[ -n "$temp_checkout" && -d "$temp_checkout" ]]; then
    rm -rf -- "$temp_checkout"
  fi
}
trap cleanup EXIT HUP INT TERM

temp_checkout=$(mktemp -d "${TMPDIR:-/tmp}/gton-collation-validation-go.XXXXXX")
git -C "$gton_dir" archive --format=tar "$gton_commit" | tar -xf - -C "$temp_checkout"

target_harness="$temp_checkout/service/validator/collator/bench_disk_corpus_test.go"
[[ ! -e "$target_harness" ]] || die "selected gton ref already contains the disk-corpus harness"
cp -- "$HARNESS" "$target_harness"

results=()

run_stage() {
  local stage=$1
  shift

  echo "run-go: stage=$stage"
  local out status=0
  out=$( (cd -- "$temp_checkout" && "$@") 2>&1 ) || status=$?
  printf '%s\n' "$out"
  if [[ -n "$output_dir" ]]; then
    printf '%s\n' "$out" >"$output_dir/$stage.log"
  fi
  ((status == 0)) || exit "$status"
  local bench=
  bench=$(printf '%s\n' "$out" | grep -m1 '^Benchmark' || true)
  if [[ -n "$bench" ]]; then
    results+=("$stage|$bench")
  fi
}

common_env=(
  env
  GOMAXPROCS="$parallel"
  GTON_BENCH_CORPUS_DIR="$corpus_dir"
)
if ((parallel > 1)); then
  common_env+=(GTON_BENCH_ALLOW_PARALLEL=1)
fi
cross_env=("${common_env[@]}")
if [[ -n "$output_dir" ]]; then
  cross_env+=(GTON_BENCH_CANDIDATE_OUT_DIR="$candidate_dir")
fi

echo "run-go: corpus=$corpus_dir"
echo "run-go: gton_commit=$gton_commit"
echo "run-go: iterations=$iterations"
if [[ -n "$output_dir" ]]; then
  echo "run-go: output_dir=$output_dir"
fi

run_stage cross-acceptance \
  "${cross_env[@]}" go test ./service/validator/collator \
  -run '^TestDiskCorpusCrossAcceptance$' -count=1 -cpu="$parallel" -v

run_stage collate \
  "${common_env[@]}" go test ./service/validator/collator \
  -run '^$' -bench '^BenchmarkCollateDiskCorpus$' \
  -benchtime="${iterations}x" -benchmem -count=1 -cpu="$parallel"

run_stage validate-collated \
  "${common_env[@]}" go test ./service/validator/collator \
  -run '^$' -bench '^BenchmarkValidateDiskCorpus/collated$' \
  -benchtime="${iterations}x" -benchmem -count=1 -cpu="$parallel"

run_stage validate-preloaded \
  "${common_env[@]}" go test ./service/validator/collator \
  -run '^$' -bench '^BenchmarkValidateDiskCorpus/preloaded$' \
  -benchtime="${iterations}x" -benchmem -count=1 -cpu="$parallel"

echo "run-go: complete"
if [[ -n "$output_dir" ]]; then
  echo "run-go: logs=$output_dir"
  echo "run-go: reverse_candidate=$candidate_dir"
fi

if ((${#results[@]})); then
  block_tx=$(printf '%s\n' "${results[0]#*|}" |
    awk '{ for (i = 2; i <= NF; ++i) if ($i == "tx/block") print $(i - 1) }')
  echo
  parallel_note=
  ((parallel == 1)) || parallel_note=", GOMAXPROCS=$parallel"
  echo "results (go average of $iterations iterations, ${block_tx%.*} transactions per block$parallel_note):"
  for entry in ${results[@]+"${results[@]}"}; do
    stage=${entry%%|*}
    case "$stage" in
    collate) label="collate" ;;
    validate-collated) label="validate (collated)" ;;
    validate-preloaded) label="validate (preloaded)" ;;
    *) label=$stage ;;
    esac
    printf '%s\n' "${entry#*|}" | awk -v label="$label" '
      {
        ns = 0
        tx = 0
        for (i = 2; i <= NF; ++i) {
          if ($i == "ns/op") ns = $(i - 1)
          if ($i == "tx/block") tx = $(i - 1)
        }
        if (ns > 0 && tx > 0) {
          printf "  %-22s %9.2f ms/block  %8.1f us/tx  %7.0f tx/s\n", label, ns / 1e6, ns / 1e3 / tx, tx * 1e9 / ns
        }
      }'
  done
fi
