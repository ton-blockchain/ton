"""Tests for generated Python code from TL-B generics schema."""

from generated.generics import (
    EitherType,
    MaybeType,
    just,
    left,
    maybe_ref,
    maybe_uint32,
    nothing,
    pair,
    pair_maybe,
    pair_ref,
    ref_maybe,
    right,
    swapped,
    wrapper,
)
from tlb.object import Ref, UintTypeConstructor

# ── Generic types ─────────────────────────────────────────────────────


class TestMaybe:
    def test_nothing_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        obj = nothing()
        cell = obj.serialize()
        result = MaybeType[int]().load_from(cell.begin_parse(), uint32_ti)
        assert isinstance(result, nothing)

    def test_just_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        obj = just[int](uint32_ti, value=42)
        cell = obj.serialize()
        result = MaybeType[int]().load_from(cell.begin_parse(), uint32_ti)
        assert isinstance(result, just)
        assert result.value == 42

    def test_nothing_bit_count(self):
        assert nothing().serialize().begin_parse().remaining_bits == 1

    def test_just_bit_count(self):
        uint32_ti = UintTypeConstructor(32)
        assert just[int](uint32_ti, value=0).serialize().begin_parse().remaining_bits == 33


class TestEither:
    def test_left_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        int8_ti = UintTypeConstructor(8)
        obj = left[int](uint32_ti, value=42)
        result = EitherType[int, int]().load_from(obj.serialize().begin_parse(), uint32_ti, int8_ti)
        assert isinstance(result, left)
        assert result.value == 42

    def test_right_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        int8_ti = UintTypeConstructor(8)
        obj = right[int](int8_ti, value=99)
        result = EitherType[int, int]().load_from(obj.serialize().begin_parse(), uint32_ti, int8_ti)
        assert isinstance(result, right)
        assert result.value == 99


class TestPair:
    def test_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        int8_ti = UintTypeConstructor(8)
        obj = pair[int, int](uint32_ti, int8_ti, first=42, second=7)
        result = pair[int, int].load_from(obj.serialize().begin_parse(), uint32_ti, int8_ti)
        assert isinstance(result, pair)
        assert result.first == 42
        assert result.second == 7


class TestWrapper:
    def test_wrapper_just(self):
        """Wrapper T = { inner: Maybe T }. Test with T=uint32, inner=just."""
        uint32_ti = UintTypeConstructor(32)
        inner: just[int] = just[int](uint32_ti, value=42)
        obj = wrapper[int](uint32_ti, inner=inner)
        result = wrapper[int].load_from(obj.serialize().begin_parse(), uint32_ti)
        assert isinstance(result, wrapper)
        assert isinstance(result.inner, just)
        assert result.inner.value == 42

    def test_wrapper_nothing(self):
        """Wrapper T with inner=nothing."""
        uint32_ti = UintTypeConstructor(32)
        inner: nothing = nothing()
        obj = wrapper[int](uint32_ti, inner=inner)
        result = wrapper[int].load_from(obj.serialize().begin_parse(), uint32_ti)
        assert isinstance(result, wrapper)
        assert isinstance(result.inner, nothing)


class TestMaybeUint32:
    """Concrete generic instantiation: Maybe uint32 as a field."""

    def test_nothing_roundtrip(self):
        inner: nothing = nothing()
        obj = maybe_uint32(inner=inner)
        result = maybe_uint32.load_from(obj.serialize().begin_parse())
        assert isinstance(result, maybe_uint32)
        assert isinstance(result.inner, nothing)

    def test_just_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        inner: just[int] = just[int](uint32_ti, value=42)
        obj = maybe_uint32(inner=inner)
        result = maybe_uint32.load_from(obj.serialize().begin_parse())
        assert isinstance(result, maybe_uint32)
        assert isinstance(result.inner, just)
        assert result.inner.value == 42


class TestPairMaybe:
    """Nested generic: Pair (Maybe T) uint32."""

    def test_roundtrip_with_just(self):
        uint32_ti = UintTypeConstructor(32)
        maybe_ti = MaybeType[int].instantiate(uint32_ti)
        inner_pair = pair[nothing | just[int], int](
            maybe_ti, uint32_ti, first=just[int](uint32_ti, value=99), second=7
        )
        obj = pair_maybe[int](uint32_ti, value=inner_pair)
        result = pair_maybe[int].load_from(obj.serialize().begin_parse(), uint32_ti)
        assert isinstance(result, pair_maybe)
        assert isinstance(result.value, pair)
        assert isinstance(result.value.first, just)
        assert result.value.first.value == 99
        assert result.value.second == 7

    def test_roundtrip_with_nothing(self):
        uint32_ti = UintTypeConstructor(32)
        maybe_ti = MaybeType[int].instantiate(uint32_ti)
        inner_pair = pair[nothing | just[int], int](
            maybe_ti, uint32_ti, first=nothing(), second=123
        )
        obj = pair_maybe[int](uint32_ti, value=inner_pair)
        result = pair_maybe[int].load_from(obj.serialize().begin_parse(), uint32_ti)
        assert isinstance(result, pair_maybe)
        assert isinstance(result.value, pair)
        assert isinstance(result.value.first, nothing)
        assert result.value.second == 123


# ── Cell ref + generic combinations ──────────────────────────────────


class TestMaybeRef:
    """Maybe ^uint8: concrete cell ref as generic arg."""

    def test_just_roundtrip(self):
        uint8_ti = UintTypeConstructor(8)
        ref_ti = Ref[int].instantiate(uint8_ti)
        inner: just[Ref[int]] = just[Ref[int]](ref_ti, value=Ref(uint8_ti, 42))
        obj = maybe_ref(inner=inner)
        result = maybe_ref.load_from(obj.serialize().begin_parse())
        assert isinstance(result, maybe_ref)
        assert isinstance(result.inner, just)
        assert result.inner.value.ref == 42

    def test_nothing_roundtrip(self):
        obj = maybe_ref(inner=nothing())
        result = maybe_ref.load_from(obj.serialize().begin_parse())
        assert isinstance(result, maybe_ref)
        assert isinstance(result.inner, nothing)


class TestRefMaybe:
    """^(Maybe T): cell ref to a generic type."""

    def test_just_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        maybe_ti = MaybeType[int].instantiate(uint32_ti)
        inner_val: just[int] = just[int](uint32_ti, value=77)
        obj = ref_maybe[int](uint32_ti, inner=Ref(maybe_ti, inner_val))
        result = ref_maybe[int].load_from(obj.serialize().begin_parse(), uint32_ti)
        assert isinstance(result, ref_maybe)
        deref = result.inner.ref
        assert isinstance(deref, just)
        assert deref.value == 77

    def test_nothing_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        maybe_ti = MaybeType[int].instantiate(uint32_ti)
        obj = ref_maybe[int](uint32_ti, inner=Ref(maybe_ti, nothing()))
        result = ref_maybe[int].load_from(obj.serialize().begin_parse(), uint32_ti)
        assert isinstance(result, ref_maybe)
        assert isinstance(result.inner.ref, nothing)


class TestPairRef:
    """Pair ^T uint32: generic type param inside cell ref as generic arg."""

    def test_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        ref_ti = Ref[int].instantiate(uint32_ti)
        inner_pair = pair[Ref[int], int](ref_ti, uint32_ti, first=Ref(uint32_ti, 99), second=7)
        obj = pair_ref[int](uint32_ti, value=inner_pair)
        result = pair_ref[int].load_from(obj.serialize().begin_parse(), uint32_ti)
        assert isinstance(result, pair_ref)
        assert isinstance(result.value, pair)
        assert result.value.first.ref == 99
        assert result.value.second == 7


class TestSwapped:
    """Swapped X Y: fields use Y before X, but type params are in X, Y order."""

    def test_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        uint8_ti = UintTypeConstructor(8)
        obj = swapped[int, int](uint32_ti, uint8_ti, second=7, first=42)
        result = swapped[int, int].load_from(obj.serialize().begin_parse(), uint32_ti, uint8_ti)
        assert isinstance(result, swapped)
        assert result.first == 42
        assert result.second == 7
