## 2025.03 Update
1. New extracurrency behavior introduced, check [GlobalVersions.md](./doc/GlobalVersions.md#version-10)
2. Optmization of validation process, in particular CellStorageStat.
3. Flag for speeding up broadcasts in various overlays.
4. Fixes for static builds for emulator and tonlibjson
5. Improving getstats output: add
  * Liteserver queries count
  * Collated/validated blocks count, number of active sessions
  * Persistent state sizes
  * Initial sync progress
6. Fixes in logging, TON Storage, external message checking, persistent state downloading, UB in tonlib

Besides the work of the core team, this update is based on the efforts of  @Sild from StonFi(UB in tonlib).
