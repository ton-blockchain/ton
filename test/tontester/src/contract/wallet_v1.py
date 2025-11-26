import os
from dataclasses import dataclass
from typing import Self, final, override

from pytoniq_core import Builder, Cell, MessageAny, StateInit, WalletMessage, begin_cell

from .address import SMCAddress
from .contract import Contract
from .crypto import private_key_to_public_key, sign_message


@final
@dataclass
class WalletV1(Contract):
    private_key: bytes | None

    CODE_BOC = "b5ee9c7201010101004e000098ff0020dd2082014c97ba9730ed44d0d70b1fe0a4f260810200d71820d70b1fed44d0d31fd3ffd15112baf2a122f901541044f910f2a2f80001d31f31d307d4d101fb00a4c8cb1fcbffc9ed54"

    @staticmethod
    def _create_data_cell(seqno: int, public_key: bytes) -> Cell:
        return begin_cell().store_uint(seqno, 32).store_bytes(public_key).end_cell()

    @classmethod
    def from_params(
        cls, public_key: bytes, wc: int = 0, private_key: bytes | None = None
    ) -> "Self":
        data = cls._create_data_cell(public_key=public_key, seqno=0)
        state_init = StateInit(code=Cell.one_from_boc(cls.CODE_BOC), data=data)
        address = cls._compute_address(wc, state_init)
        return cls(address=address, state_init=state_init, private_key=private_key)

    @classmethod
    def from_private_key(cls, private_key: bytes, wc: int = 0) -> Self:
        public_key: bytes = private_key_to_public_key(private_key)
        return cls.from_params(public_key=public_key, wc=wc, private_key=private_key)

    @classmethod
    def create(cls, wc: int = 0) -> Self:
        return cls.from_private_key(os.urandom(32), wc)

    @staticmethod
    def _raw_create_transfer_msg(private_key: bytes, seqno: int, message: WalletMessage) -> Cell:
        signing_message = (
            Builder().store_uint(seqno, 32).store_cell(message.serialize())
        ).end_cell()
        signature = sign_message(signing_message.hash, private_key)
        return Builder().store_bytes(signature).store_cell(signing_message).end_cell()

    @staticmethod
    def create_wallet_internal_message(
        destination: SMCAddress,
        send_mode: int,
        value: int,
        body: Cell | str | None = None,
        state_init: StateInit | None = None,
    ) -> WalletMessage:
        if isinstance(body, str):
            body = Builder().store_uint(0, 32).store_snake_string(body).end_cell()
        if body is None:
            body = Cell.empty()

        message = Contract.create_internal_msg(
            dest=destination, value=value, body=body, state_init=state_init
        )
        return WalletMessage(send_mode=send_mode, message=message)

    def get_transfer_message(self, seqno: int, message: WalletMessage) -> MessageAny:
        assert self.private_key is not None, "must specify wallet private key!"
        transfer_msg = self._raw_create_transfer_msg(
            private_key=self.private_key, seqno=seqno, message=message
        )
        return self.get_external_message(body=transfer_msg)

    @override
    def get_init_external(self) -> MessageAny:
        assert self.state_init is not None, "contract does not have state_init"
        assert self.private_key is not None, "must specify wallet private key!"
        body = self._raw_create_transfer_msg(
            private_key=self.private_key,
            seqno=0,
            message=self.create_wallet_internal_message(
                destination=self.address, send_mode=3, value=0
            ),
        )
        return self.get_external_message(state_init=self.state_init, body=body)
