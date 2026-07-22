"""Tests for generated Python code from TL-B collisions schema."""

from generated.collisions import (
    FooType,
    PassType,
    Tag,
    TagType,
    bar,
    class_1,
    cls_field,
    cs_field,
    foo,
    foo_1,
    int_1,
    kw_generic,
    pass_1,
    return_1,
    tag_blue,
    tag_red,
    tt,
)
from tlb.object import UintTypeConstructor

# ── Name collisions ──────────────────────────────────────────────────


class TestNameCollisions:
    def test_python_keyword_constructor_names(self):
        """Constructors named 'pass' and 'return' get suffixed."""
        result = PassType().load_from(pass_1().serialize().begin_parse())
        assert isinstance(result, pass_1)
        result = PassType().load_from(return_1().serialize().begin_parse())
        assert isinstance(result, return_1)

    def test_python_keyword_field_names(self):
        """Fields named 'for' and 'import' get suffixed."""
        obj = class_1(for_1=42, import_1=-1)
        result = class_1.load_from(obj.serialize().begin_parse())
        assert result.for_1 == 42
        assert result.import_1 == -1

    def test_builtin_type_name(self):
        """Type named 'Int' (collides with Python int) works."""
        obj = int_1(value=123)
        result = int_1.load_from(obj.serialize().begin_parse())
        assert result.value == 123

    def test_constructor_name_collision_across_types(self):
        """'foo' constructor in Foo type vs 'foo' constructor in Bar type get different names."""
        # foo (in Foo) has x:uint32 with tag $0
        obj_foo = foo(x=42)
        result = FooType().load_from(obj_foo.serialize().begin_parse())
        assert isinstance(result, foo)
        assert result.x == 42

        # foo_1 (in Bar, originally named 'foo') has y:uint64, no tag
        obj_bar = foo_1(y=999)
        result = foo_1.load_from(obj_bar.serialize().begin_parse())
        assert isinstance(result, foo_1)
        assert result.y == 999

    def test_bar_in_foo(self):
        """bar constructor in Foo type."""
        obj = bar(x=77)
        result = FooType().load_from(obj.serialize().begin_parse())
        assert isinstance(result, bar)
        assert result.x == 77

    def test_field_named_cs(self):
        """Field named 'cs' keeps its name; local var in load_from is renamed."""
        obj = cs_field(a=10, cs=20, b=30)
        result = cs_field.load_from(obj.serialize().begin_parse())
        assert result.a == 10
        assert result.cs == 20
        assert result.b == 30

    def test_field_named_cls(self):
        """Field named 'cls' keeps its name; local var in load_from is renamed."""
        obj = cls_field(cls=42)
        result = cls_field.load_from(obj.serialize().begin_parse())
        assert result.cls == 42

    def test_type_param_named_int(self):
        """Type param named 'int' (collides with Python builtin) round-trips."""
        ti = UintTypeConstructor(32)
        obj = kw_generic(_tint=ti, value=42)
        result = kw_generic[int].load_from(obj.serialize().begin_parse(), ti)
        assert result.value == 42

    def test_typeinfo_class_collides_with_user_type(self):
        """The user type `TagType` and the auto-derived TypeInfo class for
        the multi-cons `Tag` would both want the bare `TagType` symbol; the
        user binding must win and the TypeInfo gets a suffix."""
        # The user-defined `TagType` is a type alias for `tt` and round-trips.
        user_obj = tt(payload=99)
        roundtripped = tt.load_from(user_obj.serialize().begin_parse())
        assert roundtripped.payload == 99
        assert TagType is tt
        # The auto-derived TypeInfo for `Tag` still exists under a
        # collision-suffixed name and dispatches both Tag constructors.
        from generated.collisions import TagType_1

        decoded_red = TagType_1().load_from(tag_red().serialize().begin_parse())
        decoded_blue = TagType_1().load_from(tag_blue().serialize().begin_parse())
        assert isinstance(decoded_red, tag_red)
        assert isinstance(decoded_blue, tag_blue)
        # Sanity: the union alias is also exported.
        assert Tag is not None
