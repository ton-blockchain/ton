# Read-only global balance calculation

`validator/global-balance-calculator.hpp` provides historical snapshot APIs
that use the same queue accounting as the validator, without requiring a running
`ValidatorManager`.

The result contains exact nanotons in `accounts`, `dispatch_queue`,
`unprocessed_messages`, and `unrecovered_validator_fees`. These four components
add to `total`. `burned` is the masterchain block's diagnostic ValueFlow amount;
it must not be deducted from this post-state snapshot again.

## Independent snapshots

The four-argument `calculate_global_balance_at_anchor` accepts the masterchain
state/block, all shard tips referenced by that masterchain state, and a callback
for auxiliary masterchain states. It prepares every state independently. This is
useful for isolated queries and as a reference calculation.

Old dispatch queues do not store their total balance. Independent preparation
must read all their messages, so repeating it at every historical masterchain
block can be expensive.

## Incremental preparation

`prepare_global_balance_state` returns an opaque `PreparedGlobalBalanceState`:

```cpp
auto prepared = co_await prepare_global_balance_state(state, block_root, version);
auto next = co_await prepare_global_balance_state(next_state, next_block_root, version, {prepared});
```

Here `version` is the global version of the anchor masterchain configuration.
The first call initializes the queues. The second applies the next block's
`OutMsgDescr` to the outgoing queue and uses dictionary differences for a legacy
dispatch queue. A stored dispatch balance is read directly. The native split
path scans each child's legacy dispatch queue once; the merge path combines the
two predecessors before applying differences.

When supplied, predecessors must be the **immediate** predecessors from the
block header, in header order (left then right for a merge), with the same global
version. Both their full block IDs and each prepared block/state pair are checked.
The reconstructed outgoing queue must have the hash stored in the new state.
Prepare every intermediate shard block in dependency order if several shard
blocks were included by one masterchain block.

The five-argument overload calculates a complete snapshot from these handles:

```cpp
auto components = co_await calculate_global_balance_at_anchor(
    mc_state, mc_block_root, prepared_mc, prepared_shard_tips, load_aux_mc_state);
```

`prepared_mc` must match the supplied masterchain state. `prepared_shard_tips`
must contain exactly the tips in that state's shard configuration; their order
does not matter. All handles must use that masterchain configuration's global
version. An unchanged shard tip can be reused across multiple masterchain
snapshots. The calculation still evaluates outgoing messages against the current
snapshot's `processed_upto`: unchanged queue contents do not imply an unchanged
unprocessed-message balance.

## Cache ownership and recovery

The caller owns the cache and scheduling policy. Key handles by full `BlockIdExt`
and global version. Retain current tips and active dependencies, and release old
handles as the indexing window advances. Handles share immutable cells and
amounts, use separate dictionary wrappers for each operation, and do not retain
predecessor handles or a growing chain of historical states.

At startup, after a gap, or on a global-version change, prepare the current tips
without predecessors. This permits bounded initialization at any available
historical anchor. Missing inputs and inconsistent predecessors return errors;
they never produce a partial balance or modify existing handles. Failed requests
can therefore be retried, and retained handles can serve out-of-order work.

For verification, compare **every component** with the independent API on the
same anchor, including split/merge boundaries and protocol-version changes.
