"""Smoke test: a single-node tontester network with simplex consensus.

Verifies that one full node (plus a dht node) produces masterchain and
workchain-0 blocks at the configured rate and processes transactions
end-to-end, before the jetton TPS benchmark is built on top of it.
"""

import asyncio
import logging
import shutil
import sys
import time
from pathlib import Path

from contract import WalletV1Blueprint, ton
from tontester.install import Install
from tontester.network import Network

from tonlib import TonlibClient

l = logging.getLogger("bench_smoke")

FULL_SHARD = -(2**63)
MIN_SEQNO = 10
RATE_SAMPLE_BLOCKS = 20
STARTUP_TIMEOUT = 120
WALLET_TIMEOUT = 30
OVERALL_TIMEOUT = 5 * 60


async def _print_chain_headers(client: TonlibClient, workchain: int) -> None:
    print(f"block headers (wc={workchain}, seqno 2..{MIN_SEQNO}):")
    utimes: dict[int, int] = {}
    for seqno in range(2, MIN_SEQNO + 1):
        block_id = await client.lookup_block(workchain=workchain, shard=FULL_SHARD, seqno=seqno)
        header = await client.get_block_header(block_id)
        utimes[seqno] = header.gen_utime
        print(f"  wc={workchain} seqno={seqno} utime={header.gen_utime}")
    span = utimes[MIN_SEQNO] - utimes[2]
    blocks = MIN_SEQNO - 2
    print(
        (
            f"  wc={workchain} blocks 2..{MIN_SEQNO}: utime span {span}s over {blocks} blocks"
            f" -> avg interval ~{span / blocks:.2f}s (utime has 1s granularity)"
        )
    )


async def _measure_wc0_rate(network: Network, client: TonlibClient) -> tuple[float, int]:
    """Measure wall time between observing wc0 seqno advances over ~20 blocks."""
    mc_info = await client.get_masterchain_info()
    assert mc_info.last is not None
    shards = await client.get_shards(mc_info.last)
    wc0_tip = max(s.seqno for s in shards.shards if s.workchain == 0)

    # Start a few blocks ahead of the tip so that t0 is observed live.
    start = wc0_tip + 4
    end = start + RATE_SAMPLE_BLOCKS
    _ = await network.wait_block(workchain=0, shard=FULL_SHARD, seqno=start)
    t0 = time.monotonic()
    _ = await network.wait_block(workchain=0, shard=FULL_SHARD, seqno=end)
    t1 = time.monotonic()
    rate = RATE_SAMPLE_BLOCKS / (t1 - t0)
    print(
        (
            f"wc0 blocks {start}..{end}: {RATE_SAMPLE_BLOCKS} blocks in {t1 - t0:.2f}s"
            f" -> {rate:.2f} blocks/s"
        )
    )
    return rate, end


async def _deploy_wallet(network: Network, client: TonlibClient) -> None:
    main_wallet = network.zerostate.main_wallet(client)
    new_wallet = await main_wallet.deploy(WalletV1Blueprint(workchain=0), ton(1))

    async def balance_positive() -> int:
        while True:
            state = await client.raw_get_account_state(new_wallet.address)
            if state.balance > 0:
                return state.balance
            await asyncio.sleep(0.2)

    balance = await asyncio.wait_for(balance_positive(), timeout=WALLET_TIMEOUT)
    print(f"wallet {new_wallet.address.to_str()} funded with {balance} nanotons")


async def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    working_dir = repo_root / "test/integration/.smoke"
    shutil.rmtree(working_dir, ignore_errors=True)
    working_dir.mkdir(exist_ok=True)

    install = Install(repo_root / "build", repo_root)
    install.tonlibjson.client_set_verbosity_level(1)

    logging.basicConfig(
        level=logging.INFO,
        format="[%(levelname)s][%(asctime)s][%(name)s] %(message)s",
        datefmt="%Y-%m-%d %H-%M-%S",
    )

    mc_blocks = 0
    wc0_blocks = 0
    wc0_rate: float | None = None
    wallet_ok = False
    failure: str | None = None

    try:
        async with Network(install, working_dir) as network:
            dht = network.create_dht_node()
            node = network.create_full_node()
            node.make_initial_validator()
            node.announce_to(dht)

            async with asyncio.TaskGroup() as start_group:
                _ = start_group.create_task(dht.run())
                _ = start_group.create_task(node.run())

            l.info(f"waiting for masterchain and wc0 to reach seqno {MIN_SEQNO}")
            async with asyncio.timeout(STARTUP_TIMEOUT):
                await network.wait_mc_block(seqno=MIN_SEQNO)
                mc_blocks = MIN_SEQNO
                _ = await network.wait_block(workchain=0, shard=FULL_SHARD, seqno=MIN_SEQNO)
                wc0_blocks = MIN_SEQNO

            client = await node.tonlib_client()

            await _print_chain_headers(client, 0)
            await _print_chain_headers(client, -1)

            wc0_rate, wc0_blocks = await _measure_wc0_rate(network, client)

            await _deploy_wallet(network, client)
            wallet_ok = True

            mc_info = await client.get_masterchain_info()
            assert mc_info.last is not None
            mc_blocks = mc_info.last.seqno
    except Exception as e:
        l.exception("smoke test failed")
        failure = repr(e)

    passed = mc_blocks >= MIN_SEQNO and wc0_blocks >= MIN_SEQNO and wallet_ok and failure is None
    rate_str = f"{wc0_rate:.2f} blocks/s" if wc0_rate is not None else "not measured"
    print()
    print(f"=== bench_smoke {'PASS' if passed else 'FAIL'} ===")
    print(f"  mc blocks observed:   {mc_blocks}")
    print(f"  wc0 blocks observed:  {wc0_blocks}")
    print(f"  measured wc0 rate:    {rate_str}")
    print(f"  wallet deploy:        {'ok' if wallet_ok else 'FAILED'}")
    if failure is not None:
        print(f"  failure:              {failure}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(asyncio.wait_for(main(), OVERALL_TIMEOUT)))
