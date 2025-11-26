import asyncio
import logging
import shutil
from pathlib import Path

from contract import WalletV1
from tontester.install import Install
from tontester.network import FullNode, Network


async def main():
    repo_root = Path(__file__).resolve().parents[2]
    working_dir = repo_root / "test/integration/.network"
    shutil.rmtree(working_dir, ignore_errors=True)
    working_dir.mkdir(exist_ok=True)

    install = Install(repo_root / "build", repo_root)

    logging.basicConfig(
        level=logging.INFO,
        format="[%(levelname)s][%(asctime)s][%(name)s] %(message)s",
        datefmt="%Y-%m-%d %H-%M-%S",
    )

    async with Network(install, working_dir) as network:
        dht = network.create_dht_node()

        nodes: list[FullNode] = []
        for _ in range(2):
            node = network.create_full_node()
            node.make_initial_validator()
            node.announce_to(dht)
            nodes.append(node)

        async with asyncio.TaskGroup() as start_group:
            _ = start_group.create_task(dht.run())
            for node in nodes:
                _ = start_group.create_task(node.run())

        await network.wait_mc_block(seqno=1)

        actor_stats = await nodes[0].engine_console.get_actor_stats()
        assert "= ACTORS STATS =" in actor_stats and "= PERF COUNTERS =" in actor_stats

        main_wallet = network.get_or_generate_zerostate().main_wallet
        nw = WalletV1.create(wc=0)
        client = await network.get_tonlib_client()
        amount = 10**10
        msg = main_wallet.get_transfer_message(
            seqno=0,
            message=WalletV1.create_wallet_internal_message(
                destination=nw.address, send_mode=3, value=amount
            ),
        )
        _ = await client.send_message(msg)
        _ = await network.wait_block(
            workchain=0, shard=-(2**63), seqno=1
        )  # wait basechain to start

        nw_st = await network.wait_contract_balance_changed(address=nw.address, start_balance=-1)
        assert nw_st == amount
        msg = nw.get_init_external()
        _ = await client.send_message(msg)
        _ = await network.wait_contract_balance_changed(address=nw.address)
        st = await client.raw_get_account_state(nw.address)
        assert st.code and st.data  # contract should be deployed


if __name__ == "__main__":
    asyncio.run(asyncio.wait_for(main(), 5 * 60))
