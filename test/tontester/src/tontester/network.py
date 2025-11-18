import asyncio
import logging
import signal
import subprocess
import types
from abc import ABC, abstractmethod
from dataclasses import dataclass
from enum import IntEnum, auto
from ipaddress import IPv4Address
from pathlib import Path
from typing import Literal, cast, final, override

from pytonlib import TonlibClient, TonlibError  # pyright: ignore[reportMissingTypeStubs]

from tl import JSONSerializable, TLObject

from .install import Install
from .key import Key
from .log_streamer import LogStreamer
from .tl import ton_api, tonlib_api
from .zerostate import NetworkConfig, Zerostate, create_zerostate

l = logging.getLogger(__name__)


@dataclass
class _IPv4AddressAndPort:
    ip: IPv4Address
    port: int


class _Status(IntEnum):
    INITED = auto()
    ZEROSTATE_GENERATED = auto()
    CLOSED = auto()


def _write_model(file: Path, model: TLObject):
    _ = file.write_text(model.to_json())


type DebugType = None | Literal["rr"]


@final
class Network:
    class Node(ABC):
        def __init__(self, network: "Network", name: str):
            self._network: Network = network
            self.name: str = name

            self._directory: Path = self._network._directory / (
                "node" + str(self._network._node_idx)
            )
            self._network._node_idx += 1

            self._keyring: Path = self._directory / "keyring"
            self._keyring.mkdir(parents=True)

            self._static_nodes: list["DHTNode"] = []

            self.__process: asyncio.subprocess.Process | None = None
            self.__process_watcher: asyncio.Task[None] | None = None
            self.__log_streamer: LogStreamer | None = None

        @property
        def _install(self):
            return self._network._install

        def _new_network_address(self) -> _IPv4AddressAndPort:
            self._network._port += 1
            return _IPv4AddressAndPort(
                IPv4Address("127.0.42.239"),
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
            local_config: ton_api.Engine_validator_config,
            validator_config: ton_api.Validator_config_global | None,
            additional_args: list[str],
            *,
            debug: DebugType = None,
        ):
            async def process_watcher():
                assert self.__process is not None
                return_code = await self.__process.wait()
                if return_code < 0:
                    signal_name = signal.Signals(-return_code).name
                    l.info(f"Node '{self.name}' terminated by signal {signal_name}")
                else:
                    l.info(f"Node '{self.name}' exited with code {return_code}")

            assert self._network._status < _Status.CLOSED

            global_config_file = self._directory / "config.global.json"
            _write_model(
                global_config_file,
                ton_api.Config_global(
                    dht=ton_api.Dht_config_global(
                        static_nodes=ton_api.Dht_nodes(
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

            log_path = self._directory / "log"
            l.info(f"Running {self.name} and saving its raw log to {log_path}")

            cmd_flags = [
                "--global-config",
                global_config_file,
                "--local-config",
                local_config_file,
                "--db",
                ".",
                *additional_args,
            ]

            match debug:
                case None:
                    self.__process = await asyncio.create_subprocess_exec(
                        executable,
                        *cmd_flags,
                        cwd=self._directory,
                        stderr=asyncio.subprocess.PIPE,
                    )
                case "rr":
                    l.info(f"Recording {self.name} with rr")
                    self.__process = await asyncio.create_subprocess_exec(
                        "rr",
                        "record",
                        executable,
                        *cmd_flags,
                        cwd=self._directory,
                        stderr=asyncio.subprocess.PIPE,
                    )

            assert self.__process.stderr is not None  # to placate pyright
            self.__process_watcher = asyncio.create_task(process_watcher())

            self.__log_streamer = LogStreamer(
                open(log_path, "wb"),
                self.name,
                self.__process.stderr,
            )

        def announce_to(self, dht: "DHTNode"):
            self._static_nodes.append(dht)

        @abstractmethod
        async def run(self, *, debug: DebugType = None):
            pass

        async def stop(self):
            if self.__process:
                # No exception can occur between self.__process and self._log_streamer creation
                assert self.__log_streamer is not None
                assert self.__process_watcher is not None

                if not self.__process_watcher.done():
                    l.info(f"Killing node '{self.name}'")
                    try:
                        self.__process.terminate()
                    except ProcessLookupError:
                        # Terminate might still fail if Python internally has already finished
                        # waiting for the child process but didn't yet resume the watcher.
                        pass

                await self.__process_watcher
                await self.__log_streamer.aclose()

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

    @property
    def config(self):
        assert self._status < _Status.ZEROSTATE_GENERATED
        return self.__network_config

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
        await asyncio.shield(self.aclose())

    async def wait_mc_block(self, seqno: int):
        client = await self.__full_nodes[0].tonlib_client()

        while True:
            try:
                raw_result = cast(JSONSerializable, await client.get_masterchain_info())  # pyright: ignore[reportUnknownMemberType]
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
                        await asyncio.sleep(0.2)
                        continue
                except Exception:
                    pass
                raise

            mc_info = tonlib_api.Blocks_masterchainInfo.from_dict(raw_result)
            assert mc_info.last is not None

            if mc_info.last.seqno >= seqno:
                break
            else:
                await asyncio.sleep(0.2)


def _ip_to_tl(ip: IPv4Address) -> int:
    result = int(ip)
    if result >= 2**31:
        result -= 2**32
    return result


@final
class DHTNode(Network.Node):
    def __init__(self, network: "Network", name: str):
        super().__init__(network, name)

        self._addr = self._new_network_address()

        key, pk_file = self._new_key()

        address_list_to_sign = ton_api.Adnl_addressList(
            addrs=[
                ton_api.Adnl_address_udp(ip=_ip_to_tl(self._addr.ip), port=self._addr.port),
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
                address_list_to_sign.to_json(),
            ),
            check=True,
            stdout=subprocess.PIPE,
        ).stdout
        self._signed_address = ton_api.Dht_node.from_json(signed_address.decode())

        self._local_config = ton_api.Engine_validator_config(
            addrs=[
                ton_api.Engine_addr(
                    ip=_ip_to_tl(self._addr.ip),
                    port=self._addr.port,
                    categories=[0],
                )
            ],
            adnl=[ton_api.Engine_adnl(id=key.id(), category=0)],
            dht=[ton_api.Engine_dht(id=key.id())],
        )

    @property
    def signed_address(self):
        return self._signed_address

    @override
    async def run(self, *, debug: DebugType = None):
        await self._run(self._install.dht_server_exe, self._local_config, None, [], debug=debug)


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

        self._local_config = ton_api.Engine_validator_config(
            addrs=[
                ton_api.Engine_addr(
                    ip=_ip_to_tl(self._addr.ip),
                    port=self._addr.port,
                    categories=[0],
                )
            ],
            adnl=[
                ton_api.Engine_adnl(id=self._fullnode_key.id(), category=0),
                ton_api.Engine_adnl(id=self._validator_key.id(), category=0),
            ],
            dht=[
                ton_api.Engine_dht(id=self._fullnode_key.id()),
            ],
            validators=[
                ton_api.Engine_validator(
                    id=self._validator_key.id(),
                    temp_keys=[
                        ton_api.Engine_validatorTempKey(
                            key=self._validator_key.id(),
                            expire_at=KEY_EXPIRATION,
                        )
                    ],
                    adnl_addrs=[
                        ton_api.Engine_validatorAdnlAddress(
                            id=self._validator_key.id(),
                            expire_at=KEY_EXPIRATION,
                        )
                    ],
                    expire_at=KEY_EXPIRATION,
                )
            ],
            fullnode=self._fullnode_key.id(),
            liteservers=[
                ton_api.Engine_liteServer(
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

    @override
    async def run(self, *, debug: DebugType = None):
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
            debug=debug,
        )

    async def tonlib_client(self) -> TonlibClient:
        if self._client:
            return self._client

        config = ton_api.Liteclient_config_global(
            liteservers=[
                ton_api.Liteserver_desc(
                    id=self._liteserver_key.public_key,
                    ip=_ip_to_tl(self._liteserver_addr.ip),
                    port=self._liteserver_addr.port,
                ),
            ],
            validator=self._get_or_generate_zerostate().as_validator_config(),
        )

        keystore_dir = self._directory / "lc-keystore"
        keystore_dir.mkdir()

        self._client = TonlibClient(
            ls_index=0,
            config=config.to_dict(),
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
