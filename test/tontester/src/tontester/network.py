import asyncio
import json
import logging
import subprocess
import time
import types
from dataclasses import dataclass
from enum import IntEnum, auto
from ipaddress import IPv4Address
from pathlib import Path
from signal import SIGTERM
from typing import final, override

from pydantic import BaseModel
from pytonlib import TonlibClient, TonlibError  # pyright: ignore[reportMissingTypeStubs]

from .install import Install
from .key import Key
from .tl import tonapi, tonlibapi
from .zerostate import NetworkConfig, Zerostate, create_zerostate

l = logging.getLogger(__name__)


@dataclass
class _IPv4AddressAndPort:
    ip: int
    port: int


class _Status(IntEnum):
    INITED = auto()
    ZEROSTATE_GENERATED = auto()
    CLOSED = auto()


def _write_model(file: Path, model: BaseModel):
    _ = file.write_text(model.model_dump_json(indent=4, exclude_none=True))


@final
class Network:
    class Node:
        name: str

        _network: "Network"
        _directory: Path
        _keyring: Path
        _static_nodes: list["DHTNode"]

        def __init__(self, network: "Network", name: str):
            self._network = network
            self.name = name

            self._directory = self._network._directory / ("node" + str(self._network._node_idx))
            self._network._node_idx += 1

            self._keyring = self._directory / "keyring"
            self._keyring.mkdir(parents=True)

            self._static_nodes = []

            self.__process: asyncio.subprocess.Process | None = None

        @property
        def _install(self):
            return self._network._install

        def _new_network_address(self) -> _IPv4AddressAndPort:
            self._network._port += 1
            return _IPv4AddressAndPort(
                int(IPv4Address("127.0.42.239")),
                self._network._port,
            )

        def _new_key(self) -> tuple[Key, Path]:
            key = Key.new(self._install)
            pk_file = key.add_to_keyring(self._keyring)
            return (key, pk_file)

        def _ensure_no_zerostate_yet(self):
            assert self._network._status < _Status.ZEROSTATE_GENERATED

        def _get_or_generate_zerostate(self):
            return self._network._get_or_generate_zerostate()

        async def _run(
            self,
            executable: Path,
            local_config: tonapi.engine_validator_config,
            validator_config: tonapi.validator_config_Global | None,
            additional_args: list[str],
        ):
            assert self._network._status < _Status.CLOSED

            global_config_file = self._directory / "config.global.json"
            _write_model(
                global_config_file,
                tonapi.config_global(
                    dht=tonapi.dht_config_global(
                        static_nodes=tonapi.dht_nodes(
                            nodes=[node.signed_address for node in self._static_nodes]
                        ),
                        k=3,
                        a=3,
                    ),
                    validator=validator_config,
                ),
            )

            local_config_file = self._directory / "config.json"
            _write_model(local_config_file, local_config)

            self.__process = await asyncio.create_subprocess_exec(
                executable,
                "--global-config",
                global_config_file,
                "--local-config",
                local_config_file,
                "--db",
                ".",
                *additional_args,
                cwd=self._directory,
            )

        def announce_to(self, dht: "DHTNode"):
            self._static_nodes.append(dht)

        async def stop(self):
            if self.__process:
                try:
                    l.info(f"Killing node '{self.name}'")
                    self.__process.send_signal(SIGTERM)
                    _ = await self.__process.wait()
                except Exception:
                    l.exception(f"Unable to kill node '{self.name}'")
                self.__process = None

    def __init__(self, install: Install, directory: Path):
        self._install = install
        self._directory = directory.absolute()
        self._port = 2000
        self._node_idx = 0
        self._status = _Status.INITED

        self.__nodes: list[Network.Node] = []
        self.__full_nodes: list[FullNode] = []
        self.__network_config: NetworkConfig = NetworkConfig()
        self.__zerostate: Zerostate | None = None

    def create_dht_node(self) -> "DHTNode":
        assert self._status < _Status.CLOSED

        node = DHTNode(self, f"dht-{len(self.__nodes)}")
        self.__nodes.append(node)
        return node

    def create_full_node(self) -> "FullNode":
        assert self._status < _Status.CLOSED

        node = FullNode(self, f"node-{len(self.__nodes)}")
        self.__nodes.append(node)
        self.__full_nodes.append(node)
        return node

    def _get_or_generate_zerostate(self) -> Zerostate:
        if self.__zerostate is not None:
            return self.__zerostate

        assert self._status == _Status.INITED

        state_dir = self._directory / "state"
        state_dir.mkdir()

        self.__zerostate = create_zerostate(
            self._install,
            state_dir,
            self.__network_config,
            [node.validator_key for node in self.__full_nodes if node.is_initial_validator],
        )
        self._status = _Status.ZEROSTATE_GENERATED
        return self.__zerostate

    async def aclose(self):
        assert self._status < _Status.CLOSED
        self._status = _Status.CLOSED

        for node in self.__nodes:
            await node.stop()

    async def __aenter__(self):
        return self

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: types.TracebackType | None,
    ) -> bool | None:
        await self.aclose()

    async def wait_mc_block(self, seqno: int):
        client = await self.__full_nodes[0].tonlib_client()

        while True:
            try:
                raw_result = await client.get_masterchain_info()  # pyright: ignore[reportUnknownVariableType, reportUnknownMemberType]
            except TonlibError as e:
                # FIXME: We should really let node notify us that it is ready.
                try:
                    if (
                        e.result["code"] == 500
                        and (
                            e.result["message"]
                            == "LITE_SERVER_NETWORKtimeout for adnl query query"  # node is not synced yet
                            or e.result["message"]
                            == "LITE_SERVER_NETWORK"  # node is not listening the socket
                        )
                    ):
                        time.sleep(0.2)
                        continue
                except Exception:
                    pass
                raise

            mc_info = tonlibapi.blocks_masterchainInfo.model_validate_json(
                json.dumps(raw_result), strict=False
            )
            assert mc_info.last is not None

            if mc_info.last.seqno >= seqno:
                break
            else:
                time.sleep(0.2)


@final
class DHTNode(Network.Node):
    def __init__(self, network: "Network", name: str):
        super().__init__(network, name)

        self._addr = self._new_network_address()

        key, pk_file = self._new_key()

        address_list_to_sign = tonapi.adnl_addressList(
            addrs=[
                tonapi.adnl_address_udp(ip=self._addr.ip, port=self._addr.port),
            ]
        )
        signed_address = subprocess.run(
            (
                self._install.key_helper_exe,
                "-m",
                "dht",
                "-k",
                pk_file,
                "-a",
                address_list_to_sign.model_dump_json(exclude_none=True),
            ),
            check=True,
            stdout=subprocess.PIPE,
        ).stdout
        self._signed_address = tonapi.dht_node.model_validate_json(signed_address)

        self._local_config = tonapi.engine_validator_config(
            addrs=[
                tonapi.engine_addr(
                    ip=self._addr.ip,
                    port=self._addr.port,
                    categories=[0],
                )
            ],
            adnl=[tonapi.engine_adnl(id=key.id(), category=0)],
            dht=[tonapi.engine_dht(id=key.id())],
        )

    @property
    def signed_address(self):
        return self._signed_address

    async def run(self):
        await self._run(self._install.dht_server_exe, self._local_config, None, [])


@final
class FullNode(Network.Node):
    def __init__(self, network: "Network", name: str):
        super().__init__(network, name)

        KEY_EXPIRATION = (1 << 31) - 1

        self._addr = self._new_network_address()
        self._liteserver_addr = self._new_network_address()

        self._fullnode_key, _ = self._new_key()
        self._validator_key, _ = self._new_key()
        self._liteserver_key, _ = self._new_key()

        self._local_config = tonapi.engine_validator_config(
            addrs=[
                tonapi.engine_addr(
                    ip=self._addr.ip,
                    port=self._addr.port,
                    categories=[0],
                )
            ],
            adnl=[
                tonapi.engine_adnl(id=self._fullnode_key.id(), category=0),
                tonapi.engine_adnl(id=self._validator_key.id(), category=0),
            ],
            dht=[
                tonapi.engine_dht(id=self._fullnode_key.id()),
            ],
            validators=[
                tonapi.engine_validator(
                    id=self._validator_key.id(),
                    temp_keys=[
                        tonapi.engine_validatorTempKey(
                            key=self._validator_key.id(),
                            expire_at=KEY_EXPIRATION,
                        )
                    ],
                    adnl_addrs=[
                        tonapi.engine_validatorAdnlAddress(
                            id=self._validator_key.id(),
                            expire_at=KEY_EXPIRATION,
                        )
                    ],
                    expire_at=KEY_EXPIRATION,
                )
            ],
            fullnode=self._fullnode_key.id(),
            liteservers=[
                tonapi.engine_liteServer(
                    id=self._liteserver_key.id(),
                    # FIXME: IP?
                    port=self._liteserver_addr.port,
                )
            ],
        )

        self._is_initial_validator = False

        self._client: TonlibClient | None = None

    def make_initial_validator(self):
        self._ensure_no_zerostate_yet()
        self._is_initial_validator = True

    @property
    def is_initial_validator(self):
        return self._is_initial_validator

    @property
    def validator_key(self):
        return self._validator_key

    async def run(self):
        zerostate = self._get_or_generate_zerostate()

        static_dir = self._directory / "static"
        static_dir.mkdir()
        for state in (zerostate.masterchain, zerostate.shardchain):
            (static_dir / state.file_hash.hex().upper()).symlink_to(state.file)

        await self._run(
            self._install.validator_engine_exe,
            self._local_config,
            zerostate.as_validator_config(),
            ["--initial-sync-delay", "5"],
        )

    async def tonlib_client(self) -> TonlibClient:
        if self._client:
            return self._client

        config = tonapi.liteclient_config_global(
            liteservers=[
                tonapi.liteserver_desc(
                    id=self._liteserver_key.public_key,
                    ip=self._liteserver_addr.ip,
                    port=self._liteserver_addr.port,
                ),
            ],
            validator=self._get_or_generate_zerostate().as_validator_config(),
        )

        keystore_dir = self._directory / "lc-keystore"
        keystore_dir.mkdir()

        self._client = TonlibClient(
            ls_index=0,
            config=json.loads(config.model_dump_json()),  # pyright: ignore[reportAny]
            keystore=str(keystore_dir),
            cdll_path=str(self._install.tonlibjson),
            verbosity_level=3,
        )
        await self._client.init()

        return self._client

    @override
    async def stop(self):
        if self._client:
            await self._client.close()
        await super().stop()
