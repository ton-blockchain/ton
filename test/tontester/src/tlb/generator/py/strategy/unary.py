"""UnaryStrategy: simplified Unary ~n → int."""

from typing import final, override

from ..context import PyContext
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class UnaryStrategy(TypeStrategy):
    """Simplified Unary ~n: field is int, ser/deser as unary encoding.

    The int value IS the output param — get_output(0) returns the value itself.
    """

    def __init__(self, ctx: PyContext) -> None:
        self.ctx = ctx

    @override
    def py_type(self) -> str:
        return "int"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("UnaryTypeInfo")
        return "UnaryTypeInfo"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        self.ctx.use("UnaryTypeInfo")
        sb.line(f"UnaryTypeInfo.serialize_value({value}, {builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        self.ctx.use("UnaryTypeInfo")
        sb.line(f"{target} = UnaryTypeInfo.load_from({cs})")

    @override
    def emit_get_output(self, field_expr: str, position: int) -> str:
        assert position == 0
        return field_expr
