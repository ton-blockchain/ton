import asyncio
import logging
import shutil
from pathlib import Path

from tontester.install import Install
from tontester.network import Network


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

        nodes: list[Network.Node] = []
        for _ in range(2):
            node = network.create_full_node()
            node.make_initial_validator()
            node.announce_to(dht)
            nodes.append(node)

        await dht.run()
        for node in nodes:
            await node.run()

        await network.wait_mc_block(seqno=1)


if __name__ == "__main__":
    asyncio.run(asyncio.wait_for(main(), 5 * 60))
