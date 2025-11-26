import asyncio
import logging
import traceback
from pathlib import Path

from contract import SMCAddress
from pytoniq_core import MessageAny
from tonapi import ton_api, tonlib_api

from .tonlibjson import TonLib

logger = logging.getLogger(__name__)


class TonlibClient:
    def __init__(
        self,
        ls_index: int,
        config: ton_api.Liteclient_config_global,
        cdll_path: Path,
        loop: asyncio.AbstractEventLoop | None = None,
        verbosity_level: int = 0,
    ):
        self.ls_index: int = ls_index
        self._config: ton_api.Liteclient_config_global = config
        self._cdll_path: Path = cdll_path
        self._loop: asyncio.AbstractEventLoop | None = loop
        self._verbosity_level: int = verbosity_level
        self._tonlib_wrapper: TonLib | None = None

    @property
    def local_config(self) -> ton_api.Liteclient_config_global:
        local = ton_api.Liteclient_config_global.from_json(self._config.to_json())
        local.liteservers = [local.liteservers[self.ls_index]]
        return local

    async def init(self) -> None:
        if self._tonlib_wrapper:
            logger.warning("init is already done")
            return
        event_loop = self._loop or asyncio.get_running_loop()
        self._tonlib_wrapper = TonLib(
            event_loop,
            self.ls_index,
            self._cdll_path,
            self._verbosity_level,
        )

        request = tonlib_api.InitRequest(
            options=tonlib_api.Options(
                config=tonlib_api.Config(
                    config=self.local_config.to_json(),
                    blockchain_name="",
                    use_callbacks_for_network=False,
                    ignore_cache=False,
                ),
                keystore_type=tonlib_api.KeyStoreTypeInMemory(),
            )
        )

        _ = await self._tonlib_wrapper.execute(request)

        logger.info(f"TonLib #{self.ls_index:03d} inited successfully")

    async def aclose(self):
        if self._tonlib_wrapper is not None:
            await self._tonlib_wrapper.aclose()
            self._tonlib_wrapper = None

    async def __aenter__(self):
        await self.init()
        return self

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: traceback.TracebackException | None,
    ):
        await self.aclose()

    def __await__(self):
        return self.init().__await__()

    async def sync_tonlib(self) -> tonlib_api.Ton_blockIdExt:
        assert self._tonlib_wrapper is not None
        request = tonlib_api.SyncRequest()
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def get_masterchain_info(self) -> tonlib_api.Blocks_masterchainInfo:
        assert self._tonlib_wrapper is not None
        request = tonlib_api.Blocks_getMasterchainInfoRequest()
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def raw_send_message(self, serialized_boc: bytes) -> tonlib_api.TypeOk:
        assert self._tonlib_wrapper is not None
        request = tonlib_api.Raw_sendMessageRequest(body=serialized_boc)
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def send_message(self, message: MessageAny) -> tonlib_api.TypeOk:
        assert self._tonlib_wrapper is not None
        serialized_boc = message.serialize().to_boc()
        return await self.raw_send_message(serialized_boc)

    async def get_libraries(self, library_list: list[bytes]) -> tonlib_api.Smc_libraryResult:
        assert self._tonlib_wrapper is not None
        request = tonlib_api.Smc_getLibrariesRequest(library_list)
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def lookup_block(
        self,
        workchain: int,
        shard: int,
        seqno: int | None = None,
        lt: int | None = None,
        utime: int | None = None,
    ):
        assert self._tonlib_wrapper is not None
        assert any((seqno, lt, utime)), "seqno, lt or unixtime must be provided"
        mode = 0
        if seqno is not None:
            mode += 1
        if lt is not None:
            mode += 2
        if utime is not None:
            mode += 4
        request = tonlib_api.Blocks_lookupBlockRequest(
            mode=mode,
            id=tonlib_api.Ton_blockId(
                workchain=workchain,
                shard=shard,
                seqno=seqno or 0,
            ),
            lt=lt or 0,
            utime=utime or 0,
        )
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def raw_get_transactions(
        self, account_address: SMCAddress, from_transaction_id: tonlib_api.Internal_transactionId
    ) -> tonlib_api.Raw_transactions:
        assert self._tonlib_wrapper is not None
        request = tonlib_api.Raw_getTransactionsRequest(
            account_address=tonlib_api.AccountAddress(
                account_address.to_str(is_user_friendly=True)
            ),
            from_transaction_id=from_transaction_id,
        )
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def raw_get_account_state(
        self, account_address: SMCAddress
    ) -> tonlib_api.Raw_fullAccountState:
        assert self._tonlib_wrapper is not None
        request = tonlib_api.Raw_getAccountStateRequest(
            account_address=tonlib_api.AccountAddress(account_address.to_str(is_user_friendly=True))
        )
        return request.parse_result(await self._tonlib_wrapper.execute(request))
