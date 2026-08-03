"""NatExpr: renders resolved nat expressions in different contexts."""

from ..ast_nodes import CompareOp
from ..sema.types import (
    CheckConstraint,
    NatAdd,
    NatFieldValue,
    NatGetBit,
    NatLiteral,
    NatMul,
    NatParamDef,
    NatParamRef,
    NatSub,
    NatTypeArg,
    ResolvedField,
    ResolvedNatExpr,
    TypeLevelParam,
)
from .name_scope import NameScope

_COMPARE_OP_STR: dict[CompareOp, str] = {
    CompareOp.EQ: "==",
    CompareOp.LT: "<",
    CompareOp.LE: "<=",
    CompareOp.GT: ">",
    CompareOp.GE: ">=",
}


class NatExpr:
    """A resolved nat expression that can be rendered in different contexts.

    Holds the sema expression and scope, renders with appropriate prefix:
    - local form (for load_from): bare variable name
    - self form (for serialize_to): self-prefixed
    """

    _expr: ResolvedNatExpr
    _scope: NameScope

    def __init__(self, expr: ResolvedNatExpr, scope: NameScope) -> None:
        self._expr = expr
        self._scope = scope

    @property
    def local(self) -> str:
        return self._render(self._expr, use_local=True)

    @property
    def self_(self) -> str:
        return self._render(self._expr, prefix="self.")

    @property
    def is_constant(self) -> bool:
        """True if this is a compile-time constant (NatLiteral)."""
        return isinstance(self._expr, NatLiteral)

    @property
    def is_zero(self) -> bool:
        """True if this is a compile-time constant 0."""
        return isinstance(self._expr, NatLiteral) and self._expr.value == 0

    def _resolve(self, obj: NatParamDef | TypeLevelParam | ResolvedField, use_local: bool) -> str:
        if use_local:
            return self._scope.lookup_local(obj)
        return self._scope.lookup(obj)

    def _render(self, expr: ResolvedNatExpr, use_local: bool = False, prefix: str = "") -> str:
        match expr:
            case NatLiteral(value=value):
                return str(value)
            case NatParamRef(param=param):
                return f"{prefix}{self._resolve(param, use_local)}"
            case NatFieldValue(field=field):
                return f"{prefix}{self._resolve(field, use_local)}"
            case NatAdd(left=left, right=right):
                return f"({self._render(left, use_local, prefix)} + {self._render(right, use_local, prefix)})"
            case NatSub(left=left, right=right):
                return f"({self._render(left, use_local, prefix)} - {self._render(right, use_local, prefix)})"
            case NatMul(left=left, right=right):
                return f"({self._render(left, use_local, prefix)} * {self._render(right, use_local, prefix)})"
            case NatGetBit(value=value, bit=bit):
                return f"(({self._render(value, use_local, prefix)} >> {self._render(bit, use_local, prefix)}) & 1)"
            case NatTypeArg(param=param):
                return self._scope.lookup(param)


def render_constraint(cc: CheckConstraint, scope: NameScope, *, use_self: bool = False) -> str:
    """Render a CheckConstraint as a Python expression string."""
    if use_self:
        left = NatExpr(cc.left, scope).self_
        right = NatExpr(cc.right, scope).self_
    else:
        left = NatExpr(cc.left, scope).local
        right = NatExpr(cc.right, scope).local
    return f"{left} {_COMPARE_OP_STR[cc.op]} {right}"
