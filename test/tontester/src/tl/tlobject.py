import base64
import json
from abc import ABC, abstractmethod
from collections.abc import Mapping, Sequence
from typing import Callable, Self, TextIO, cast, overload, override

type JSONSerializable = (
    None
    | bool
    | int
    | float
    | str
    | bytes
    | Sequence["JSONSerializable"]
    | Mapping[str, "JSONSerializable"]
)


class TLObject(ABC):
    @staticmethod
    def serialize_bytes(data: bytes | str) -> bytes:
        """Write bytes by using Telegram guidelines"""
        if not isinstance(data, bytes):
            data = data.encode("utf-8")

        r: list[bytes] = []
        if len(data) < 254:
            padding = (len(data) + 1) % 4
            if padding != 0:
                padding = 4 - padding

            r.append(bytes([len(data)]))
            r.append(data)

        else:
            padding = len(data) % 4
            if padding != 0:
                padding = 4 - padding

            r.append(bytes([254, len(data) % 256, (len(data) >> 8) % 256, (len(data) >> 16) % 256]))
            r.append(data)

        r.append(bytes(padding))
        return b"".join(r)

    @override
    def __eq__(self, o: object):
        return isinstance(o, type(self)) and self.to_dict() == o.to_dict()

    @override
    def __ne__(self, o: object):
        return not isinstance(o, type(self)) or self.to_dict() != o.to_dict()

    @override
    def __str__(self):
        return self.__class__.__name__ + f"<{str(self.to_dict())}>"

    @abstractmethod
    def to_dict(self) -> dict[str, JSONSerializable]:
        raise NotImplementedError

    @overload
    def to_json(self, fp: TextIO) -> None: ...

    @overload
    def to_json(self, fp: None = None) -> str: ...

    def to_json(self, fp: TextIO | None = None):
        """
        Represent the current `TLObject` as JSON.

        If ``fp`` is given, the JSON will be dumped to said
        file pointer, otherwise a JSON string will be returned.

        Note that bytes cannot be represented
        in JSON, so if those are found, they will be base64
        encoded and ISO-formatted, respectively, by default.
        """

        def encoder(value: JSONSerializable) -> JSONSerializable:
            if isinstance(value, bytes):
                return base64.b64encode(value).decode("ascii")
            else:
                return value

        d = self.to_dict()
        if fp:
            return json.dump(d, fp, default=encoder)
        else:
            return json.dumps(d, default=encoder)

    @classmethod
    @abstractmethod
    def from_dict(cls, d: JSONSerializable) -> Self:
        raise NotImplementedError

    @classmethod
    def from_json(cls, source: str) -> Self:
        return cls.from_dict(cast(JSONSerializable, json.loads(source)))


class TLRequest(TLObject, ABC):
    pass


class ModelError(Exception):
    pass


def _deserialize_signed_int(value: JSONSerializable, bits: int):
    if not isinstance(value, int):
        if bits > 53 and isinstance(value, str):
            try:
                value = int(value)
            except ValueError as e:
                raise ModelError(f"Expected int, got {type(value)}") from e
        else:
            raise ModelError(f"Expected int, got {type(value)}")
    min_value = -(1 << (bits - 1))
    max_value = (1 << (bits - 1)) - 1
    if min_value <= value <= max_value:
        return value
    raise ModelError(f"Integer value {value} out of bounds [{min_value}, {max_value}]")


def _deserialize_byte_string(value: JSONSerializable, length: int = -1):
    if not isinstance(value, str):
        raise ModelError(f"Expected base64-encoded string, got {type(value)}")
    try:
        decoded = base64.b64decode(value)
        if length != -1 and len(decoded) != length:
            raise ModelError(f"Expected byte string of length {length}, got {len(decoded)}")
        return decoded
    except Exception as e:
        raise ModelError(f"Invalid base64-encoded string: {e}") from e


def deserialize_int(value: JSONSerializable):
    return _deserialize_signed_int(value, 32)


def deserialize_long(value: JSONSerializable):
    return _deserialize_signed_int(value, 64)


def deserialize_int128(value: JSONSerializable):
    return _deserialize_byte_string(value, 16)


def deserialize_int256(value: JSONSerializable):
    return _deserialize_byte_string(value, 32)


def deserialize_int64(value: JSONSerializable):
    return _deserialize_signed_int(value, 64)


def deserialize_int32(value: JSONSerializable):
    return _deserialize_signed_int(value, 32)


def deserialize_int53(value: JSONSerializable):
    return _deserialize_signed_int(value, 53)


def deserialize_double(value: JSONSerializable):
    if not isinstance(value, (float, int)):
        raise ModelError(f"Expected float, got {type(value)}")
    return float(value)


def deserialize_string(value: JSONSerializable):
    if not isinstance(value, str):
        raise ModelError(f"Expected string, got {type(value)}")
    return value


def deserialize_bytes(value: JSONSerializable):
    return _deserialize_byte_string(value)


def deserialize_Bool(value: JSONSerializable):
    if not isinstance(value, bool):
        raise ModelError(f"Expected bool, got {type(value)}")
    return value


def deserialize_true(value: JSONSerializable):
    if value is not True:
        raise ModelError(f"Expected true, got {value}")
    return True


def deserialize_secureString(value: JSONSerializable):
    return deserialize_string(value)


def deserialize_secureBytes(value: JSONSerializable):
    return _deserialize_byte_string(value)


def deserialize_object[T](
    cls: Callable[[JSONSerializable], T],
) -> Callable[[JSONSerializable | None], T | None]:
    def deserializer(value: JSONSerializable | None) -> T | None:
        if value is None:
            return None
        return cls(value)

    return deserializer


def deserialize_list[T](
    value: JSONSerializable, item_deserializer: Callable[[JSONSerializable], T]
) -> list[T]:
    if not isinstance(value, list):
        raise ModelError(f"Expected list, got {type(value)}")
    return [item_deserializer(item) for item in value]
