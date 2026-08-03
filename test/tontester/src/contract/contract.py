from abc import ABC, abstractmethod
from dataclasses import dataclass
from decimal import Decimal
from typing import Callable, ClassVar, Protocol

from pytoniq_core import (
    Address,
    Cell,
    CurrencyCollection,
    ExternalAddress,
    ExternalMsgInfo,
    MessageAny,
    StateInit,
)


def ton(amount: int | float | str | Decimal) -> CurrencyCollection:
    decimal_amount = amount if isinstance(amount, Decimal) else Decimal(str(amount))
    nano = decimal_amount * Decimal("1000000000")
    if nano != nano.to_integral_value():
        raise ValueError(f"TON amount must resolve to a whole number of nanotons: {amount!r}")
    return CurrencyCollection(grams=int(nano))


class Provider(Protocol):
    @property
    def latest_state_reader(self) -> StateReader: ...

    async def send_external(self, message: MessageAny) -> None: ...


class StateReader(Protocol):
    async def fetch[T](self, address: Address, parser: Callable[[Cell], T]) -> T: ...


class ContractState(Protocol):
    def serialize(self) -> Cell: ...


class ContractBlueprint[T](Protocol):
    state_init: StateInit
    address: Address

    def materialize(self, provider: Provider) -> T: ...


@dataclass
class ContractView[S: ContractState](ABC):
    provider: Provider
    address: Address

    @abstractmethod
    def _parse_state(self, state: Cell) -> S:
        pass

    @property
    def current(self):
        return self.provider.latest_state_reader.fetch(self.address, self._parse_state)

    async def send_external(
        self,
        src: ExternalAddress | None = None,
        import_fee: int = 0,
        state_init: StateInit | None = None,
        body: Cell | None = None,
    ):
        info = ExternalMsgInfo(src, self.address, import_fee)  # pyright: ignore [reportArgumentType]
        if body is None:
            body = Cell.empty()
        return await self.provider.send_external(MessageAny(info=info, init=state_init, body=body))


class Blueprint[S: ContractState](ABC):
    CODE_BOC: ClassVar[Cell]

    def __init__(self, workchain: int, data_init: S):
        self.state_init: StateInit = StateInit(code=self.CODE_BOC, data=data_init.serialize())
        self.address: Address = Address((workchain, self.state_init.serialize().hash))

    @abstractmethod
    def materialize(self, provider: Provider) -> ContractView[S]:
        pass

    async def deploy[T](self: ContractBlueprint[T], provider: Provider) -> T:
        info = ExternalMsgInfo(None, self.address, 0)  # pyright: ignore [reportArgumentType]
        msg = MessageAny(info=info, init=self.state_init, body=Cell.empty())
        await provider.send_external(msg)
        return self.materialize(provider)
