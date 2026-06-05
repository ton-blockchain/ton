import asyncio
import logging
import traceback
from typing import Callable, final

from pytoniq_core import Address, Cell, MessageAny
from tonapi import ton_api, tonlib_api

from .tonlib_cdll import TonlibCDLL
from .tonlibjson import TonLib

logger = logging.getLogger(__name__)


@final
class TonlibStateReader:
    def __init__(self, client: TonlibClient):
        self._client = client
        self._cache: dict[tuple[int, int, int, Address], Cell] = {}

    async def _get_data(self, address: Address) -> Cell:
        state = await self._client.raw_get_account_state(address)
        block_id = state.block_id
        assert block_id is not None
        key = (block_id.workchain, block_id.shard, block_id.seqno, address)
        if key not in self._cache:
            self._cache[key] = Cell.one_from_boc(state.data)
        return self._cache[key]

    async def fetch[T](self, address: Address, parser: Callable[[Cell], T]) -> T:
        data = await self._get_data(address)
        return parser(data)


class TonlibClient:
    def __init__(
        self,
        config: ton_api.Liteclient_config_global,
        tonlib: TonlibCDLL,
        loop: asyncio.AbstractEventLoop | None = None,
    ):
        self._config: ton_api.Liteclient_config_global = config
        self._tonlib: TonlibCDLL = tonlib
        self._loop: asyncio.AbstractEventLoop | None = loop
        self._tonlib_wrapper: TonLib | None = None

    async def init(self) -> None:
        if self._tonlib_wrapper:
            logger.warning("init is already done")
            return
        event_loop = self._loop or asyncio.get_running_loop()
        self._tonlib_wrapper = TonLib(event_loop, self._tonlib)

        request = tonlib_api.InitRequest(
            options=tonlib_api.Options(
                config=tonlib_api.Config(
                    config=self._config.to_json(),
                    blockchain_name="",
                    use_callbacks_for_network=False,
                    ignore_cache=False,
                ),
                keystore_type=tonlib_api.KeyStoreTypeInMemory(),
            )
        )

        _ = await self._tonlib_wrapper.execute(request)

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

    @property
    def latest_state_reader(self) -> TonlibStateReader:
        return TonlibStateReader(self)

    async def send_external(self, message: MessageAny) -> None:
        _ = await self.raw_send_message(message.serialize().to_boc())

    async def raw_send_message(self, serialized_boc: bytes) -> tonlib_api.TypeOk:
        assert self._tonlib_wrapper is not None
        request = tonlib_api.Raw_sendMessageRequest(body=serialized_boc)
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def get_libraries(self, library_list: list[bytes]) -> tonlib_api.Smc_libraryResult:
        assert self._tonlib_wrapper is not None
        request = tonlib_api.Smc_getLibrariesRequest(library_list)
        return request.parse_result(await self._tonlib_wrapper.execute(request))

    async def raw_get_transactions(
        self, account_address: Address, from_transaction_id: tonlib_api.Internal_transactionId
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
        self, account_address: Address
    ) -> tonlib_api.Raw_fullAccountState:
        assert self._tonlib_wrapper is not None
        request = tonlib_api.Raw_getAccountStateRequest(
            account_address=tonlib_api.AccountAddress(account_address.to_str(is_user_friendly=True))
        )
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
        assert seqno is not None or lt is not None or utime is not None
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
