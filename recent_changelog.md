## 2024.01 Update

1. Fixes in how gas in transactions on special accounts is accounted in block limit. Previously, gas was counted as usual, so to conduct elections that costs >30m gas block limit in masterchain was set to 37m gas. To lower the limit for safety reasons it is proposed to caunt gas on special accounts separately. Besides `gas_max` is set to `special_gas_limit` for all types of transactions on special accounts. New behavior is activated through setting `version >= 5` in `ConfigParam 8;`.
   * Besides update of config temporally increases gas limit on `EQD_v9j1rlsuHHw2FIhcsCFFSD367ldfDdCKcsNmNpIRzUlu` to `special_gas_limit`, see [details](https://t.me/tonstatus/88).
2. Improvements in LS behavior
   * Improved detection of the state with all shards applied to decrease rate of `Block is not applied` error
   * Better error logs: `block not in db` and `block is not applied` separation
   * Fix error in proof generation for blocks after merge
   * Fix most of `block is not applied` issues related to sending too recent block in Proofs
   * LS now check external messages till `accept_message` (`set_gas`).
3. Improvements in DHT work and storage, CellDb, config.json ammendment, peer misbehavior detection, validator session stats collection, emulator.
4. Change in CTOS and XLOAD behavior activated through setting `version >= 5` in `ConfigParam 8;`:
   * Loading "nested libraries" (i.e. a library cell that points to another library cell) throws an exception.
   * Loading a library consumes gas for cell load only once (for the library cell), not twice (both for the library cell and the cell in the library).
   * `XLOAD` now works differently. When it takes a library cell, it returns the cell that it points to. This allows loading "nested libraries", if needed.

Besides the work of the Core team, this update is based on the efforts of @XaBbl4 (peer misbehavior detection) and @akifoq (CTOS behavior and gas limit scheme for special accounts).
