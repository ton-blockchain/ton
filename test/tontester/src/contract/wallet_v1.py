from dataclasses import dataclass
from typing import ClassVar, override

import nacl.signing
from pytoniq_core import (
    Builder,
    Cell,
    CurrencyCollection,
    InternalMsgInfo,
    MessageAny,
    WalletMessage,
    begin_cell,
)

from .contract import (
    Blueprint,
    ContractBlueprint,
    ContractView,
    Provider,
)


@dataclass
class WalletState:
    seqno: int
    public_key: nacl.signing.VerifyKey

    def serialize(self) -> Cell:
        return (
            begin_cell().store_uint(self.seqno, 32).store_bytes(self.public_key.encode()).end_cell()
        )


class WalletV1View(ContractView[WalletState]):
    _cached_public_key: nacl.signing.VerifyKey | None = None

    @override
    def _parse_state(self, state: Cell) -> WalletState:
        cs = state.begin_parse()
        seqno = cs.load_uint(32)
        public_key_bytes = cs.load_bytes(32)
        if self._cached_public_key is None:
            self._cached_public_key = nacl.signing.VerifyKey(public_key_bytes)
        return WalletState(seqno, self._cached_public_key)


@dataclass
class WalletV1(WalletV1View):
    key: nacl.signing.SigningKey

    def sign(self, message: WalletMessage, seqno: int) -> Cell:
        message_to_sign = (
            Builder().store_uint(seqno, 32).store_cell(message.serialize())
        ).end_cell()
        signature = self.key.sign(message_to_sign.hash).signature
        return Builder().store_bytes(signature).store_cell(message_to_sign).end_cell()

    async def send(self, message: WalletMessage, seqno: int | None = None):
        if seqno is None:
            seqno = (await self.current).seqno
        return await self.send_external(body=self.sign(message, seqno))

    async def deploy[T](
        self,
        blueprint: ContractBlueprint[T],
        amount: CurrencyCollection,
        seqno: int | None = None,
    ) -> T:
        msg = WalletMessage(
            send_mode=3,
            message=MessageAny(
                info=InternalMsgInfo(
                    ihr_disabled=True,
                    bounce=False,
                    bounced=False,
                    src=self.address,
                    dest=blueprint.address,
                    value=amount,
                    ihr_fee=0,
                    fwd_fee=0,
                    created_lt=0,
                    created_at=0,
                ),
                init=blueprint.state_init,
                body=Cell.empty(),
            ),
        )
        await self.send(msg, seqno)
        # FIXME: Wait for an internal message to actually be delivered.
        return blueprint.materialize(self.provider)


WALLET_V1_CODE = Cell.one_from_boc(
    "b5ee9c7201010101004e000098ff0020dd2082014c97ba9730ed44d0d70b1fe0a4f260810200d71820d70b1fed44d0d31fd3ffd15112baf2a122f901541044f910f2a2f80001d31f31d307d4d101fb00a4c8cb1fcbffc9ed54"
)


class WalletV1ViewBlueprint(Blueprint[WalletState]):
    CODE_BOC: ClassVar[Cell] = WALLET_V1_CODE

    def __init__(self, workchain: int, public_key: nacl.signing.VerifyKey):
        super().__init__(workchain, WalletState(seqno=0, public_key=public_key))

    @override
    def materialize(self, provider: Provider):
        return WalletV1View(provider, self.address)


class WalletV1Blueprint(WalletV1ViewBlueprint):
    def __init__(self, workchain: int = 0, private_key: nacl.signing.SigningKey | None = None):
        if private_key is None:
            private_key = nacl.signing.SigningKey.generate()
        self.private_key: nacl.signing.SigningKey = private_key
        super().__init__(workchain, private_key.verify_key)

    @override
    def materialize(self, provider: Provider):
        return WalletV1(provider, self.address, self.private_key)
