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
from tlb.object import TlbModelError, TypeInfo, UintTypeConstructor, VarUIntTypeConstructor

from .generated import (
    AccountDispatchQueue,
    CurrencyCollection,
    DepthBalanceInfo,
    DispatchQueueAugData,
    DispatchQueueAugDataType,
    EnqueuedMsg,
    KeyExtBlkRef,
    KeyMaxLt,
    ShardAccount,
    account,
    account_descr,
    account_dispatch_queue,
    currencies,
    depth_balance,
    dispatch_queue_aug,
    dispatch_queue_aug_old,
    ext_out_msg_info,
    extra_currencies,
    int_msg_info,
    msg_envelope_v2,
    nanograms,
)


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
class DispatchQueueAug(Augmentation[AccountDispatchQueue, DispatchQueueAugData]):
    """DispatchQueue: HashmapAugE 256 AccountDispatchQueue DispatchQueueAugData.

    eval_leaf returns the smallest lt key and, for the new account queue
    constructor, its stored total balance. merge keeps a total balance only
    when both children have one; otherwise it emits the legacy augmentation.
    """

    @property
    @override
    def extra_ti(self) -> TypeInfo[DispatchQueueAugData]:
        return DispatchQueueAugDataType()

    @override
    def eval_leaf(self, value: AccountDispatchQueue) -> DispatchQueueAugData:
        first = next(iter(value.messages.ref.keys()), None)
        if first is None:
            raise TlbModelError("AccountDispatchQueue messages must not be empty")
        if isinstance(value, account_dispatch_queue):
            return dispatch_queue_aug(
                min_created_lt=first,
                total_balance=value.total_balance,
            )
        return dispatch_queue_aug_old(min_created_lt=first)

    @override
    def merge(
        self, left: DispatchQueueAugData, right: DispatchQueueAugData
    ) -> DispatchQueueAugData:
        min_created_lt = min(left.min_created_lt, right.min_created_lt)
        if isinstance(left, dispatch_queue_aug) and isinstance(right, dispatch_queue_aug):
            return dispatch_queue_aug(
                min_created_lt=min_created_lt,
                total_balance=_add_cc(left.total_balance, right.total_balance),
            )
        return dispatch_queue_aug_old(min_created_lt=min_created_lt)

    @override
    def eval_empty(self) -> DispatchQueueAugData:
        return dispatch_queue_aug_old(min_created_lt=0)


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
