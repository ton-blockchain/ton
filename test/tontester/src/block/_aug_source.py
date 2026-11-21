"""Augmentation implementations for HashmapAugE types used in block.tlb.

This module is INPUT-ONLY for the codegen — it is never imported at
runtime. The codegen reads its source at generation time and splices the
class/function definitions into block/generated.py, where they reference
generated types directly. The `from .generated import …` line below is
present purely so basedpyright can type-check this file; it is stripped
during the splice.

These match the C++ augmentation logic in crypto/block/block-parse.cpp.
"""

from typing import final, override

from tlb.hashmap import Augmentation, HashmapDict
from tlb.object import TypeInfo, UintTypeConstructor, VarUIntTypeConstructor

from .generated import (
    AccountDispatchQueue,
    CurrencyCollection,
    DepthBalanceInfo,
    EnqueuedMsg,
    KeyExtBlkRef,
    KeyMaxLt,
    ShardAccount,
    account,
    account_descr,
    currencies,
    depth_balance,
    ext_out_msg_info,
    extra_currencies,
    int_msg_info,
    msg_envelope_v2,
    nanograms,
)

_UINT64_MAX = (1 << 64) - 1


def _zero_cc() -> CurrencyCollection:
    return currencies(
        grams=nanograms(amount=0),
        other=extra_currencies(dict=HashmapDict(32, VarUIntTypeConstructor(32))),
    )


def _add_extras(a: extra_currencies, b: extra_currencies) -> extra_currencies:
    """Sum two ExtraCurrencyCollection values per currency id."""
    merged: HashmapDict[int, None] = HashmapDict(32, VarUIntTypeConstructor(32))
    for key, value in a.dict.items():
        merged[key] = value
    for key, value in b.dict.items():
        if key in merged:
            merged[key] = merged[key] + value
        else:
            merged[key] = value
    return extra_currencies(dict=merged)


def _add_cc(a: CurrencyCollection, b: CurrencyCollection) -> CurrencyCollection:
    """Add two CurrencyCollection values: sum grams and sum extras per currency."""
    return currencies(
        grams=nanograms(amount=a.grams.amount + b.grams.amount),
        other=_add_extras(a.other, b.other),
    )


@final
class DepthBalanceAug(Augmentation[ShardAccount, DepthBalanceInfo]):
    """ShardAccounts: HashmapAugE 256 ShardAccount DepthBalanceInfo.

    eval_leaf: split_depth from anycast (or 0), balance from account storage.
    merge: max(split_depths), sum(balances).
    """

    @property
    @override
    def extra_ti(self) -> TypeInfo[DepthBalanceInfo]:
        return depth_balance

    @override
    def eval_leaf(self, value: ShardAccount) -> DepthBalanceInfo:
        assert isinstance(value, account_descr)
        acc = value.account.ref
        if isinstance(acc, account):
            anycast = acc.addr.anycast
            split_depth = anycast.depth if anycast is not None else 0
            balance = acc.storage.balance
        else:
            split_depth = 0
            balance = _zero_cc()
        return depth_balance(split_depth=split_depth, balance=balance)

    @override
    def merge(self, left: DepthBalanceInfo, right: DepthBalanceInfo) -> DepthBalanceInfo:
        return depth_balance(
            split_depth=max(left.split_depth, right.split_depth),
            balance=_add_cc(left.balance, right.balance),
        )

    @override
    def eval_empty(self) -> DepthBalanceInfo:
        return depth_balance(split_depth=0, balance=_zero_cc())


@final
class OutMsgQueueAug(Augmentation[EnqueuedMsg, int]):
    """OutMsgQueue: HashmapAugE 352 EnqueuedMsg uint64.

    eval_leaf mirrors `MsgEnvelope::get_emitted_lt` in block-parse.cpp:
    prefer the envelope's `emitted_lt` (v2 envelopes may carry one), else
    fall back to the inner Message's `created_lt` (carried on int_msg_info
    and ext_out_msg_info; ext_in_msg_info has none and yields 0).
    merge: min — the extra is the earliest emit time in the subtree.
    """

    @property
    @override
    def extra_ti(self) -> TypeInfo[int]:
        return UintTypeConstructor(64)

    @override
    def eval_leaf(self, value: EnqueuedMsg) -> int:
        envelope = value.out_msg.ref
        if isinstance(envelope, msg_envelope_v2) and envelope.emitted_lt is not None:
            return envelope.emitted_lt
        info = envelope.msg.ref.info
        if isinstance(info, int_msg_info | ext_out_msg_info):
            return info.created_lt
        return 0

    @override
    def merge(self, left: int, right: int) -> int:
        return min(left, right)

    @override
    def eval_empty(self) -> int:
        return 0


@final
class DispatchQueueAug(Augmentation[AccountDispatchQueue, int]):
    """DispatchQueue: HashmapAugE 256 AccountDispatchQueue uint64.

    eval_leaf mirrors `Aug_DispatchQueue::eval_leaf` in block-parse.cpp:
    return the smallest lt key in the inner messages dict, or
    (1<<64)-1 when the dict is empty so a min-merge with non-empty
    siblings still picks the real minimum.
    merge: min across subtrees.
    """

    @property
    @override
    def extra_ti(self) -> TypeInfo[int]:
        return UintTypeConstructor(64)

    @override
    def eval_leaf(self, value: AccountDispatchQueue) -> int:
        first = next(iter(value.messages.keys()), None)
        return first if first is not None else _UINT64_MAX

    @override
    def merge(self, left: int, right: int) -> int:
        return min(left, right)

    @override
    def eval_empty(self) -> int:
        return 0


@final
class KeyMaxLtAug(Augmentation[KeyExtBlkRef, KeyMaxLt]):
    """OldMcBlocksInfo: HashmapAugE 32 KeyExtBlkRef KeyMaxLt.

    eval_leaf: copy `key:Bool` and `max_end_lt:uint64` from the entry.
    merge: key1 OR key2, max(lt1, lt2).
    """

    @property
    @override
    def extra_ti(self) -> TypeInfo[KeyMaxLt]:
        return KeyMaxLt

    @override
    def eval_leaf(self, value: KeyExtBlkRef) -> KeyMaxLt:
        return KeyMaxLt(key=value.key, max_end_lt=value.blk_ref.end_lt)

    @override
    def merge(self, left: KeyMaxLt, right: KeyMaxLt) -> KeyMaxLt:
        return KeyMaxLt(
            key=left.key or right.key,
            max_end_lt=max(left.max_end_lt, right.max_end_lt),
        )

    @override
    def eval_empty(self) -> KeyMaxLt:
        return KeyMaxLt(key=False, max_end_lt=0)
