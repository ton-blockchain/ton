# Collation and validation benchmark

`collation-validation-bench` measures the production `Collator` and
`ValidateQuery` over deterministic basechain workloads, entirely in memory:
all predecessor and masterchain state, validator context, and ordered external
messages are loaded before timing, the in-memory manager never starts its
database actor, and timed callbacks never touch files or the network. It is
not a validator-engine, database, or networking benchmark.

## Quick start

```sh
./benchmark/collation/run-cpp.sh   # C++ reference
./benchmark/collation/run-go.sh    # gton (Go) on the same corpus
```

Both are zero-config. `run-cpp.sh` (re)builds the benchmark incrementally,
generates the default corpus in `cmake-build-bench/` on first use (one-time,
several minutes), measures collation, collated-data validation, and
preloaded-state validation in three separate processes, and ends with a table
of arithmetic means (ms/block, us/tx, tx/s). `run-go.sh` clones xssnick/gton's
`validator` branch into a sibling `gton-validator` checkout if missing,
fetches it on each run, installs the vendored test harness into a temporary
copy of that revision (never touching the checkout or corpus), runs a
correctness gate plus the same three series, and prints the same table —
converted from Go's `ns/op`, which is also an arithmetic mean. It needs a Go
toolchain (gton pins `go 1.26.0`; any recent `go` fetches it via
`GOTOOLCHAIN=auto`). If gton's internals drift, the vendored harness fails to
compile loudly — adjust `gton/bench_disk_corpus_test.go` or pin `--gton-ref`.

Everything else is opt-in (`--help`):

- `--workload W` — `jetton` (default): Wallet V5 jetton transfers, four
  transactions per transfer; `transfer`: plain non-bounceable W5 -> W5 TON
  transfers, two transactions per transfer. Both build the identical
  1M-account state and differ only in the signed externals; the transfer
  corpus gets a `-transfer` name suffix. The flag only selects which corpus is
  generated — measured runs read the workload back from `meta.json`, and the
  per-block transaction count in the tables adjusts automatically.
- `--parallel N` — multi-core profile: C++ runs N scheduler threads with
  parallel account validation (its collation stays serial); Go runs with
  `GOMAXPROCS=N`, enabling gton's parallel collation and lane-parallel
  validation. Default 1 — the single-core CPU-efficiency headline.
- `--binary PATH` (run-cpp.sh) — benchmark another tree's executable on the
  same corpus for A/B runs; alternate the launch order across rounds, since
  outer fresh-process repetition is the control for machine drift.
- `--verbose` (run-cpp.sh) — per-iteration samples and per-stage attribution.
- `--accounts/--transfers/--corpus/--iterations/--warmup`, and
  `--gton-dir/--gton-ref/--output-dir` on the Go side.

## Modes

The binary measures two independent profiles, each with a direct Go
counterpart: `--mode collate` (default) runs one production collation query
per iteration; `--mode validate` runs one candidate validation per iteration,
against the full-collated-data candidate (`--validation-input collated`,
default) or the proof-stripped resident-state sidecar (`preloaded`). Warmups
are mode-specific. The wall clock around `run_collate_query` /
`run_validate_query` is the headline; per-stage real/CPU timings are
attribution only and may overlap — do not sum them to reconstruct wall time.

Manual build (the wrapper does this automatically; Release only — the binary
refuses debug builds, and the target is excluded from default builds/CTest):

```sh
cmake -S . -B cmake-build-bench \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG'
cmake --build cmake-build-bench --target collation-validation-bench
```

To only generate a corpus (the `--corpus-out` path must not exist yet —
fixture construction, the masterchain seqno-1 bootstrap, golden collation,
validation, and proof stripping all happen outside measured samples):

```sh
./cmake-build-bench/benchmark/collation/collation-validation-bench \
  --corpus-out "$CORPUS" --accounts 1000000 --transfers 128 \
  --warmup 0 --iterations 1
```

## CPU profiles

On macOS, wrap a resident, high-iteration process with Time Profiler; use a
fresh output path per trace and unprofiled processes for timing:

```sh
xcrun xctrace record --template 'Time Profiler' \
  --output /private/tmp/ton-cpp-collate.trace --target-stdout - --launch -- \
  ./cmake-build-bench/benchmark/collation/collation-validation-bench \
  --corpus-in "$CORPUS" --mode collate --warmup 5 --iterations 100
```

## Corpus contract

Schema `ton-collation-validation-fixture/v1`; `manifest.sha256` is exactly
`<lowercase SHA256 of the exact meta.json bytes>  meta.json`. Fixed layout:

```text
meta.json  manifest.sha256
states/shard-prev.boc  states/masterchain.boc  blocks/masterchain.boc
externals/0000.boc ...
candidate/block.boc  candidate/collated-full.boc  candidate/collated-preloaded.boc
```

V1 fixes a full wc0 shard, one seqno-0 predecessor, no split/merge, no
internal messages, no neighbors or top blocks, an empty out queue, and full
collated data. Every artifact records byte size, SHA256, and per-occurrence
root hashes; each file is deserialized independently so equal hashes never
become a global hash-to-cell association. Fixture preparation canonicalizes
raw full-shard `ShardIdent` prefixes, stages ConfigParam 19 in the
configuration smart contract, and advances the masterchain through a real
key-block transition, so a fresh gton tracker can bootstrap from the corpus
alone (global ID `-777`).

## Foreign candidate reverse acceptance

`--candidate-in DIR` (with `--corpus-in`, `--mode validate`) reads exactly
`DIR/block.boc` and `DIR/collated.boc` — e.g. the candidate `run-go.sh
--output-dir` exports — and validates the foreign block with the production
validator. The importer derives the block id and creator from the raw
artifacts and requires the corpus shard, predecessor, masterchain reference,
timestamps, random seed, creator, successor state root, transaction count,
and gas; it never compares the foreign bytes to the C++ golden candidate.
Loading and preflight are outside timing. This is the secondary
interoperability check — the primary comparison is both validators consuming
the same C++ candidate from the shared corpus.

## Output

Machine-readable lines begin with `COLLATION-VALIDATION-BENCH`; setup phases
are marked `measured=0`. By default only setup context and per-series
summaries (mean, median, MAD, p90, min) are printed; `--verbose` adds
per-iteration samples and per-stage attribution. Resident memory growth and
allocator caching persist across inner iterations by design; alternating
fresh-process rounds are the control for process history and machine drift.
