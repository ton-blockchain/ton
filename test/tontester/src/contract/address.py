import base64
import binascii
from typing import override

from pytoniq_core.boc.address import Address, Anycast


class SMCAddress(Address):
    def __init__(self, address: "str | tuple[int, bytes] | SMCAddress"):
        super().__init__(address)
        self.wc: int
        self.hash_part: bytes
        self.is_bounceable: bool = False
        self.is_test_only: bool = False
        self.anycast: Anycast | None = None

        if isinstance(address, tuple):
            # Address((-1, b'\x11\x01\xff...'))
            self.wc = address[0]
            self.hash_part = address[1]
            return
        if isinstance(address, SMCAddress):
            self.wc = address.wc
            self.hash_part = address.hash_part
            return
        if self._parse_hex(address):
            return
        if self._parse_b64(address):
            return
        raise Exception("Unknown address type provided")

    def _parse_hex(self, addr: str) -> bool:
        try:
            wc, hash_part = addr.split(":")
            _ = int(hash_part, 16)
            self.wc = int(wc)
            self.hash_part = bytes.fromhex(hash_part)
            return True
        except ValueError:
            return False

    def _parse_b64(self, addr: str) -> bool:
        try:
            decoded = base64.urlsafe_b64decode(addr)
            tag = decoded[0]
            if tag & 0x80:  # test flag
                self.is_test_only = True
                tag ^= 0x80
            if tag == 0x11:  # bounceable
                self.is_bounceable = True
            self.wc = int.from_bytes(decoded[1:2], "big", signed=True)
            self.hash_part = decoded[2:34]
            if decoded[34:] != self._compute_crc16(decoded[:34]):
                raise Exception("the address is invalid")
            return True
        except binascii.Error:
            return False

    @staticmethod
    def _compute_crc16(message: bytes):
        message += b"\x00\x00"
        poly = 0x1021
        reg = 0
        for byte in message:
            mask = 0x80
            while mask > 0:
                reg <<= 1
                if byte & mask:
                    reg += 1
                mask >>= 1
                if reg > 0xFFFF:
                    reg &= 0xFFFF
                    reg ^= poly
        return reg.to_bytes(2, "big")

    @override
    def to_str(
        self,
        is_user_friendly: bool = True,
        is_url_safe: bool = True,
        is_bounceable: bool | None = None,
        is_test_only: bool | None = None,
    ) -> str:
        if not is_user_friendly:
            return f"{self.wc}:{self.hash_part.hex()}"
        tag = 0x11  # bounceable tag
        is_bounceable = is_bounceable if is_bounceable is not None else self.is_bounceable
        is_test_only = is_test_only if is_test_only is not None else self.is_test_only
        if not is_bounceable:
            tag = 0x51
        if is_test_only:
            tag |= 0x80
        result = tag.to_bytes(1, "big") + self.wc.to_bytes(1, "big", signed=True) + self.hash_part
        result += self._compute_crc16(result)
        if is_url_safe:
            result = base64.urlsafe_b64encode(result).decode()
        else:
            result = base64.b64encode(result).decode()
        return result

    @override
    def __repr__(self):
        return f"SMCAddress<{self.to_str()}>"
