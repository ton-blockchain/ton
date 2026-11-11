"""Tests for the hand-written augmentations spliced into block.generated."""

from bitarray import bitarray
from block.generated import (
    AccountDispatchQueue,
    AccountType,
    CommonMsgInfo,
    DepthBalanceAug,
    DispatchQueueAug,
    EnqueuedMsg,
    KeyExtBlkRef,
    KeyMaxLt,
    KeyMaxLtAug,
    MsgEnvelopeType,
    OutMsgQueueAug,
    account,
    account_descr,
    account_none,
    account_storage,
    account_uninit,
    addr_none,
    addr_std,
    anycast_info,
    currencies,
    depth_balance,
    ext_blk_ref,
    ext_in_msg_info,
    ext_out_msg_info,
    extra_currencies,
    interm_addr_regular,
    left,
    message,
    msg_envelope,
    msg_envelope_v2,
    nanograms,
    storage_extra_none,
    storage_info,
    storage_used,
)
from pytoniq_core import Builder, Slice
from tlb.hashmap import HashmapDict
from tlb.object import AnyType, Ref

# ── Fixtures ────────────────────────────────────────────────────────────


def _bits(n: int, width: int = 256) -> bitarray:
    out = bitarray(width)
    out.setall(0)
    for i in range(width):
        if (n >> (width - 1 - i)) & 1:
            out[i] = 1
    return out


def _balance(grams: int = 0, extras: dict[int, int] | None = None) -> currencies:
    return currencies(
        grams=nanograms(amount=grams),
        other=extra_currencies(extras or {}),
    )


def _shard_account_none() -> account_descr:
    return account_descr(
        account=Ref(AccountType(), account_none()),
        last_trans_hash=_bits(0),
        last_trans_lt=0,
    )


def _shard_account_active(
    *,
    grams: int = 0,
    extras: dict[int, int] | None = None,
    anycast_depth: int | None = None,
) -> account_descr:
    anycast = (
        anycast_info(depth=anycast_depth, rewrite_pfx=bitarray("0" * anycast_depth))
        if anycast_depth is not None
        else None
    )
    acc = account(
        addr=addr_std(anycast=anycast, workchain_id=0, address=_bits(1)),
        storage_stat=storage_info(
            used=storage_used(cells=0, bits=0),
            storage_extra=storage_extra_none(),
            last_paid=0,
            due_payment=None,
        ),
        storage=account_storage(
            last_trans_lt=0,
            balance=_balance(grams=grams, extras=extras),
            state=account_uninit(),
        ),
    )
    return account_descr(
        account=Ref(AccountType(), acc),
        last_trans_hash=_bits(0),
        last_trans_lt=0,
    )


def _make_addr_std(workchain: int = 0, address_int: int = 1) -> addr_std:
    return addr_std(anycast=None, workchain_id=workchain, address=_bits(address_int))


def _make_message(*, info: CommonMsgInfo) -> message[Slice]:
    """Build a Message[Slice] with a trivial empty-slice body and no init."""
    empty_slice = Builder().end_cell().begin_parse()
    return message[Slice](
        _tX=AnyType,
        info=info,
        init=None,
        body=left(_tX=AnyType, value=empty_slice),
    )


def _make_message_ref(info: CommonMsgInfo) -> Ref[message[Slice]]:
    return Ref(message[Slice].instantiate(AnyType), _make_message(info=info))


def _msg_envelope_v2(
    *,
    emitted_lt: int | None,
    inner_info: CommonMsgInfo | None = None,
) -> msg_envelope_v2:
    info = inner_info if inner_info is not None else _ext_out_info(0)
    return msg_envelope_v2(
        cur_addr=interm_addr_regular(use_dest_bits=0),
        next_addr=interm_addr_regular(use_dest_bits=0),
        fwd_fee_remaining=nanograms(amount=0),
        msg=_make_message_ref(info),
        emitted_lt=emitted_lt,
        metadata=None,
    )


def _msg_envelope_old(
    *,
    inner_info: CommonMsgInfo | None = None,
) -> msg_envelope:
    info = inner_info if inner_info is not None else _ext_out_info(0)
    return msg_envelope(
        cur_addr=interm_addr_regular(use_dest_bits=0),
        next_addr=interm_addr_regular(use_dest_bits=0),
        fwd_fee_remaining=nanograms(amount=0),
        msg=_make_message_ref(info),
    )


def _ext_out_info(created_lt: int) -> ext_out_msg_info:
    return ext_out_msg_info(
        src=_make_addr_std(),
        dest=addr_none(),
        created_lt=created_lt,
        created_at=0,
    )


def _ext_in_info() -> ext_in_msg_info:
    return ext_in_msg_info(
        src=addr_none(),
        dest=_make_addr_std(),
        import_fee=nanograms(amount=0),
    )


def _enqueued(enqueued_lt: int, envelope: msg_envelope | msg_envelope_v2) -> EnqueuedMsg:
    return EnqueuedMsg(
        enqueued_lt=enqueued_lt,
        out_msg=Ref(MsgEnvelopeType(), envelope),
    )


# ── DepthBalanceAug ─────────────────────────────────────────────────────


class TestDepthBalanceAug:
    def test_eval_empty(self):
        result = DepthBalanceAug().eval_empty()
        assert result.split_depth == 0
        assert result.balance.grams.amount == 0

    def test_eval_leaf_account_none(self):
        sa = _shard_account_none()
        result = DepthBalanceAug().eval_leaf(sa)
        assert result.split_depth == 0
        assert result.balance.grams.amount == 0

    def test_eval_leaf_active_no_anycast(self):
        sa = _shard_account_active(grams=12345)
        result = DepthBalanceAug().eval_leaf(sa)
        assert result.split_depth == 0
        assert result.balance.grams.amount == 12345

    def test_eval_leaf_active_with_anycast(self):
        sa = _shard_account_active(grams=500, anycast_depth=8)
        result = DepthBalanceAug().eval_leaf(sa)
        assert result.split_depth == 8
        assert result.balance.grams.amount == 500

    def test_merge_takes_max_depth(self):
        a = depth_balance(split_depth=3, balance=_balance(grams=10))
        b = depth_balance(split_depth=7, balance=_balance(grams=10))
        result = DepthBalanceAug().merge(a, b)
        assert result.split_depth == 7

    def test_merge_sums_grams(self):
        a = depth_balance(split_depth=0, balance=_balance(grams=100))
        b = depth_balance(split_depth=0, balance=_balance(grams=250))
        result = DepthBalanceAug().merge(a, b)
        assert result.balance.grams.amount == 350

    def test_merge_sums_extras_per_currency(self):
        a = depth_balance(split_depth=0, balance=_balance(grams=0, extras={1: 50, 2: 100}))
        b = depth_balance(split_depth=0, balance=_balance(grams=0, extras={1: 25, 3: 7}))
        merged = DepthBalanceAug().merge(a, b).balance.other.dict
        assert merged[1] == 75
        assert merged[2] == 100
        assert merged[3] == 7


# ── OutMsgQueueAug ──────────────────────────────────────────────────────


class TestOutMsgQueueAug:
    def test_eval_empty(self):
        assert OutMsgQueueAug().eval_empty() == 0

    def test_merge_picks_min(self):
        aug = OutMsgQueueAug()
        assert aug.merge(100, 50) == 50
        assert aug.merge(50, 100) == 50
        assert aug.merge(7, 7) == 7

    def test_eval_leaf_v2_envelope_with_emitted_lt(self):
        env = _msg_envelope_v2(emitted_lt=42, inner_info=_ext_out_info(7))
        msg = _enqueued(enqueued_lt=10, envelope=env)
        # v2's emitted_lt wins over the inner message's created_lt.
        assert OutMsgQueueAug().eval_leaf(msg) == 42

    def test_eval_leaf_v2_envelope_no_emitted_lt_uses_created_lt(self):
        env = _msg_envelope_v2(emitted_lt=None, inner_info=_ext_out_info(123))
        msg = _enqueued(enqueued_lt=10, envelope=env)
        # Falls back to MsgEnvelope::get_emitted_lt's behavior: read the
        # inner message's created_lt.
        assert OutMsgQueueAug().eval_leaf(msg) == 123

    def test_eval_leaf_old_envelope_uses_created_lt(self):
        env = _msg_envelope_old(inner_info=_ext_out_info(456))
        msg = _enqueued(enqueued_lt=99, envelope=env)
        assert OutMsgQueueAug().eval_leaf(msg) == 456

    def test_eval_leaf_ext_in_msg_yields_zero(self):
        # ext_in_msg_info has no created_lt — fallback returns 0.
        env = _msg_envelope_old(inner_info=_ext_in_info())
        msg = _enqueued(enqueued_lt=99, envelope=env)
        assert OutMsgQueueAug().eval_leaf(msg) == 0


# ── DispatchQueueAug ────────────────────────────────────────────────────


class TestDispatchQueueAug:
    def test_eval_empty(self):
        assert DispatchQueueAug().eval_empty() == 0

    def test_merge_picks_min(self):
        assert DispatchQueueAug().merge(50, 100) == 50

    def test_eval_leaf_empty_messages_returns_uint64_max(self):
        # Empty inner dict → (1<<64)-1 so a min-merge with non-empty
        # siblings still surfaces the real minimum lt.
        adq = AccountDispatchQueue(
            messages=HashmapDict(64, EnqueuedMsg, allow_empty=True),
            count=0,
        )
        assert DispatchQueueAug().eval_leaf(adq) == (1 << 64) - 1

    def test_eval_leaf_picks_smallest_key(self):
        # The inner dict's keys ARE the per-message lts; aug returns the min.
        d: HashmapDict[EnqueuedMsg, None] = HashmapDict(64, EnqueuedMsg, allow_empty=True)
        d[1000] = _enqueued(1000, _msg_envelope_v2(emitted_lt=None))
        d[42] = _enqueued(42, _msg_envelope_v2(emitted_lt=None))
        d[500] = _enqueued(500, _msg_envelope_v2(emitted_lt=None))
        adq = AccountDispatchQueue(messages=d, count=3)
        assert DispatchQueueAug().eval_leaf(adq) == 42


# ── KeyMaxLtAug ─────────────────────────────────────────────────────────


def _key_ext_blk_ref(*, key: bool, end_lt: int) -> KeyExtBlkRef:
    return KeyExtBlkRef(
        key=key,
        blk_ref=ext_blk_ref(
            end_lt=end_lt,
            seq_no=0,
            root_hash=_bits(0),
            file_hash=_bits(0),
        ),
    )


class TestKeyMaxLtAug:
    def test_eval_empty(self):
        result = KeyMaxLtAug().eval_empty()
        assert result.key is False
        assert result.max_end_lt == 0

    def test_eval_leaf_copies_fields(self):
        ke = _key_ext_blk_ref(key=True, end_lt=999)
        result = KeyMaxLtAug().eval_leaf(ke)
        assert result.key is True
        assert result.max_end_lt == 999

    def test_merge_ors_keys_and_takes_max_lt(self):
        a = KeyMaxLt(key=True, max_end_lt=5)
        b = KeyMaxLt(key=False, max_end_lt=10)
        result = KeyMaxLtAug().merge(a, b)
        assert result.key is True
        assert result.max_end_lt == 10

    def test_merge_both_false_keys(self):
        a = KeyMaxLt(key=False, max_end_lt=3)
        b = KeyMaxLt(key=False, max_end_lt=8)
        result = KeyMaxLtAug().merge(a, b)
        assert result.key is False
        assert result.max_end_lt == 8


# ── End-to-end: HashmapDict with a real aug round-trips ─────────────────


class TestMappingInit:
    """Generated dataclasses with HashmapDict fields accept a plain Mapping
    in __init__; HashmapDict.of normalizes to a real HashmapDict."""

    def test_empty_dict_for_simple_hashmap_field(self):
        from block.generated import ShardHashes

        sh = ShardHashes(field={})
        assert sh.field.is_empty()

    def test_pre_built_hashmap_passes_through(self):
        from block.generated import ProcessedInfo, processed_upto
        from tlb.hashmap import HashmapDict

        d: HashmapDict[processed_upto, None] = HashmapDict(96, processed_upto)
        info = ProcessedInfo(field=d)
        assert info.field is d

    def test_augmented_field_accepts_dict(self):
        from block.generated import OldMcBlocksInfo

        info = OldMcBlocksInfo(field={})
        assert info.field.is_empty()
        # The aug must still be wired correctly so serialize_to passes.
        b = Builder()
        info.serialize_to(b)


class TestHashmapDictWithAug:
    def test_shard_accounts_roundtrip_extra_matches(self):
        """Insert several entries into a HashmapDict[ShardAccount,
        DepthBalanceInfo] backed by DepthBalanceAug, serialize, and read
        back the outer extra from the resulting cell. The extra must equal
        the augmentation merged across all leaves."""
        d: HashmapDict[account_descr, depth_balance] = HashmapDict(
            256, account_descr, depth_balance, DepthBalanceAug()
        )
        d[1] = _shard_account_active(grams=100, anycast_depth=4)
        d[2] = _shard_account_active(grams=250)
        d[3] = _shard_account_none()

        b = Builder()
        d.serialize_to(b)
        cs = b.end_cell().begin_parse()
        assert cs.load_bit() == 1  # non-empty (ahme_root form)
        _ = cs.load_ref()  # root edge cell
        outer_extra = depth_balance.load_from(cs)

        # The outer extra must equal merge(merge(leaf1, leaf2), leaf3) in
        # any associative order — DepthBalanceAug.merge is associative.
        aug = DepthBalanceAug()
        expected = aug.merge(
            aug.merge(aug.eval_leaf(d[1]), aug.eval_leaf(d[2])),
            aug.eval_leaf(d[3]),
        )
        assert outer_extra.split_depth == expected.split_depth
        assert outer_extra.balance.grams.amount == expected.balance.grams.amount
