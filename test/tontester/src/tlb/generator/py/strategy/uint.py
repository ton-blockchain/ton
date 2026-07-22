"""UintStrategy: emit store/load for unsigned integers."""

from typing import final, override

from ..context import PyContext
from ..nat_expr import NatExpr
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class UintStrategy(TypeStrategy):
    def __init__(self, width: NatExpr, ctx: PyContext) -> None:
        self.width = width
        self.ctx = ctx

    @override
    def py_type(self) -> str:
        return "int"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("UintTypeConstructor")
        return f"UintTypeConstructor({self.width.local})"

    @override
    def type_info_expr_self(self) -> str:
        self.ctx.use("UintTypeConstructor")
        return f"UintTypeConstructor({self.width.self_})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        if self.width.is_zero:
            sb.line("pass")
        elif self.width.is_constant:
            sb.line(f"_ = {builder}.store_uint({value}, {self.width.self_})")
        else:
            self.ctx.use("UintTypeConstructor")
            sb.line(f"UintTypeConstructor({self.width.self_}).serialize_value({value}, {builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        if self.width.is_zero:
            sb.line(f"{target} = 0")
        elif self.width.is_constant:
            sb.line(f"{target} = {cs}.load_uint({self.width.local})")
        else:
            self.ctx.use("UintTypeConstructor")
            sb.line(f"{target} = UintTypeConstructor({self.width.local}).load_from({cs})")

    @override
    def load_uses_cs(self) -> bool:
        return not self.width.is_zero
