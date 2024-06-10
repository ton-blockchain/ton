## 2024.04 Update

1. Make Jemalloc default allocator
2. Add candidate broadcasting and caching
3. Limit per address speed for external messages broadcast by reasonably large number 
4. Overlay improvements: fix dropping peers in small custom overlays, fix wrong certificate on missed keyblocks
5. Extended statistics and logs for celldb usage, session stats, persistent state serialization
6. Tonlib and explorer fixes
7. Flags for precize control of Celldb: `--celldb-cache-size`, `--celldb-direct-io` and `--celldb-preload-all`
8. Add valiator-console command to stop persistent state serialization
9. Use `@` path separator for defining include path in fift and create-state utilities on Windows only.
