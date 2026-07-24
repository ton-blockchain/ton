"""EnumLiteralStrategy: simplified enum types (Unit, Bool, True, BoolFalse)."""

from dataclasses import dataclass
from typing import final, override

from ..context import PyContext
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@dataclass(frozen=True)
class EnumLiteralInfo:
    """Describes how to simplify an enum type to a Python literal."""

    py_type_str: str
    type_info_expr: str
    tag_len: int
    values: dict[int, str]  # tag_value → Python literal expression


@final
class EnumLiteralStrategy(TypeStrategy):
    """Simplified enum type: Unit → None, Bool → bool, True → Literal[True], etc."""

    def __init__(self, info: EnumLiteralInfo, ctx: PyContext) -> None:
        self.info = info
        self.ctx = ctx
        if "Literal" in info.py_type_str:
            ctx.use("Literal")

    @override
    def py_type(self) -> str:
        return self.info.py_type_str

    @override
    def type_info_expr(self) -> str:
        self.ctx.use(self.info.type_info_expr)
        return self.info.type_info_expr

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        if self.info.tag_len == 0:
            sb.line("pass")
        elif len(self.info.values) == 1:
            tag_val = next(iter(self.info.values))
            sb.line(f"_ = {builder}.store_uint({tag_val}, {self.info.tag_len})")
        else:
            sb.line(f"_ = {builder}.store_uint(int({value}), {self.info.tag_len})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        if self.info.tag_len == 0:
            assert len(self.info.values) == 1
            val = next(iter(self.info.values.values()))
            sb.line(f"{target} = {val}")
        elif len(self.info.values) == 1:
            tag_val = next(iter(self.info.values))
            val = next(iter(self.info.values.values()))
            self.ctx.use("TlbModelError")
            sb.line(f"if {cs}.load_uint({self.info.tag_len}) != {tag_val}:")
            with sb.block():
                sb.line("raise TlbModelError('tag mismatch')")
            sb.line(f"{target} = {val}")
        else:
            sb.line(f"{target} = bool({cs}.load_uint({self.info.tag_len}))")

    @override
    def load_uses_cs(self) -> bool:
        return self.info.tag_len > 0

    @property
    @override
    def is_nullable(self) -> bool:
        return self.info.py_type_str == "None"

    @override
    def emit_conditional_assert(self, value: str, sb: SourceBuilder) -> None:
        if not self.is_nullable:
            sb.line(f"assert {value} is not None")
