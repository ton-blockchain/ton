## 2024.01 Update

1. Fixes in how gas in transactions on special accounts is accounted in block limit. Previously, gas was counted as usual, so to conduct elections that costs >30m gas block limit in masterchain was set to 37m gas. To lower the limit for safety reasons it is proposed to not count gas on special accounts. Besides `gas_max` is set to `special_gas_limit` for all types of transactions on special accounts. New behavior is activated through setting `gas_prices_v3` in `ConfigParam 20;`.
   * Besides update of config temporally increases gas limit on `EQD_v9j1rlsuHHw2FIhcsCFFSD367ldfDdCKcsNmNpIRzUlu` to `special_gas_limit`, see [details](https://t.me/tonstatus/88).
2. Improvements in LS behavior
   * Improved detection of the state with all shards applied to decrease rate of `Block is not applied` error
   * Better error logs: `block not in db` and `block is not applied` separation
   * Fix error in proof generation for blocks after merge
3. Improvements in DHT work and storage, CellDb, config.json ammendment, peer misbehavior detection, validator session stats collection, emulator.

Besides the work of the core team, this update is based on the efforts of @XaBbl4 (peer misbehavior detection).
