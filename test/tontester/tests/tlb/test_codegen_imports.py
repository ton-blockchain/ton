"""End-to-end tests for cross-module TL-B imports.

`imports_user.tlb` imports `imports_base.tlb` and uses three flavors of foreign
type: a sole-cons named (Currency), a multi-cons (Color), and an unnamed
sole-cons (Point). The generated user file imports all of those from the
generated base file via `from generated.imports_base import …`.
"""

from generated.imports_base import (
    Color,
    ColorType,
    Currency,
    Point,
    blue,
    currency,
    green,
    pair,
    red,
)
from generated.imports_user import (
    UnaryType,
    sized,
    unary_succ,
    unary_zero,
    wallet,
)


def _roundtrip(w: wallet) -> wallet:
    cell = w.serialize()
    return wallet.load_from(cell.begin_parse())


class TestForeignTypes:
    def test_currency_is_imported_class(self):
        # The Wallet annotation reads `Currency`, which is the type-alias name
        # in the base module. It must resolve to the same object as the source.
        assert wallet.__annotations__["bal"] is Currency

    def test_color_typeinfo_is_imported(self):
        # Multi-cons types use `<Name>Type` for serialization dispatch — that
        # symbol must be importable from the base module.
        assert ColorType is not None

    def test_point_is_imported_constructor(self):
        # Unnamed sole-cons types: the constructor IS the type name.
        assert wallet.__annotations__["origin"] is Point


class TestRoundTrip:
    def test_currency_only(self):
        w = wallet(bal=currency(amount=42), tag=red(), origin=Point(x=1, y=2))
        result = _roundtrip(w)
        assert result.bal == currency(amount=42)

    def test_each_color_constructor(self):
        for color in [red(), green(), blue()]:
            w = wallet(bal=currency(amount=0), tag=color, origin=Point(x=0, y=0))
            result = _roundtrip(w)
            assert type(result.tag) is type(color)

    def test_full_value_roundtrip(self):
        w = wallet(
            bal=currency(amount=1_000_000),
            tag=blue(),
            origin=Point(x=300, y=400),
        )
        result = _roundtrip(w)
        assert result == wallet(
            bal=currency(amount=1_000_000),
            tag=blue(),
            origin=Point(x=300, y=400),
        )


class TestColorTypeInfoDispatch:
    def test_dispatch_from_typeinfo(self):
        # The user module deserializes Color via `ColorType().load_from(cs)`.
        # Confirm that path works against each constructor's wire format.
        for color, expected in [(red(), red), (green(), green), (blue(), blue)]:
            cell = color.serialize()
            decoded = ColorType().load_from(cell.begin_parse())
            assert isinstance(decoded, expected)


class TestCrossModuleInferenceChain:
    """`sized` extracts `n` by chaining through the foreign Pair into Unary.

    The deser plan emits `m_1.first.get_output(0)` — `first` is a foreign field
    name that codegen reads from imports_base's PyManifest.
    """

    def _build_unary(self, n: int):
        # Build a Unary value that decodes to `n`.
        u: unary_zero | unary_succ = unary_zero()
        for k in range(n):
            u = unary_succ(n=k, x=u)
        return u

    def _make_pair(self, n: int):
        from tlb.object import UintTypeConstructor

        return pair(
            _tX=UnaryType(),
            _tY=UintTypeConstructor(32),
            first=self._build_unary(n),
            second=42,
        )

    def test_roundtrip_n_zero(self):
        s = sized(n=0, m=self._make_pair(0), tail=0)
        cell = s.serialize()
        result = sized.load_from(cell.begin_parse())
        assert result.n == 0
        assert result.tail == 0

    def test_roundtrip_n_three(self):
        s = sized(n=3, m=self._make_pair(3), tail=5)  # 5 fits in 3 bits
        cell = s.serialize()
        result = sized.load_from(cell.begin_parse())
        assert result.n == 3
        assert result.tail == 5

    def test_unary_typeinfo_dispatches(self):
        # Smoke check that the user-module-defined Unary works end-to-end.
        u = self._build_unary(2)
        cell = u.serialize()
        decoded = UnaryType().load_from(cell.begin_parse())
        assert decoded.get_output(0) == 2


class TestTypeAliasIdentity:
    def test_currency_alias_equals_constructor(self):
        # `currency$_ … = Currency;` produces `Currency = currency` at module
        # scope; both names refer to the same class.
        assert Currency is currency

    def test_color_alias_resolves_to_union_of_constructors(self):
        # `type Color = red | green | blue` is a PEP 695 alias; ColorType()
        # dispatches the deserialization, so the round-trip in
        # TestColorTypeInfoDispatch already proves the union covers all three.
        # Here we just confirm the alias name is bound to a TypeAliasType.
        from typing import TypeAliasType

        assert isinstance(Color, TypeAliasType)
