"""TupleStrategy: emit store/load for n * Type sequences."""

from typing import final, override

from ..context import PyContext
from ..nat_expr import NatExpr
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class TupleStrategy(TypeStrategy):
    """n * Type: fixed or variable-length sequence of values."""

    def __init__(self, count: NatExpr, element: TypeStrategy, ctx: PyContext) -> None:
        self.count = count
        self.element = element
        self.ctx = ctx

    @override
    def py_type(self) -> str:
        return f"list[{self.element.py_type()}]"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("TupleTypeConstructor")
        elem_ti = self.element.type_info_expr()
        return f"TupleTypeConstructor({self.count.local}, {elem_ti})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        idx = self.ctx.tmp("_i")
        sb.line(f"for {idx} in range({self.count.self_}):")
        with sb.block():
            self.element.emit_store(f"{value}[{idx}]", builder, sb)

    @override
    def emit_serialize_assertions(self, field_name: str, sb: SourceBuilder) -> bool:
        sb.line(f"assert len({field_name}) == {self.count.self_}")
        return True

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target}: list[{self.element.py_type()}] = []")
        sb.line(f"for _ in range({self.count.local}):")
        with sb.block():
            elem_tmp = self.ctx.tmp("_elem")
            self.element.emit_load(elem_tmp, cs, sb)
            sb.line(f"{target}.append({elem_tmp})")
