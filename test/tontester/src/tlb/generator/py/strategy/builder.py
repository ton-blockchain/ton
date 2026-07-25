"""StrategyBuilder: builds TypeStrategy instances for resolved type expressions."""

from ...sema.builtins import (
    Any_type,
    Bits_type,
    CellRef_type,
    Int_type,
    Nat_type,
    NatLeq_type,
    NatLess_type,
    NatWidth_type,
    UInt_type,
)
from ...sema.types import (
    AnonymousRecordType,
    CellRefType,
    NatLiteral,
    ResolvedExpr,
    ResolvedNatExpr,
    ResolvedType,
    ResolvedTypeExpr,
    TupleType,
    TypeApply,
    TypeParamRef,
    WellKnownType,
    is_nat,
)
from ..context import PyContext
from ..name_scope import NameScope
from ..nat_expr import NatExpr
from ._base import TypeStrategy
from .bits import BitsStrategy
from .bounded_uint import BoundedUintStrategy
from .cell import CellRefBuiltinStrategy
from .cell_ref import CellRefStrategy
from .enum_literal import EnumLiteralInfo, EnumLiteralStrategy
from .int import IntStrategy
from .maybe import MaybeStrategy
from .slice import SliceTypeStrategy
from .tuple import TupleStrategy
from .type_param import TypeParamStrategy
from .uint import UintStrategy

_ENUM_LITERAL_INFOS: dict[WellKnownType, EnumLiteralInfo] = {
    WellKnownType.UNIT: EnumLiteralInfo(
        py_type_str="None",
        type_info_expr="UnitTypeInfo",
        tag_len=0,
        values={0: "None"},
    ),
    WellKnownType.BOOL: EnumLiteralInfo(
        py_type_str="bool",
        type_info_expr="BoolTypeInfo",
        tag_len=1,
        values={0: "False", 1: "True"},
    ),
    WellKnownType.BOOL_TRUE: EnumLiteralInfo(
        py_type_str="Literal[True]",
        type_info_expr="TrueTypeInfo",
        tag_len=1,
        values={1: "True"},
    ),
    WellKnownType.BOOL_FALSE: EnumLiteralInfo(
        py_type_str="Literal[False]",
        type_info_expr="FalseTypeInfo",
        tag_len=1,
        values={0: "False"},
    ),
    WellKnownType.BIT: EnumLiteralInfo(
        py_type_str="bool",
        type_info_expr="BoolTypeInfo",
        tag_len=1,
        values={0: "False", 1: "True"},
    ),
}


class StrategyBuilder:
    """Builds TypeStrategy instances for resolved type expressions.

    Holds the constructor-local context (scope, type param mappings)
    instead of threading them through every strategy_for call.
    Tracks which type params are referenced during building.
    """

    ctx: PyContext
    scope: NameScope
    parent_type: ResolvedType | None

    def __init__(
        self,
        ctx: PyContext,
        scope: NameScope,
        parent_type: ResolvedType | None = None,
    ) -> None:
        self.ctx = ctx
        self.scope = scope
        self.parent_type = parent_type

    @staticmethod
    def _to_nat(expr: ResolvedExpr) -> ResolvedNatExpr:
        assert is_nat(expr)
        return expr

    def _nat(self, expr: ResolvedExpr) -> NatExpr:
        """Create a NatExpr from a resolved expression."""
        return NatExpr(self._to_nat(expr), self.scope)

    def build(self, type_expr: ResolvedTypeExpr) -> TypeStrategy:
        """Create a TypeStrategy for a resolved type expression."""
        match type_expr:
            case TypeApply():
                return self._build_type_apply(type_expr)

            case TypeParamRef(param=param):
                ti_field = self.scope.lookup(param)
                ti_local = self.scope.lookup_local(param)
                type_var = self.scope.lookup_generic(param.type_level_param)
                return TypeParamStrategy(param, type_var, ti_field, ti_local)

            case CellRefType(inner=inner_expr):
                inner = self.build(inner_expr)
                return CellRefStrategy(inner, self.ctx)

            case TupleType(count=count_expr, element=element_expr):
                if (
                    isinstance(element_expr, TypeApply)
                    and element_expr.type.well_known == WellKnownType.BIT
                    and self.ctx.simplify.is_enabled(WellKnownType.BIT)
                ):
                    return BitsStrategy(NatExpr(count_expr, self.scope), self.ctx)
                count = NatExpr(count_expr, self.scope)
                element = self.build(element_expr)
                return TupleStrategy(count, element, self.ctx)

            case AnonymousRecordType(type=anon_type):
                return self._build_user_type(TypeApply(type=anon_type, arguments=[]))

    def _build_type_apply(self, type_expr: TypeApply) -> TypeStrategy:
        t = type_expr.type
        if t is UInt_type:
            assert len(type_expr.arguments) == 1
            return UintStrategy(self._nat(type_expr.arguments[0]), self.ctx)
        if t is Int_type:
            assert len(type_expr.arguments) == 1
            return IntStrategy(self._nat(type_expr.arguments[0]), self.ctx)
        if t is Bits_type:
            assert len(type_expr.arguments) == 1
            return BitsStrategy(self._nat(type_expr.arguments[0]), self.ctx)
        if t is Nat_type:
            return UintStrategy(NatExpr(NatLiteral(32), NameScope()), self.ctx)
        if t is NatWidth_type:
            assert len(type_expr.arguments) == 1
            return UintStrategy(self._nat(type_expr.arguments[0]), self.ctx)
        if t is NatLeq_type:
            assert len(type_expr.arguments) == 1
            return BoundedUintStrategy(self._nat(type_expr.arguments[0]), True, self.ctx)
        if t is NatLess_type:
            assert len(type_expr.arguments) == 1
            return BoundedUintStrategy(self._nat(type_expr.arguments[0]), False, self.ctx)
        if t.is_builtin and t.arity == 0:
            name = t.name
            if name.startswith("uint"):
                assert t.produces_nat
                return UintStrategy(NatExpr(NatLiteral(int(name[4:])), NameScope()), self.ctx)
            if name.startswith("int"):
                return IntStrategy(NatExpr(NatLiteral(int(name[3:])), NameScope()), self.ctx)
            if name.startswith("bits"):
                return BitsStrategy(NatExpr(NatLiteral(int(name[4:])), NameScope()), self.ctx)
        if t is Any_type:
            return SliceTypeStrategy(self.ctx)
        if t is CellRef_type:
            return CellRefBuiltinStrategy(self.ctx)
        if not t.is_builtin:
            if t.well_known and self.ctx.simplify.is_enabled(t.well_known):
                if t.well_known == WellKnownType.MAYBE:
                    return self._build_maybe(type_expr)
                if t.well_known == WellKnownType.UNARY:
                    from .unary import UnaryStrategy

                    return UnaryStrategy(self.ctx)
                if t.well_known in (WellKnownType.HASHMAP_E, WellKnownType.HASHMAP):
                    from .hashmap import HashmapStrategy

                    return HashmapStrategy(
                        type_expr,
                        self.ctx,
                        self.scope,
                        self,
                        allow_empty=(t.well_known == WellKnownType.HASHMAP_E),
                    )
                if t.well_known == WellKnownType.HASHMAP_AUG_E:
                    aug_class = self._aug_for_parent()
                    if aug_class is not None:
                        from .hashmap import HashmapAugStrategy

                        return HashmapAugStrategy(
                            type_expr, self.ctx, self.scope, self, aug_class=aug_class
                        )
                    # No aug configured — fall through to the unsimplified
                    # generic class so loading still works.
                if t.well_known in (WellKnownType.VAR_UINTEGER, WellKnownType.VAR_INTEGER):
                    from .varint import VarIntStrategy

                    return VarIntStrategy(
                        type_expr,
                        self.ctx,
                        self.scope,
                        signed=(t.well_known == WellKnownType.VAR_INTEGER),
                    )
                if info := _ENUM_LITERAL_INFOS.get(t.well_known):
                    return EnumLiteralStrategy(info, self.ctx)
            return self._build_user_type(type_expr)

        assert False, f"unhandled builtin type: {t.name}"

    def _build_user_type(self, type_expr: TypeApply) -> TypeStrategy:
        from .user_type import UserTypeStrategy

        return UserTypeStrategy(type_expr, self.ctx, self.scope, builder=self)

    def _aug_for_parent(self) -> str | None:
        """Look up the aug class for this build's containing type, if any."""
        if self.parent_type is None:
            return None
        if self.parent_type.origin_module is None:
            return None
        return self.ctx.simplify.aug_for(self.parent_type.origin_module, self.parent_type.name)

    def _build_maybe(self, type_expr: TypeApply) -> TypeStrategy:
        """Build strategy for simplified Maybe X → X | None.

        Falls back to UserTypeStrategy if the inner type is nullable
        (e.g., Maybe (Maybe X) would otherwise collapse None | None).
        """
        assert len(type_expr.arguments) == 1
        inner_arg = type_expr.arguments[0]
        assert isinstance(
            inner_arg, TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType
        )
        inner = self.build(inner_arg)
        if inner.is_nullable:
            return self._build_user_type(type_expr)
        return MaybeStrategy(inner, self.ctx)
