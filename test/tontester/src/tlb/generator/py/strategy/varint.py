"""VarIntStrategy: simplified VarUInteger/VarInteger → int."""

from typing import final, override

from ...sema.types import TypeApply, is_nat
from ..context import PyContext
from ..name_scope import NameScope
from ..nat_expr import NatExpr
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class VarIntStrategy(TypeStrategy):
    """Simplified VarUInteger n or VarInteger n → int."""

    def __init__(
        self, type_expr: TypeApply, ctx: PyContext, scope: NameScope, *, signed: bool
    ) -> None:
        self._ctx = ctx
        self._signed = signed
        assert len(type_expr.arguments) == 1
        arg = type_expr.arguments[0]
        assert is_nat(arg)
        self._n = NatExpr(arg, scope)
        ti_name = "VarIntTypeConstructor" if signed else "VarUIntTypeConstructor"
        self._ti_name = ti_name
        ctx.use(ti_name)

    @override
    def py_type(self) -> str:
        return "int"

    @override
    def type_info_expr(self) -> str:
        return f"{self._ti_name}({self._n.local})"

    @override
    def type_info_expr_self(self) -> str:
        return f"{self._ti_name}({self._n.self_})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"{self._ti_name}({self._n.self_}).serialize_value({value}, {builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {self._ti_name}({self._n.local}).load_from({cs})")
