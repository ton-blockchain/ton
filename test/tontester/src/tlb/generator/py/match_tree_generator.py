"""MatchTreeGenerator: generates Python code for match tree dispatch."""

from ..sema.types import (
    CheckConstraint,
    MatchBit,
    MatchConstraint,
    MatchConstructor,
    MatchFail,
    MatchTag,
    MatchTree,
    ParamKind,
    ResolvedConstructor,
    TypeLevelParam,
    TypeParamRef,
)
from .context import PyContext
from .name_scope import NameScope
from .nat_expr import render_constraint
from .source_builder import SourceBuilder


class MatchTreeGenerator:
    ctx: PyContext
    scope: NameScope
    entry_params: list[TypeLevelParam]
    probe_name: str

    def __init__(
        self,
        ctx: PyContext,
        scope: NameScope,
        entry_params: list[TypeLevelParam],
        probe_name: str = "probe",
    ) -> None:
        self.ctx = ctx
        self.scope = scope
        self.entry_params = entry_params
        self.probe_name = probe_name

    def _check_constraint_to_python(self, cc: CheckConstraint) -> str:
        return render_constraint(cc, self.scope)

    def _constructor_load_args(self, cons: ResolvedConstructor) -> list[str]:
        """Map type-level entry params to a constructor's load_from args."""
        cons_type_params = set(self.ctx.get_constructor(cons).type_params)
        result: list[str] = []
        for tlp in self.entry_params:
            if tlp.kind == ParamKind.NAT:
                result.append(self.scope.lookup(tlp))
            else:
                expr = cons.result_param_exprs.get(tlp.position)
                if isinstance(expr, TypeParamRef) and expr.param in cons_type_params:
                    result.append(self.scope.lookup(tlp))
        return result

    def generate(self, tree: MatchTree, sb: SourceBuilder) -> None:
        match tree:
            case MatchConstructor(constructor=cons):
                cons_name = self.ctx.scope.lookup(cons)
                cons_load_args = self._constructor_load_args(cons)
                cons_cg = self.ctx.get_constructor(cons)
                if cons_cg.type_params:
                    cons_name = f"{cons_name}[{', '.join(cons_cg.type_var_name(p) for p in cons_cg.type_params)}]"
                if cons_load_args:
                    args = ", ".join(["cs"] + cons_load_args)
                    sb.line(f"return {cons_name}.load_from({args})")
                else:
                    sb.line(f"return {cons_name}.load_from(cs)")

            case MatchBit(zero=zero, one=one):
                sb.line(f"if {self.probe_name}.load_bit() == 0:")
                with sb.block():
                    self.generate(zero, sb)
                sb.line("else:")
                with sb.block():
                    self.generate(one, sb)

            case MatchTag(bits=bits, child=child):
                self.ctx.use("TlbModelError")
                tag_val = int(bits, 2)
                sb.line(f"if {self.probe_name}.load_uint({len(bits)}) != {tag_val}:")
                with sb.block():
                    sb.line("raise TlbModelError('tag mismatch')")
                self.generate(child, sb)

            case MatchConstraint(condition=condition, if_true=if_true, if_false=if_false):
                cond = self._check_constraint_to_python(condition)
                sb.line(f"if {cond}:")
                with sb.block():
                    self.generate(if_true, sb)
                sb.line("else:")
                with sb.block():
                    self.generate(if_false, sb)

            case MatchFail():
                self.ctx.use("TlbModelError")
                sb.line("raise TlbModelError('no matching constructor')")
