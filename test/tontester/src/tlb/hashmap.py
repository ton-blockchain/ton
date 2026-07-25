"""Lazy dictionary backed by a TON HashmapAug cell.

Wraps a HashmapAugE cell and provides dict-like read/write access.
Reads traverse the tree lazily. Writes are tracked in a sorted overlay
and merged during iteration and serialization.

Regular (non-augmented) hashmaps use UnitTypeInfo as the extra type,
which takes 0 bits on the wire.
"""

from collections.abc import Callable, Iterator, Mapping, Sequence
from dataclasses import dataclass
from typing import Protocol, final, override

from bitarray import bitarray
from bitarray.util import ba2int, int2ba
from pytoniq_core import Builder, Cell, Slice
from sortedcontainers import SortedDict

from .hashmap_auto import (
    HmLabel,
    ahm_edge,
    ahmn_fork,
    ahmn_leaf,
    hml_long,
    hml_same,
    hml_short,
)
from .object import InstantiableTypeInfo, TypeInfo, UnitTypeInfo, drain_slice


class _Deleted:
    """Sentinel for deleted keys in the overlay."""

    pass


_DELETED = _Deleted()


class Augmentation[V, E](Protocol):
    """Augmentation callbacks for HashmapAug."""

    @property
    def extra_ti(self) -> TypeInfo[E]: ...
    def eval_leaf(self, value: V) -> E: ...
    def merge(self, left: E, right: E) -> E: ...
    def eval_empty(self) -> E: ...


@final
class _UnitAug:
    @property
    def extra_ti(self) -> TypeInfo[None]:
        return UnitTypeInfo

    def eval_leaf(self, value: object) -> None:
        _ = value
        return None

    def merge(self, left: None, right: None) -> None:
        _ = left
        _ = right
        return None

    def eval_empty(self) -> None:
        return None


UnitAug = _UnitAug()


def _label_bits(label: HmLabel) -> bitarray:
    match label:
        case hml_short(s=s):
            return s
        case hml_long(s=s):
            return s
        case hml_same(v=v, n=n):
            return bitarray([v]) * n


def _walk_cell[V, E](
    cell: Cell,
    prefix: bitarray,
    key_bits: int,
    value_ti: TypeInfo[V],
    extra_ti: TypeInfo[E],
    skip: Callable[[bitarray], bool] | None = None,
) -> Iterator[_Leaf[V] | _Segment[E]]:
    """Walk a hashmap subtree rooted at `cell` with `prefix` already consumed.

    Yields each leaf as `_Leaf(key, value)`. If `skip(prefix)` returns True
    for a subtree, that subtree's cell is yielded as `_Segment` and not
    descended into — its `.ref` is never accessed, so pruned cells never
    force a deserialization.
    """
    if skip is not None and skip(prefix):
        yield _Segment(prefix=bitarray(prefix), cell=cell)
        return
    edge = ahm_edge[V, E].load_from(cell.begin_parse(), key_bits - len(prefix), value_ti, extra_ti)
    full = prefix + _label_bits(edge.label)
    match edge.node:
        case ahmn_leaf(value=value):
            yield _Leaf(key=ba2int(full), value=value)
        case ahmn_fork(left=l, right=r):
            yield from _walk_cell(
                l.cell, full + bitarray([False]), key_bits, value_ti, extra_ti, skip
            )
            yield from _walk_cell(
                r.cell, full + bitarray([True]), key_bits, value_ti, extra_ti, skip
            )


def _lookup_cell[V, E](
    cell: Cell,
    key: bitarray,
    pos: int,
    key_bits: int,
    value_ti: TypeInfo[V],
    extra_ti: TypeInfo[E],
) -> V | None:
    """Look up a single key, descending only the matching branch at each fork."""
    edge = ahm_edge[V, E].load_from(cell.begin_parse(), key_bits - pos, value_ti, extra_ti)
    label = _label_bits(edge.label)
    if key[pos : pos + len(label)] != label:
        return None
    pos += len(label)
    match edge.node:
        case ahmn_leaf(value=value):
            return value
        case ahmn_fork(left=l, right=r):
            child = r.cell if key[pos] else l.cell
            return _lookup_cell(child, key, pos + 1, key_bits, value_ti, extra_ti)


def _merge_sorted[V](
    tree: Iterator[tuple[int, V]],
    overlay: Iterator[tuple[int, V]],
    skip: frozenset[int],
) -> Iterator[tuple[int, V]]:
    """Merge two sorted iterators, skipping keys in `skip` from the tree iterator."""
    t = next(tree, None)
    o = next(overlay, None)
    while t is not None or o is not None:
        if t is not None and t[0] in skip:
            t = next(tree, None)
            continue
        if t is None:
            assert o is not None
            yield o
            o = next(overlay, None)
        elif o is None:
            yield t
            t = next(tree, None)
        elif t[0] < o[0]:
            yield t
            t = next(tree, None)
        elif t[0] > o[0]:
            yield o
            o = next(overlay, None)
        else:
            yield o
            o = next(overlay, None)
            t = next(tree, None)


def _serialize_label(builder: Builder, label: bitarray, max_len: int) -> None:
    # k = ceil(log2(max_len + 1))
    # hml_short ('0'): 2n + 2 bits
    # hml_long  ('10'): 2 + k + n bits
    # hml_same  ('11'): 3 + k bits

    n = len(label)
    k = max_len.bit_length() if max_len > 0 else 0
    is_same = n > 0 and (label.all() or not label.any())
    if is_same and n > 1 and k < 2 * n - 1:
        hml_same(max_len, v=bool(label[0]), n=n).serialize_to(builder)
    elif k < n:
        hml_long(max_len, n=n, s=label).serialize_to(builder)
    else:
        hml_short(max_len, n, len=n, s=label).serialize_to(builder)


@final
class HashmapDict[V, E = None]:
    """Lazy dictionary backed by a HashmapAugE n X Y cell.

    Reads traverse the tree on demand from the source cell. Writes go into
    a sorted overlay (SortedDict) and are merged with the tree during
    iteration and serialization.
    """

    def __init__(
        self,
        key_bits: int,
        value_ti: TypeInfo[V],
        extra_ti: TypeInfo[E] = UnitTypeInfo,
        aug: Augmentation[V, E] = UnitAug,
        cell: Cell | None = None,
        allow_empty: bool = True,
    ) -> None:
        self._key_bits = key_bits
        self._value_ti = value_ti
        self._extra_ti = extra_ti
        self._aug = aug
        self._cell = cell
        self._allow_empty = allow_empty
        self._overlay: SortedDict[int, V | _Deleted] = SortedDict()

    @property
    def key_bits(self) -> int:
        return self._key_bits

    @property
    def value_ti(self) -> TypeInfo[V]:
        return self._value_ti

    @property
    def extra_ti(self) -> TypeInfo[E]:
        return self._extra_ti

    @property
    def aug(self) -> Augmentation[V, E]:
        return self._aug

    @property
    def allow_empty(self) -> bool:
        return self._allow_empty

    def _key_ba(self, key: int) -> bitarray:
        return int2ba(key, self._key_bits)

    def _tree_iter(self) -> Iterator[tuple[int, V]]:
        if self._cell is None:
            return
        for item in _walk_cell(
            self._cell, bitarray(), self._key_bits, self._value_ti, self._extra_ti
        ):
            assert isinstance(item, _Leaf)
            yield (item.key, item.value)

    def _overlay_live(self) -> Iterator[tuple[int, V]]:
        for k, v in self._overlay.items():
            if not isinstance(v, _Deleted):
                yield (k, v)

    def is_empty(self) -> bool:
        return next(self.items(), None) is None

    def _lookup(self, key: int) -> V | None:
        if self._cell is None:
            return None
        return _lookup_cell(
            self._cell, self._key_ba(key), 0, self._key_bits, self._value_ti, self._extra_ti
        )

    def __getitem__(self, key: int) -> V:
        if key in self._overlay:
            val = self._overlay[key]
            if isinstance(val, _Deleted):
                raise KeyError(key)
            return val
        result = self._lookup(key)
        if result is None:
            raise KeyError(key)
        return result

    def __setitem__(self, key: int, value: V) -> None:
        self._overlay[key] = value

    def __delitem__(self, key: int) -> None:
        if key not in self:
            raise KeyError(key)
        self._overlay[key] = _Deleted()

    def __contains__(self, key: object) -> bool:
        if not isinstance(key, int):
            return False
        if key in self._overlay:
            return not isinstance(self._overlay[key], _Deleted)
        return self._lookup(key) is not None

    def get(self, key: int, default: V | None = None) -> V | None:
        try:
            return self[key]
        except KeyError:
            return default

    def items(self) -> Iterator[tuple[int, V]]:
        """Iterate in sorted key order, merging tree and overlay lazily."""
        if not self._overlay:
            yield from self._tree_iter()
            return
        skip = frozenset(self._overlay.keys())
        yield from _merge_sorted(self._tree_iter(), self._overlay_live(), skip)

    def keys(self) -> Iterator[int]:
        for k, _ in self.items():
            yield k

    def values(self) -> Iterator[V]:
        for _, v in self.items():
            yield v

    def __iter__(self) -> Iterator[int]:
        return self.keys()

    def __len__(self) -> int:
        return sum(1 for _ in self.items())

    def to_dict(self) -> dict[int, V]:
        return dict(self.items())

    def serialize_to(self, builder: Builder) -> None:
        """Serialize the hashmap.

        HashmapAugE (allow_empty): $0 extra for empty, $1 ^root extra for non-empty.
        HashmapAug (!allow_empty): root edge directly in the builder.
        """
        if self._allow_empty:
            self._serialize_hashmap_e(builder)
        else:
            self._serialize_hashmap(builder)

    def _serialize_hashmap_e(self, builder: Builder) -> None:
        items = self._collect_items()
        if not items:
            _ = builder.store_uint(0, 1)
            self._aug.extra_ti.serialize_value(self._aug.eval_empty(), builder)
            return
        root_cell, root_extra = _build_from_items(
            items, 0, self._key_bits, self._value_ti, self._aug
        )
        _ = builder.store_uint(1, 1)
        _ = builder.store_ref(root_cell)
        self._aug.extra_ti.serialize_value(root_extra, builder)

    def _serialize_hashmap(self, builder: Builder) -> None:
        items = self._collect_items()
        assert items, "non-empty Hashmap cannot be serialized as empty"
        root_cell, _ = _build_from_items(items, 0, self._key_bits, self._value_ti, self._aug)
        _ = builder.store_slice(root_cell.begin_parse())

    def _collect_items(self) -> list[_Item[V, E]]:
        """Merge tree leaves and overlay-live entries into a single sorted list.

        Subtrees with no overlay activity in their key range are emitted as
        opaque `_Segment`s — their cells are reused without ever accessing
        `.ref`, so pruned cells outside the overlay's reach are never
        deserialized.
        """

        def overlay_misses(prefix: bitarray) -> bool:
            pad = self._key_bits - len(prefix)
            low = ba2int(prefix + bitarray([False] * pad)) if pad else ba2int(prefix)
            high = ba2int(prefix + bitarray([True] * pad)) if pad else low
            return next(self._overlay.irange(low, high), None) is None

        items: list[_Item[V, E]] = []
        if self._cell is not None:
            for item in _walk_cell(
                self._cell,
                bitarray(),
                self._key_bits,
                self._value_ti,
                self._extra_ti,
                skip=overlay_misses,
            ):
                # Overlay-superseded leaves are dropped; the live overlay
                # value will be added below.
                if isinstance(item, _Leaf) and item.key in self._overlay:
                    continue
                items.append(item)
        for k, v in self._overlay.items():
            if not isinstance(v, _Deleted):
                items.append(_Leaf(key=k, value=v))
        items.sort(key=lambda i: _sort_key(i, self._key_bits))
        return items

    @classmethod
    def load_from(
        cls,
        cs: Slice,
        key_bits: int,
        value_ti: TypeInfo[V],
        extra_ti: TypeInfo[E] = UnitTypeInfo,
        allow_empty: bool = True,
        aug: Augmentation[V, E] = UnitAug,
    ) -> HashmapDict[V, E]:
        """Load a hashmap from a slice.

        allow_empty=True: HashmapAugE format ($0 extra for empty, $1 ^root extra)
        allow_empty=False: HashmapAug format (root edge directly from cs)
        """
        if allow_empty:
            if cs.load_bit():
                cell = cs.load_ref()
                _ = extra_ti.load_from(cs)
                return cls(key_bits, value_ti, extra_ti, aug, cell, allow_empty=True)
            _ = extra_ti.load_from(cs)
            return cls(key_bits, value_ti, extra_ti, aug, allow_empty=True)
        else:
            b = Builder()
            _ = b.store_slice(cs)
            drain_slice(cs)
            return cls(key_bits, value_ti, extra_ti, aug, b.end_cell(), allow_empty=False)

    @classmethod
    def type_info(
        cls,
        key_bits: int,
        value_ti: TypeInfo[V],
        allow_empty: bool = True,
    ) -> TypeInfo[HashmapDict[V]]:
        """Create a TypeInfo for HashmapDict — used by Ref[Hashmap] and generics."""
        return HashmapDictTypeInfo[V]().instantiate(key_bits, value_ti, allow_empty)

    @classmethod
    def of(
        cls,
        src: HashmapDict[V, E] | Mapping[int, V],
        *,
        key_bits: int,
        value_ti: TypeInfo[V],
        extra_ti: TypeInfo[E] = UnitTypeInfo,
        aug: Augmentation[V, E] = UnitAug,
        allow_empty: bool = True,
    ) -> HashmapDict[V, E]:
        """Coerce a HashmapDict-or-Mapping into a HashmapDict.

        If `src` is already a HashmapDict it is returned unchanged. Otherwise
        the entries are copied into a fresh HashmapDict with the given
        configuration. Used by generated dataclass constructors so callers
        can pass a plain `dict` for empty/small dict fields.
        """
        if isinstance(src, HashmapDict):
            return src
        d = cls(key_bits, value_ti, extra_ti, aug, allow_empty=allow_empty)
        for k, v in src.items():
            d[k] = v
        return d


@final
class HashmapDictTypeInfo[V](InstantiableTypeInfo[HashmapDict[V], int, TypeInfo[V], bool]):
    @override
    def serialize_value(self, value: HashmapDict[V], builder: Builder) -> None:
        value.serialize_to(builder)

    @override
    def load_from(
        self,
        cs: Slice,
        key_bits: int,
        value_ti: TypeInfo[V],
        allow_empty: bool,
    ) -> HashmapDict[V]:
        return HashmapDict[V].load_from(cs, key_bits, value_ti, allow_empty=allow_empty)

    @override
    def __repr__(self):
        return "HashmapDict"


@dataclass(frozen=True)
class _Leaf[V]:
    """A (key, value) pair to be embedded in the rebuilt tree."""

    key: int
    value: V


@dataclass(frozen=True)
class _Segment[E]:
    """An opaque subtree to be embedded as-is by reusing its cell.

    `prefix` is the bit-path from the root to this segment's start (the cell's
    own internal label is not part of `prefix`).
    """

    prefix: bitarray
    cell: Cell


type _Item[V, E] = _Leaf[V] | _Segment[E]


def _sort_key[V, E](item: _Item[V, E], key_bits: int) -> bitarray:
    if isinstance(item, _Leaf):
        return int2ba(item.key, key_bits)
    return item.prefix + bitarray([False] * (key_bits - len(item.prefix)))


def _item_bits[V, E](item: _Item[V, E], key_bits: int) -> bitarray:
    """The known bit-prefix of an item: full key for leaves, prefix for segments."""
    if isinstance(item, _Leaf):
        return int2ba(item.key, key_bits)
    return item.prefix


def _build_from_items[V, E](
    items: Sequence[_Item[V, E]],
    pos: int,
    key_bits: int,
    value_ti: TypeInfo[V],
    aug: Augmentation[V, E],
) -> tuple[Cell, E]:
    """Build a hashmap edge cell from a sorted list of leaves and segments."""
    assert items, "cannot build from empty items"

    # Single item: a leaf becomes a fresh leaf cell; a segment is plugged in
    # by reusing its cell directly.
    if len(items) == 1:
        item = items[0]
        if isinstance(item, _Segment):
            assert len(item.prefix) == pos
            if isinstance(aug, _UnitAug):
                return (item.cell, None)  # pyright: ignore[reportReturnType]
            edge = ahm_edge[V, E].load_from(
                item.cell.begin_parse(), key_bits - pos, value_ti, aug.extra_ti
            )
            return (item.cell, edge.node.extra)
        b = Builder()
        _serialize_label(b, int2ba(item.key, key_bits)[pos:], key_bits - pos)
        extra = aug.eval_leaf(item.value)
        aug.extra_ti.serialize_value(extra, b)
        value_ti.serialize_value(item.value, b)
        return (b.end_cell(), extra)

    # Multiple items — find longest common prefix, then split at next bit.
    bits = [_item_bits(it, key_bits) for it in items]
    label_len = 0
    while True:
        bit_pos = pos + label_len
        if any(bit_pos >= len(b) for b in bits):
            break
        first = bits[0][bit_pos]
        if any(b[bit_pos] != first for b in bits[1:]):
            break
        label_len += 1
    label = bits[0][pos : pos + label_len]
    split_pos = pos + label_len
    split_idx = next(i for i, b in enumerate(bits) if b[split_pos])

    left_cell, left_extra = _build_from_items(
        items[:split_idx], split_pos + 1, key_bits, value_ti, aug
    )
    right_cell, right_extra = _build_from_items(
        items[split_idx:], split_pos + 1, key_bits, value_ti, aug
    )
    b = Builder()
    _serialize_label(b, label, key_bits - pos)
    _ = b.store_ref(left_cell)
    _ = b.store_ref(right_cell)
    extra = aug.merge(left_extra, right_extra)
    aug.extra_ti.serialize_value(extra, b)
    return (b.end_cell(), extra)
