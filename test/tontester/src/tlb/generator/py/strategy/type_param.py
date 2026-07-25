"""TypeParamStrategy: emit store/load for generic type parameters."""

from typing import final, override

from ...sema.types import TypeParamDef
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class TypeParamStrategy(TypeStrategy):
    """Field whose type is a generic type parameter (e.g. value:X where {X:Type}).

    Delegates serialization to a runtime TypeInfo passed as an argument.
    type_var is the generic type variable name (e.g. "X").
    ti_field is the dataclass field name (e.g. "_tX"), used for self.X access.
    ti_local is the local variable name (e.g. "_tX" or "_tX_1"), used in load_from body.
    """

    def __init__(self, param: TypeParamDef, type_var: str, ti_field: str, ti_local: str) -> None:
        self.param = param
        self.type_var = type_var
        self.ti_field = ti_field
        self.ti_local = ti_local

    @override
    def py_type(self) -> str:
        return self.type_var

    @override
    def type_info_expr(self) -> str:
        return self.ti_local

    @override
    def type_info_expr_self(self) -> str:
        return f"self.{self.ti_field}"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"self.{self.ti_field}.serialize_value({value}, {builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {self.ti_local}.load_from({cs})")

    @property
    @override
    def is_nullable(self) -> bool:
        return True

    @override
    def emit_conditional_assert(self, value: str, sb: SourceBuilder) -> None:
        sb.line(f"assert self.{self.ti_field}.is_nonnull({value})")
