import asyncio
import logging
import shutil
from pathlib import Path
from typing import final, override

from tonapi import ton_api
from tontester.install import Install
from tontester.network import FullNode, Network
from tontester.zerostate import SimplexConsensusConfig

BASECHAIN_SHARD = ton_api.TonNode_shardId(workchain=0, shard=-(2**63))


@final
class LogWatcher(logging.Handler):
    def __init__(self, pattern: str):
        super().__init__()
        self._pattern = pattern
        self._loop = asyncio.get_running_loop()
        self.seen = asyncio.Event()

    @override
    def emit(self, record: logging.LogRecord):
        if self._pattern in record.getMessage():
            _ = self._loop.call_soon_threadsafe(self.seen.set)


async def main():
    repo_root = Path(__file__).resolve().parents[2]
    working_dir = repo_root / "test/integration/.network"
    shutil.rmtree(working_dir, ignore_errors=True)
    working_dir.mkdir(exist_ok=True)

    install = Install(repo_root / "build", repo_root)
    install.tonlibjson.client_set_verbosity_level(3)

    logging.basicConfig(
        level=logging.INFO,
        format="[%(levelname)s][%(asctime)s][%(name)s] %(message)s",
        datefmt="%Y-%m-%d %H-%M-%S",
    )

    async with Network(install, working_dir) as network:
        dht = network.create_dht_node()

        network.config.shard_validators = 2
        network.config.mc_valgroup_lifetime = 25
        network.config.shard_valgroup_lifetime = 25
        network.config.mc_consensus = SimplexConsensusConfig(protocol_version=3)
        network.config.shard_consensus = SimplexConsensusConfig(protocol_version=3)

        validators: list[FullNode] = []
        for _ in range(2):
            node = network.create_full_node()
            node.make_initial_validator()
            node.announce_to(dht)
            validators.append(node)

        collator = network.create_full_node()
        collator.announce_to(dht)

        delegated = LogWatcher("Delegating window")
        for node in validators:
            logging.getLogger(node.name).addHandler(delegated)
        accepted = LogWatcher("is delegated to us")
        logging.getLogger(collator.name).addHandler(accepted)

        async with asyncio.TaskGroup() as start_group:
            _ = start_group.create_task(dht.run())
            for node in [*validators, collator]:
                _ = start_group.create_task(node.run())

        collator_id = collator.fullnode_key.id
        _ = await collator.engine_console.request(
            ton_api.Engine_validator_addCollatorRequest(adnl_id=collator_id, shard=BASECHAIN_SHARD)
        )
        collators_list = ton_api.Engine_validator_collatorsList(
            shards=[
                ton_api.Engine_validator_collatorsList_shard(
                    shard_id=BASECHAIN_SHARD,
                    collators=[
                        ton_api.Engine_validator_collatorsList_collator(adnl_id=collator_id)
                    ],
                    self_collate=False,
                    select_mode="random",
                )
            ]
        )
        for node in validators:
            _ = await node.engine_console.request(
                ton_api.Engine_validator_setCollatorsListRequest(collators_list)
            )

        await network.wait_mc_block(seqno=1)

        async with asyncio.timeout(240):
            _ = await delegated.seen.wait()
            _ = await accepted.seen.wait()

        client = await validators[0].tonlib_client()
        mc_info = await client.get_masterchain_info()
        assert mc_info.last is not None
        await network.wait_mc_block(seqno=mc_info.last.seqno + 8)


if __name__ == "__main__":
    asyncio.run(asyncio.wait_for(main(), 10 * 60))
