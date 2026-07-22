"""Compute validator_list_hash_short matching C++ block.cpp / mc-config.cpp.

Reimplements ValidatorSetPRNG, do_compute_validator_set, and
compute_validator_set_hash for the masterchain zerostate case.
"""

import hashlib
import struct
from typing import final

from pytoniq_core.crypto.crc import crc32c

from .key import Key


@final
class _ValidatorSetPRNG:
    """SHA512-based PRNG matching C++ ValidatorSetPRNG."""

    def __init__(self, workchain: int, shard: int, cc_seqno: int):
        # validator_set_descr: seed[32] + shard:u64be + workchain:i32be + cc_seqno:u32be
        self._seed = bytearray(32)
        self._shard = shard
        self._workchain = workchain
        self._cc_seqno = cc_seqno
        self._hash_longs: list[int] = []
        self._pos = 0

    def _hash_and_advance(self):
        # Hash the descriptor with SHA512
        buf = (
            bytes(self._seed)
            + struct.pack(">Q", self._shard)
            + struct.pack(">i", self._workchain)
            + struct.pack(">I", self._cc_seqno)
        )
        h = hashlib.sha512(buf).digest()
        # Parse as 8 big-endian uint64s (then bswap = read as big-endian)
        self._hash_longs = [struct.unpack(">Q", h[i * 8 : (i + 1) * 8])[0] for i in range(8)]
        self._pos = 0
        # Increment seed (big-endian 256-bit increment)
        for i in range(31, -1, -1):
            self._seed[i] = (self._seed[i] + 1) & 0xFF
            if self._seed[i] != 0:
                break

    def next_ulong(self) -> int:
        if self._pos >= len(self._hash_longs):
            self._hash_and_advance()
        val = self._hash_longs[self._pos]
        self._pos += 1
        return val

    def next_ranged(self, range_: int) -> int:
        """Return integer in [0, range_). Uses (range * next_ulong) >> 64."""
        y = self.next_ulong()
        return (range_ * y) >> 64


def compute_validator_set_hash(
    validator_keys: list[Key],
    shuffle: bool = True,
) -> int:
    """Compute the validator_list_hash_short for the initial masterchain validator set.

    Matches C++ store_validator_list_hash → do_compute_validator_set →
    compute_validator_set_hash chain for cc_seqno=0, masterchain shard.
    """
    cc_seqno = 0
    workchain = -1
    shard = 0x8000_0000_0000_0000  # masterchain shard

    count = len(validator_keys)
    if count == 0:
        return 0

    # Build the ordered validator list (pubkey, weight=17, adnl_addr=0)
    validators = [(key.public_key.key, 17, b"\x00" * 32) for key in validator_keys]

    # Shuffle for masterchain (Fisher-Yates using ValidatorSetPRNG)
    if shuffle and count > 1:
        prng = _ValidatorSetPRNG(workchain, shard, cc_seqno)
        idx = list(range(count))
        for i in range(count):
            j = prng.next_ranged(i + 1)
            idx[i], idx[j] = idx[j], idx[i]
        validators = [validators[idx[i]] for i in range(count)]

    # Serialize in TL format (little-endian) and compute CRC32C
    buf = struct.pack("<i", -1877581587)  # magic
    buf += struct.pack("<i", cc_seqno)
    buf += struct.pack("<I", count)
    for pubkey, weight, adnl_addr in validators:
        buf += pubkey  # 32 bytes, raw
        buf += struct.pack("<Q", weight)
        buf += adnl_addr  # 32 bytes
    h = crc32c(buf, byteorder="little")
    return int.from_bytes(h, "little")
