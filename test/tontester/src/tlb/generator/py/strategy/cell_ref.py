"""CellRefStrategy: emit store/load for ^Type using runtime Ref[X]."""

from typing import final, override

from ..context import PyContext
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class CellRefStrategy(TypeStrategy):
    """^Type: uses the runtime Ref[X] and RefType[X]."""

    def __init__(self, inner: TypeStrategy, ctx: PyContext) -> None:
        self.inner = inner
        self.ctx = ctx
        ctx.use("Ref")

    @override
    def py_type(self) -> str:
        return f"Ref[{self.inner.py_type()}]"

    @override
    def type_info_expr(self) -> str:
        return f"Ref[{self.inner.py_type()}].instantiate({self.inner.type_info_expr()})"

    @override
    def type_info_expr_self(self) -> str:
        return f"Ref[{self.inner.py_type()}].instantiate({self.inner.type_info_expr_self()})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"{value}.serialize_to({builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        self.ctx.use("Ref")
        inner_ti = self.inner.type_info_expr()
        sb.line(f"{target} = Ref({inner_ti}, {cs}.load_ref())")
