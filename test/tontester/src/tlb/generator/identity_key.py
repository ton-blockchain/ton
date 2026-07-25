from typing import final, override


@final
class IdentityKey[T]:
    __slots__ = "_obj"

    def __init__(self, obj: T) -> None:
        self._obj = obj

    @property
    def value(self) -> T:
        return self._obj

    @override
    def __hash__(self) -> int:
        return id(self._obj)

    @override
    def __eq__(self, other: object) -> bool:
        if not isinstance(other, IdentityKey):
            return False
        return self._obj is other._obj

    @override
    def __repr__(self) -> str:
        return f"IdentityKey({self._obj!r})"
