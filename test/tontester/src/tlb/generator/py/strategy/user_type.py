"""UserTypeStrategy: emit store/load for user-defined types."""

from typing import final, override

from ...sema.types import (
    ParamKind,
    TypeApply,
    TypeParamRef,
    is_nat,
    is_type,
)
from ..context import PyContext
from ..name_scope import NameScope
from ..nat_expr import NatExpr
from ..source_builder import SourceBuilder
from ._base import StrategyBuilderProtocol, TypeStrategy


@final
class UserTypeStrategy(TypeStrategy):
    """User-defined type, possibly generic. Self-contained — computes all
    rendering from the TypeApply and scope."""

    def __init__(
        self,
        type_expr: TypeApply,
        ctx: PyContext,
        scope: NameScope,
        builder: StrategyBuilderProtocol,
    ) -> None:
        self._type_expr = type_expr
        self._ctx = ctx

        if type_expr.type.has_sole_constructor:
            cons = type_expr.type.constructors[0]
            cons_type_params = cons.used_type_params
        else:
            cons = None
            cons_type_params = None

        # Unnamed sole constructors have no separate type binding.
        if cons is not None and type_expr.type.has_unnamed_sole_constructor:
            self._type_name = ctx.lookup_constructor(cons)
        else:
            self._type_name = ctx.lookup_type(type_expr.type)

        # Type-level lists (for py_type and assertions — always all TLPs)
        self._type_var_args: list[str] = []
        self._nat_assertions: list[tuple[int, str]] = []
        self._type_assertions: list[tuple[int, str]] = []

        # TypeInfo-level lists (for type_info_expr / emit_load):
        #   multi-constructor: all TLPs
        #   single-constructor: only params the constructor uses
        ti_args: list[str] = []
        ti_args_self: list[str] = []
        ti_type_var_args: list[str] = []

        for tlp, arg in zip(type_expr.type.type_level_params, type_expr.arguments, strict=True):
            if tlp.is_output:
                continue
            if tlp.kind == ParamKind.NAT:
                assert is_nat(arg)
                local = NatExpr(arg, scope).local
                self_ = NatExpr(arg, scope).self_
                ti_args.append(local)
                ti_args_self.append(self_)
                if not arg.references_type_arg:
                    self._nat_assertions.append((tlp.position, NatExpr(arg, scope).self_))
            else:
                assert is_type(arg)
                arg_strategy = builder.build(arg)
                ti = arg_strategy.type_info_expr()
                ti_self = arg_strategy.type_info_expr_self()
                py = arg_strategy.py_type()
                self._type_var_args.append(py)
                self._type_assertions.append((tlp.position, ti_self))
                # For single-constructor, only include if constructor uses this type param
                if cons is not None and cons_type_params is not None:
                    expr = cons.result_param_exprs.get(tlp.position)
                    if not (isinstance(expr, TypeParamRef) and expr.param in cons_type_params):
                        continue
                ti_args.append(ti)
                ti_args_self.append(ti_self)
                ti_type_var_args.append(py)

        # Resolve the TypeInfo class and bare expression once.
        # _ti_class: used with .instantiate() and .load_from()
        # _ti_bare: the TypeInfo expression when there are no args
        if cons is not None:
            base = ctx.lookup_constructor(cons)
            if ti_type_var_args:
                base = f"{base}[{', '.join(ti_type_var_args)}]"
            self._ti_class = base
            self._ti_bare = base
        else:
            base = ctx.lookup_type_info(type_expr.type)
            if self._type_var_args:
                base = f"{base}[{', '.join(self._type_var_args)}]"
            self._ti_class = base
            self._ti_bare = f"{base}()"

        self._ti_args = ti_args
        self._ti_args_self = ti_args_self

    @override
    def py_type(self) -> str:
        if self._type_var_args:
            return f"{self._type_name}[{', '.join(self._type_var_args)}]"
        return self._type_name

    @override
    def type_info_expr(self) -> str:
        if self._ti_args:
            return f"{self._ti_class}.instantiate({', '.join(self._ti_args)})"
        return self._ti_bare

    @override
    def type_info_expr_self(self) -> str:
        if self._ti_args_self:
            return f"{self._ti_class}.instantiate({', '.join(self._ti_args_self)})"
        return self._ti_bare

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"{value}.serialize_to({builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        if self._ti_args:
            args = ", ".join([cs] + self._ti_args)
        else:
            args = cs
        sb.line(f"{target} = {self._ti_bare}.load_from({args})")

    @override
    def emit_get_output(self, field_expr: str, position: int) -> str:
        tlp = self._type_expr.type.type_level_params[position]
        assert tlp.kind == ParamKind.NAT and tlp.is_output
        return f"{field_expr}.get_output({position})"

    @override
    def emit_serialize_assertions(self, field_name: str, sb: SourceBuilder) -> bool:
        emitted = False
        for pos, expected in self._nat_assertions:
            sb.line(f"assert {field_name}.get_output({pos}) == {expected}")
            emitted = True
        for pos, ti_expr in self._type_assertions:
            sb.line(f"{field_name}.check_type({pos}, {ti_expr})")
            emitted = True
        return emitted
