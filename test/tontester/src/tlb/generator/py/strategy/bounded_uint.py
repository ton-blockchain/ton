"""BoundedUintStrategy: emit store/load for bounded unsigned integers."""

from typing import final, override

from ..context import PyContext
from ..nat_expr import NatExpr
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class BoundedUintStrategy(TypeStrategy):
    """Strategy for #<= N (inclusive) or #< N (exclusive) bounded unsigned integers."""

    def __init__(self, bound: NatExpr, inclusive: bool, ctx: PyContext) -> None:
        self.bound = bound
        self.inclusive = inclusive
        self.ctx = ctx

    @override
    def py_type(self) -> str:
        return "int"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("BoundedUintTypeConstructor")
        return f"BoundedUintTypeConstructor({self.bound.local}, inclusive={self.inclusive})"

    @override
    def type_info_expr_self(self) -> str:
        self.ctx.use("BoundedUintTypeConstructor")
        return f"BoundedUintTypeConstructor({self.bound.self_}, inclusive={self.inclusive})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        if self.inclusive and self.bound.is_zero:
            sb.line("pass")
            return
        self.ctx.use("BoundedUintTypeConstructor")
        sb.line(
            (
                f"BoundedUintTypeConstructor({self.bound.self_}, inclusive={self.inclusive})"
                f".serialize_value({value}, {builder})"
            )
        )

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        if self.inclusive and self.bound.is_zero:
            sb.line(f"{target} = 0")
            return
        self.ctx.use("BoundedUintTypeConstructor")
        sb.line(
            (
                f"{target} = BoundedUintTypeConstructor({self.bound.local}, inclusive={self.inclusive})"
                f".load_from({cs})"
            )
        )

    @override
    def load_uses_cs(self) -> bool:
        return not (self.inclusive and self.bound.is_zero)
