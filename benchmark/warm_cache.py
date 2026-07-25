#!/usr/bin/env python3
"""Seed the OS page cache with a random subset of a RocksDB celldb.

`drop_caches` gives the pessimistic fully-cold extreme; a real validator runs
with a warm working set — the hot top of the accounts dictionary plus a random
scatter of leaves it has touched recently. This script approximates the
"scatter" half: it reads a uniformly random subset of the celldb SST extents
into the page cache, sized to fill (most of) free RAM. Run it *after*
drop_caches and *before* starting the node; the node's own reads then warm the
genuinely-hot pages on top, and the kernel evicts the coldest of our random
pages (clean, reclaimable) as the validator grows its RSS — which is itself the
realistic behaviour.

It is a subset, not a working-set model: pages are picked uniformly at random,
so the dictionary's always-hot upper levels are only as represented as their
byte share. That is fine here — those levels get cached within seconds of the
run regardless; what this reproduces is a partially-populated cold tail.

Usage:
  # fill ~90% of MemAvailable with a random subset of the celldb
  python benchmark/warm_cache.py /mnt/bench/state-full-b5/celldb

  # fill a fixed amount, reproducibly
  python benchmark/warm_cache.py /mnt/bench/state-full-b5 --gib 80 --seed 1

A --manifest path or a state dir containing `celldb/` are both accepted.
"""

from __future__ import annotations

import argparse
import os
import random
import sys
import time
from concurrent.futures import ThreadPoolExecutor


def parse_size(s: str) -> int:
    s = s.strip().upper()
    mult = 1
    for suffix, m in (("K", 1 << 10), ("M", 1 << 20), ("G", 1 << 30)):
        if s.endswith(suffix):
            mult = m
            s = s[:-1]
            break
        if s.endswith(suffix + "IB"):
            mult = m
            s = s[: -len(suffix) - 2]
            break
    return int(float(s) * mult)


def meminfo_kib(key: str) -> int:
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith(key + ":"):
                return int(line.split()[1])
    return 0


def resolve_celldb(path: str) -> str:
    """Accept a celldb dir, a state dir containing celldb/, or a manifest.json."""
    if os.path.isfile(path):
        path = os.path.dirname(path)
    if os.path.isdir(os.path.join(path, "celldb")):
        path = os.path.join(path, "celldb")
    if not os.path.isdir(path):
        sys.exit(f"not a directory: {path}")
    return path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    _ = ap.add_argument("celldb", help="celldb dir, state dir, or manifest.json")
    g = ap.add_mutually_exclusive_group()
    _ = g.add_argument("--gib", type=float, help="exact amount to warm, in GiB")
    _ = g.add_argument(
        "--fraction", type=float, default=0.9, help="fraction of MemAvailable to warm (default 0.9)"
    )
    _ = ap.add_argument("--extent", type=parse_size, default=2 << 20, help="read granularity (default 2M)")
    _ = ap.add_argument("--seed", type=int, default=0, help="RNG seed for a reproducible subset (default 0)")
    _ = ap.add_argument("--threads", type=int, default=16, help="concurrent readers (default 16)")
    _ = ap.add_argument(
        "--include-nonsst", action="store_true", help="also warm the WAL/.log, not just .sst files"
    )
    args = ap.parse_args()

    celldb = resolve_celldb(args.celldb)

    files: list[tuple[str, int]] = []
    for name in sorted(os.listdir(celldb)):
        if not args.include_nonsst and not name.endswith(".sst"):
            continue
        p = os.path.join(celldb, name)
        if not os.path.isfile(p):
            continue
        sz = os.path.getsize(p)
        if sz >= args.extent:
            files.append((p, sz))
    if not files:
        sys.exit(f"no warmable files found in {celldb}")

    db_bytes = sum(sz for _, sz in files)
    extent = args.extent

    if args.gib is not None:
        target = int(args.gib * (1 << 30))
    else:
        target = int(meminfo_kib("MemAvailable") * 1024 * args.fraction)
    target = min(target, db_bytes)

    # Global list of extent-sized chunks across all files; a random prefix is our
    # subset. dedup-free by construction, so coverage is exact.
    chunks: list[tuple[str, int]] = []
    for p, sz in files:
        for off in range(0, sz - extent + 1, extent):
            chunks.append((p, off))
    rng = random.Random(args.seed)
    rng.shuffle(chunks)
    n_sel = min(len(chunks), target // extent)
    selected = chunks[:n_sel]

    print(
        f"celldb {celldb}: {len(files)} files, {db_bytes / (1<<30):.1f} GiB; "
        f"MemAvailable {meminfo_kib('MemAvailable')/(1<<20):.1f} GiB",
        flush=True,
    )
    print(
        f"warming {n_sel} x {extent>>20} MiB = {n_sel*extent/(1<<30):.1f} GiB "
        f"({100*n_sel*extent/db_bytes:.0f}% of db) random subset, seed={args.seed}, {args.threads} threads",
        flush=True,
    )
    cached_before = meminfo_kib("Cached")

    # Open each file once, advise random so buffered reads don't trigger
    # sequential readahead that would over-fill the cache beyond `target`.
    fds: dict[str, int] = {}
    for p, _ in files:
        fd = os.open(p, os.O_RDONLY)
        try:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_RANDOM)
        except (AttributeError, OSError):
            pass
        fds[p] = fd

    done = [0]
    t0 = time.monotonic()

    def read_one(item: tuple[str, int]) -> None:
        p, off = item
        # The returned bytes are discarded; the point is the page-cache fill.
        _ = os.pread(fds[p], extent, off)
        done[0] += 1

    next_report = 2 << 30  # report every 2 GiB
    try:
        with ThreadPoolExecutor(max_workers=args.threads) as ex:
            for _ in ex.map(read_one, selected, chunksize=64):
                read_bytes = done[0] * extent
                if read_bytes >= next_report:
                    dt = time.monotonic() - t0
                    print(
                        f"  {read_bytes/(1<<30):5.1f} / {n_sel*extent/(1<<30):.1f} GiB  "
                        f"({read_bytes/(1<<30)/max(dt,1e-3):.1f} GiB/s)",
                        flush=True,
                    )
                    next_report += 2 << 30
    finally:
        for fd in fds.values():
            os.close(fd)

    dt = time.monotonic() - t0
    cached_after = meminfo_kib("Cached")
    print(
        f"done: read {done[0]*extent/(1<<30):.1f} GiB in {dt:.1f}s "
        f"({done[0]*extent/(1<<30)/max(dt,1e-3):.1f} GiB/s); "
        f"page Cached {cached_before/(1<<20):.1f} -> {cached_after/(1<<20):.1f} GiB "
        f"(+{(cached_after-cached_before)/(1<<20):.1f})",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
