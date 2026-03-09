import asyncio
import logging
import traceback
from pathlib import Path

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

    async def get_libraries(self, library_list: list[bytes]) -> tonlib_api.Smc_libraryResult:
        assert self._tonlib_wrapper is not None
        request = tonlib_api.Smc_getLibrariesRequest(library_list)
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def raw_get_transactions(
        self, account_address: str, from_transaction_lt: int, from_transaction_hash: bytes
    ) -> tonlib_api.Raw_transactions:
        assert self._tonlib_wrapper is not None
        # FIXME: Replace these with proper classes for TransactionId and Address.
        assert len(account_address) == 48, "account address must be serialized"
        assert len(from_transaction_hash) == 32
        request = tonlib_api.Raw_getTransactionsRequest(
            account_address=tonlib_api.AccountAddress(account_address),
            from_transaction_id=tonlib_api.Internal_transactionId(
                lt=from_transaction_lt,
                hash=from_transaction_hash,
            ),
        )
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def raw_get_account_state(self, account_address: str) -> tonlib_api.Raw_fullAccountState:
        assert self._tonlib_wrapper is not None
        # FIXME: Replace these with proper class for Address.
        assert len(account_address) == 48, "account address must be serialized"
        request = tonlib_api.Raw_getAccountStateRequest(
            account_address=tonlib_api.AccountAddress(account_address)
        )
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def get_shards(self, block_id: tonlib_api.Ton_blockIdExt) -> tonlib_api.Blocks_shards:
        assert self._tonlib_wrapper is not None
        request = tonlib_api.Blocks_getShardsRequest(id=block_id)
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def get_block_header(
        self, block_id: tonlib_api.Ton_blockIdExt
    ) -> tonlib_api.Blocks_header:
        assert self._tonlib_wrapper is not None
        request = tonlib_api.Blocks_getBlockHeaderRequest(id=block_id)
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def raw_get_block_transactions(
        self,
        block_id: tonlib_api.Ton_blockIdExt,
        count: int,
        after: tonlib_api.Blocks_accountTransactionId | None = None,
    ) -> tonlib_api.Blocks_transactions:
        assert self._tonlib_wrapper is not None
        mode = 7
        if after is not None:
            mode += 128
        request = tonlib_api.Blocks_getTransactionsRequest(
            id=block_id,
            mode=mode,
            count=count,
            after=after,
        )
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def get_block_transactions(
        self,
        block_id: tonlib_api.Ton_blockIdExt,
    ) -> list[tonlib_api.Blocks_shortTxId]:
        result: list[tonlib_api.Blocks_shortTxId] = []
        after = None
        while True:
            batch = await self.raw_get_block_transactions(block_id, 256, after)
            result.extend(batch.transactions)
            if not batch.incomplete:
                break
            after = tonlib_api.Blocks_accountTransactionId(
                account=batch.transactions[-1].account,
                lt=batch.transactions[-1].lt,
            )
        return result
