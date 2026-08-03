"""BitsStrategy: emit store/load for fixed-width bit arrays."""

from typing import final, override

from ..context import PyContext
from ..nat_expr import NatExpr
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class BitsStrategy(TypeStrategy):
    def __init__(self, width: NatExpr, ctx: PyContext) -> None:
        self.width = width
        self.ctx = ctx
        ctx.use("bitarray")

    @override
    def py_type(self) -> str:
        return "bitarray"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("BitsTypeConstructor")
        return f"BitsTypeConstructor({self.width.local})"

    @override
    def type_info_expr_self(self) -> str:
        self.ctx.use("BitsTypeConstructor")
        return f"BitsTypeConstructor({self.width.self_})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        if self.width.is_constant:
            sb.line(f"_ = {builder}.store_bits({value})")
        else:
            self.ctx.use("BitsTypeConstructor")
            sb.line(f"BitsTypeConstructor({self.width.self_}).serialize_value({value}, {builder})")

    @override
    def emit_serialize_assertions(self, field_name: str, sb: SourceBuilder) -> bool:
        if not self.width.is_constant:
            return False
        sb.line(f"assert len({field_name}) == {self.width.self_}")
        return True

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        if self.width.is_constant:
            sb.line(f"{target} = {cs}.load_bits({self.width.local})")
        else:
            self.ctx.use("BitsTypeConstructor")
            sb.line(f"{target} = BitsTypeConstructor({self.width.local}).load_from({cs})")
