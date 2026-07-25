"""SliceTypeStrategy: emit store/load for Any (raw slice)."""

from typing import final, override

from ..context import PyContext
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class SliceTypeStrategy(TypeStrategy):
    def __init__(self, ctx: PyContext) -> None:
        self.ctx = ctx
        ctx.use("Slice")

    @override
    def py_type(self) -> str:
        return "Slice"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("AnyType")
        return "AnyType"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"_ = {builder}.store_slice({value})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {cs}")
