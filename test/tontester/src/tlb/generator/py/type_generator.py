"""TypeGenerator: generates Python code for a resolved TL-B type."""

from ..sema.types import (
    ParamKind,
    ResolvedConstructor,
    ResolvedType,
    TypeParamRef,
)
from .constructor_generator import ConstructorGenerator
from .context import PyContext
from .match_tree_generator import MatchTreeGenerator
from .name_scope import NameScope
from .source_builder import SourceBuilder


class TypeGenerator:
    ctx: PyContext
    t: ResolvedType
    scope: NameScope
    type_vars: list[str]
    cons_generators: list[ConstructorGenerator]

    def __init__(self, ctx: PyContext, t: ResolvedType) -> None:
        self.ctx = ctx
        self.t = t

        self._bind_names()

    def _bind_names(self) -> None:
        """Pre-bind type-level and constructor field names (must happen before codegen)."""
        self.scope = self.ctx.scope.child()
        self.ctx.set_type_scope(self.t, self.scope)
        self.type_vars = []
        for tlp in self.t.type_level_params:
            if tlp.is_output:
                continue
            if tlp.kind == ParamKind.TYPE:
                nice_name = f"T{tlp.position}"
                for c in self.t.constructors:
                    expr = c.result_param_exprs.get(tlp.position)
                    if isinstance(expr, TypeParamRef):
                        nice_name = expr.param.name
                        break
                type_var = self.scope.bind_generic(tlp, nice_name)
                _ = self.scope.bind(tlp, f"_t{type_var}")
                self.type_vars.append(type_var)
            else:
                _ = self.scope.bind(tlp, f"_type_arg_{tlp.position}")

        self.cons_generators = []
        for c in self.t.constructors:
            cg = ConstructorGenerator(self.ctx, c, self.scope)
            self.cons_generators.append(cg)

    def generate(self, sb: SourceBuilder) -> None:
        for cg in self.cons_generators:
            cg.generate(sb)
            sb.blank()
            sb.blank()

        generic_suffix = f"[{', '.join(self.type_vars)}]" if self.type_vars else ""

        def _cons_type(c: ResolvedConstructor) -> str:
            name = self.ctx.scope.lookup(c)
            cg = self.ctx.get_constructor(c)
            if cg.type_params:
                return f"{name}[{', '.join(cg.type_var_name(p) for p in cg.type_params)}]"
            return name

        if not self.t.has_sole_constructor:
            type_name = self.ctx.scope.lookup(self.t)
            cons_names = " | ".join(_cons_type(c) for c in self.t.constructors)
            sb.line(f"type {type_name}{generic_suffix} = {cons_names}")
            sb.blank()
            sb.blank()
            self._generate_type_info(sb)
            sb.blank()
            sb.blank()
        else:
            c = self.t.constructors[0]
            cons_name = self.ctx.scope.lookup(c)
            if not self.t.has_unnamed_sole_constructor:
                # Named constructor — emit alias
                type_name = self.ctx.scope.lookup(self.t)
                cg = self.ctx.get_constructor(c)
                if len(cg.type_params) == len(self.type_vars):
                    sb.line(f"{type_name} = {cons_name}")
                else:
                    sb.line(f"type {type_name}{generic_suffix} = {_cons_type(c)}")
                sb.blank()
                sb.blank()
            # Unnamed constructor uses type name directly — no alias needed

    def _generate_type_info(
        self,
        sb: SourceBuilder,
    ) -> None:
        self.ctx.use("final", "Builder", "Slice", "override")
        type_name = self.ctx.scope.lookup(self.t)
        info_name = self.ctx.lookup_type_info(self.t)

        generic_suffix = f"[{', '.join(self.type_vars)}]" if self.type_vars else ""
        entry_nat_count = sum(
            1 for tlp in self.t.type_level_params if not tlp.is_output and tlp.kind == ParamKind.NAT
        )
        has_args = bool(self.type_vars) or entry_nat_count > 0

        protocol_args: list[str] = []
        for _ in range(entry_nat_count):
            protocol_args.append("int")
        for v in self.type_vars:
            protocol_args.append(f"TypeInfo[{v}]")
        protocol_args_str = ", ".join(protocol_args)

        self.ctx.use("TypeInfo")
        if has_args:
            self.ctx.use("InstantiableTypeInfo")
            sb.line("@final")
            sb.line(
                (
                    f"class {info_name}{generic_suffix}"
                    f"(InstantiableTypeInfo[{type_name}{generic_suffix}, {protocol_args_str}]):"
                )
            )
        else:
            sb.line("@final")
            sb.line(f"class {info_name}(TypeInfo[{type_name}]):")

        with sb.block():
            sb.line("@override")
            sb.line(
                f"def serialize_value(self, value: {type_name}{generic_suffix}, builder: Builder) -> None:"
            )
            with sb.block():
                sb.line("value.serialize_to(builder)")
            sb.blank()

            entry_params = [tlp for tlp in self.t.type_level_params if not tlp.is_output]
            params = ["cs: Slice"]
            type_var_idx = 0
            for tlp in entry_params:
                name = self.scope.lookup(tlp)
                if tlp.kind == ParamKind.NAT:
                    params.append(f"{name}: int")
                else:
                    params.append(f"{name}: TypeInfo[{self.type_vars[type_var_idx]}]")
                    type_var_idx += 1
            params_str = ", ".join(params)

            sb.line("@override")
            sb.line(f"def load_from(self, {params_str}) -> {type_name}{generic_suffix}:")
            with sb.block():
                for tlp in entry_params:
                    if tlp.kind == ParamKind.NAT:
                        sb.line(f"assert {self.scope.lookup(tlp)} >= 0")
                assert self.t.match_tree is not None
                probe_name = "probe"
                if self.t.match_tree.reads_bits:
                    probe_name = self.scope.reserve("probe")
                    sb.line(f"{probe_name} = cs.copy()")
                MatchTreeGenerator(self.ctx, self.scope, entry_params, probe_name).generate(
                    self.t.match_tree, sb
                )

            if self.t.is_special:
                self.ctx.use("Cell", "TlbModelError")
                sb.blank()
                deser_params = ["cell: Cell"] + params[1:]
                deser_params_str = ", ".join(deser_params)
                sb.line("@override")
                sb.line(
                    f"def deserialize(self, {deser_params_str}) -> {type_name}{generic_suffix}:"
                )
                with sb.block():
                    sb.line("cs = cell.begin_parse()")
                    sb.line("if not cs.is_special():")
                    with sb.block():
                        sb.line(
                            (
                                "raise TlbModelError("
                                f"'expected special cell for {type_name}, got ordinary cell')"
                            )
                        )
                    if entry_params:
                        arg_names = [self.scope.lookup(tlp) for tlp in entry_params]
                        load_args = ", ".join(["cs"] + arg_names)
                        sb.line(f"result = self.load_from({load_args})")
                    else:
                        sb.line("result = self.load_from(cs)")
                    sb.line("TlbModelError.raise_if_not_empty(cs)")
                    sb.line("return result")

            sb.blank()
            sb.line("@override")
            sb.line("def __eq__(self, other: object) -> bool:")
            with sb.block():
                sb.line(f"return isinstance(other, {info_name})")
            sb.blank()
            sb.line("@override")
            sb.line("def __hash__(self) -> int:")
            with sb.block():
                sb.line(f"return hash('{info_name}')")
