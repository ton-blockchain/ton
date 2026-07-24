"""MaybeStrategy: simplified Maybe X → X | None."""

from typing import final, override

from ..context import PyContext
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class MaybeStrategy(TypeStrategy):
    """Simplified Maybe X: field is X | None, ser/deser inlined."""

    def __init__(self, inner: TypeStrategy, ctx: PyContext) -> None:
        self.inner = inner
        self.ctx = ctx

    @override
    def py_type(self) -> str:
        return f"{self.inner.py_type()} | None"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("MaybeTypeConstructor")
        return f"MaybeTypeConstructor({self.inner.type_info_expr()})"

    @override
    def type_info_expr_self(self) -> str:
        self.ctx.use("MaybeTypeConstructor")
        return f"MaybeTypeConstructor({self.inner.type_info_expr_self()})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"if {value} is not None:")
        with sb.block():
            sb.line(f"_ = {builder}.store_uint(1, 1)")
            self.inner.emit_store(value, builder, sb)
        sb.line("else:")
        with sb.block():
            sb.line(f"_ = {builder}.store_uint(0, 1)")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"if {cs}.load_bit():")
        with sb.block():
            self.inner.emit_load(target, cs, sb)
        sb.line("else:")
        with sb.block():
            sb.line(f"{target} = None")

    @property
    @override
    def is_nullable(self) -> bool:
        return True

    @override
    def emit_conditional_assert(self, value: str, sb: SourceBuilder) -> None:
        pass
