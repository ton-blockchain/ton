"""CellRefBuiltinStrategy: emit store/load for ^Cell (opaque cell reference)."""

from typing import final, override

from ..context import PyContext
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class CellRefBuiltinStrategy(TypeStrategy):
    """Strategy for ^Cell — opaque cell reference using CellRefType."""

    def __init__(self, ctx: PyContext) -> None:
        self.ctx = ctx
        ctx.use("Cell")

    @override
    def py_type(self) -> str:
        return "Cell"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("CellRefType")
        return "CellRefType"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"_ = {builder}.store_ref({value})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {cs}.load_ref()")
