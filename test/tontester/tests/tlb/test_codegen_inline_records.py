"""Tests for generated code with inline records ([field:Type ...])."""

from generated.inline_records import (
    Anon_1,
    Anon_2,
    Anon_3,
    Anon_4,
    Anon_5,
    bare_unnamed,
    generic_inline,
    ref_inline,
    ref_unnamed,
    with_inline,
)
from tlb.object import Ref, UintTypeConstructor


class TestWithInline:
    """Simple inline record: inner:[a:uint32 b:uint8]."""

    def test_roundtrip(self):
        inner = Anon_1(a=42, b=7)
        obj = with_inline(inner=inner)
        result = with_inline.load_from(obj.serialize().begin_parse())
        assert isinstance(result, with_inline)
        assert isinstance(result.inner, Anon_1)
        assert result.inner.a == 42
        assert result.inner.b == 7

    def test_bit_count(self):
        inner = Anon_1(a=0, b=0)
        assert with_inline(inner=inner).serialize().begin_parse().remaining_bits == 40  # 32 + 8


class TestRefInline:
    """Cell ref to inline record: inner:^[x:uint32 y:uint32]."""

    def test_roundtrip(self):
        inner = Anon_2(x=100, y=200)
        obj = ref_inline(inner=Ref(Anon_2, inner))
        result = ref_inline.load_from(obj.serialize().begin_parse())
        assert isinstance(result, ref_inline)
        assert result.inner.ref.x == 100
        assert result.inner.ref.y == 200

    def test_structure(self):
        """Outer cell has 0 data bits + 1 ref, inner cell has 64 bits."""
        inner = Anon_2(x=0, y=0)
        cell = ref_inline(inner=Ref(Anon_2, inner)).serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 0
        assert cs.remaining_refs == 1
        ref_cs = cs.load_ref().begin_parse()
        assert ref_cs.remaining_bits == 64


class TestBareUnnamed:
    """Unnamed bare inline record: _:[x:uint32 y:uint32] — properties inlined."""

    def test_property_access(self):
        obj = bare_unnamed(tag=1, field=Anon_4(x=42, y=99))
        assert obj.x == 42
        assert obj.y == 99

    def test_roundtrip_with_properties(self):
        obj = bare_unnamed(tag=5, field=Anon_4(x=10, y=20))
        result = bare_unnamed.load_from(obj.serialize().begin_parse())
        assert result.tag == 5
        assert result.x == 10
        assert result.y == 20

    def test_setter(self):
        obj = bare_unnamed(tag=1, field=Anon_4(x=1, y=2))
        obj.set_x(100)
        obj.set_y(200)
        assert obj.x == 100
        assert obj.y == 200
        # Verify underlying field is updated
        assert obj.field.x == 100
        assert obj.field.y == 200

    def test_roundtrip_after_set(self):
        obj = bare_unnamed(tag=1, field=Anon_4(x=1, y=2))
        obj.set_x(999)
        result = bare_unnamed.load_from(obj.serialize().begin_parse())
        assert result.x == 999


class TestRefUnnamed:
    """Unnamed ref inline record: _:^[a:uint32 b:uint32] — properties inlined."""

    def test_property_access(self):
        obj = ref_unnamed(tag=1, field=Ref(Anon_5, Anon_5(a=42, b=99)))
        assert obj.a == 42
        assert obj.b == 99

    def test_roundtrip_with_properties(self):
        obj = ref_unnamed(tag=7, field=Ref(Anon_5, Anon_5(a=10, b=20)))
        result = ref_unnamed.load_from(obj.serialize().begin_parse())
        assert result.tag == 7
        assert result.a == 10
        assert result.b == 20

    def test_setter(self):
        obj = ref_unnamed(tag=1, field=Ref(Anon_5, Anon_5(a=1, b=2)))
        obj.set_a(100)
        obj.set_b(200)
        assert obj.a == 100
        assert obj.b == 200

    def test_roundtrip_after_set(self):
        obj = ref_unnamed(tag=1, field=Ref(Anon_5, Anon_5(a=1, b=2)))
        obj.set_a(999)
        result = ref_unnamed.load_from(obj.serialize().begin_parse())
        assert result.a == 999


class TestGenericInline:
    """Generic inline record: {T:Type} ^([{X:Type} a:X] T)."""

    def test_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        anon_ti = Anon_3[int].instantiate(uint32_ti)
        inner_val = Anon_3[int](uint32_ti, a=42)
        obj = generic_inline[int](uint32_ti, inner=Ref(anon_ti, inner_val))
        result = generic_inline[int].load_from(obj.serialize().begin_parse(), uint32_ti)
        assert isinstance(result, generic_inline)
        deref = result.inner.ref
        assert isinstance(deref, Anon_3)
        assert deref.a == 42
