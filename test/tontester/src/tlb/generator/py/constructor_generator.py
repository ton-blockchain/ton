"""ConstructorGenerator: generates Python code for a resolved TL-B constructor."""

from ..identity_key import IdentityKey
from ..sema.types import (
    AnonymousRecordType,
    BindOutputParam,
    BindParam,
    CellRefType,
    CheckConstraint,
    InferenceStep,
    NatParamDef,
    NatTypeArg,
    ParamDef,
    ParamKind,
    ReadField,
    ResolvedConstructor,
    ResolvedField,
    ResolvedType,
    SolveConstraint,
    TypeParamDef,
    TypeParamRef,
)
from .context import PyContext
from .name_scope import NameScope
from .nat_expr import NatExpr, render_constraint
from .source_builder import SourceBuilder
from .strategy import TypeStrategy
from .strategy.builder import StrategyBuilder


def _get_inlineable_anon(f: ResolvedField) -> tuple[ResolvedType, bool] | None:
    """Check if a field is an inlineable anonymous record.

    Returns (anon_type, is_cell_ref) or None.
    Only unnamed, non-conditional, non-generic anonymous records qualify.
    """
    if f.name is not None or f.condition is not None:
        return None
    if isinstance(f.type_expr, AnonymousRecordType) and f.type_expr.type.arity == 0:
        return (f.type_expr.type, False)
    if (
        isinstance(f.type_expr, CellRefType)
        and isinstance(f.type_expr.inner, AnonymousRecordType)
        and f.type_expr.inner.type.arity == 0
    ):
        return (f.type_expr.inner.type, True)
    return None


class ConstructorGenerator:
    ctx: PyContext
    c: ResolvedConstructor
    type_scope: NameScope
    scope: NameScope
    params: list[ParamDef]
    type_params: list[TypeParamDef]
    strategies: dict[IdentityKey[ResolvedField], TypeStrategy]
    inlined_fields: list[tuple[ResolvedField, ResolvedType, bool]]
    cls_name: str

    def __init__(
        self,
        ctx: PyContext,
        c: ResolvedConstructor,
        type_scope: NameScope,
    ) -> None:
        self.ctx = ctx
        self.c = c
        self.type_scope = type_scope
        self.params = []
        self.type_params = []
        self.strategies = {}
        self.inlined_fields = []

        self._bind_names()

    def _bind_names(self) -> None:
        """Pre-bind all field and param names in this constructor's scope."""
        self.scope = self.type_scope.child()
        self.ctx.register_constructor(self.c, self)

        for p in self.c.params:
            match p:
                case TypeParamDef():
                    _ = self.scope.bind_field(p, f"_t{p.name}")
                case NatParamDef():
                    _ = self.scope.bind_field(p, p.name)

        for f in self.c.fields:
            _ = self.scope.bind_field(f, f.name or "field")

        # Detect inlineable anonymous record fields and bind their sub-field names.
        if self.ctx.simplify.inline_records:
            for f in self.c.fields:
                result = _get_inlineable_anon(f)
                if result is None:
                    continue
                anon_type, is_cell_ref = result
                anon_cons = anon_type.constructors[0]
                for sf in anon_cons.fields:
                    if sf.name is not None:
                        _ = self.scope.bind_field(sf, sf.name)
                        _ = self.scope.bind_setter(sf, f"set_{sf.name}")
                self.inlined_fields.append((f, anon_type, is_cell_ref))

        self.cls_name = self.ctx.scope.lookup(self.c)

    def type_var_name(self, p: TypeParamDef) -> str:
        """Get the scope-bound type variable name for a TypeParamDef."""
        return self.type_scope.lookup_generic(p.type_level_param)

    def _class_header(self) -> str:
        """Return the generic suffix + base class + metaclass portion of the class declaration."""
        generic_suffix = ""
        if self.type_params:
            generic_suffix = f"[{', '.join(self.type_var_name(p) for p in self.type_params)}]"

        # Build TLBRecord[...] args matching load_from's entry params
        record_args: list[str] = []
        for tlp in self.c.parent_type.type_level_params:
            if tlp.is_output:
                continue
            if tlp.kind == ParamKind.NAT:
                record_args.append("int")
            else:
                expr = self.c.result_param_exprs.get(tlp.position)
                if isinstance(expr, TypeParamRef) and expr.param in self.type_params:
                    record_args.append(f"TypeInfo[{self.type_var_name(expr.param)}]")

        metaclass = ""
        if self.c.parent_type.has_sole_constructor:
            self.ctx.use("SelfTypeInfoTag")
            metaclass = f", SelfTypeInfoTag, metaclass={'Generic' if record_args else ''}TLBType"

        if record_args:
            record_suffix = f"[{', '.join(record_args)}]"
            self.ctx.use("GenericTLBType")
            return f"{generic_suffix}(TLBRecord{record_suffix}{metaclass})"
        self.ctx.use("TLBType")
        return f"{generic_suffix}(TLBRecord[()]{metaclass})"

    def generate(self, sb: SourceBuilder) -> None:
        self.ctx.use("final", "dataclass", "TLBRecord", "Builder", "Slice", "override")

        builder = StrategyBuilder(self.ctx, self.scope, parent_type=self.c.parent_type)
        for f in self.c.fields:
            self.strategies[IdentityKey(f)] = builder.build(f.type_expr)

        self.params = [
            p for p in self.c.params if isinstance(p, NatParamDef) or p in self.c.used_type_params
        ]
        self.type_params = [p for p in self.params if isinstance(p, TypeParamDef)]

        if self.type_params:
            self.ctx.use("TypeInfo")

        needs_custom_init = any(
            self.strategies[IdentityKey(f)].init_param_type() is not None for f in self.c.fields
        )

        sb.line("@final")
        sb.line("@dataclass(init=False)" if needs_custom_init else "@dataclass")
        sb.line(f"class {self.cls_name}{self._class_header()}:")

        with sb.block():
            for p in self.c.params:
                match p:
                    case TypeParamDef():
                        if p in self.type_params:
                            sb.line(f"{self.scope.lookup(p)}: TypeInfo[{self.type_var_name(p)}]")
                    case NatParamDef():
                        sb.line(f"{self.scope.lookup(p)}: int")
            for f in self.c.fields:
                py_type = self.strategies[IdentityKey(f)].py_type()
                if f.condition is not None:
                    py_type = f"{py_type} | None"
                sb.line(f"{self.scope.lookup(f)}: {py_type}")

            if needs_custom_init:
                sb.blank()
                self._generate_init(sb)

            sb.blank()
            self._generate_serialize_to(sb)
            sb.blank()
            self._generate_load_from(sb)
            if any(v is not None for v in self.c.nat_param_values):
                sb.blank()
                self._generate_get_output(sb)
            if self.type_params:
                sb.blank()
                self._generate_check_type(sb)
            if self.c.parent_type.has_sole_constructor and self.c.parent_type.is_special:
                sb.blank()
                self._generate_special_deserialize(sb)
            if self.inlined_fields:
                self._generate_inline_properties(sb)

    def _generate_init(self, sb: SourceBuilder) -> None:
        """Emit a custom keyword-only __init__ for constructors whose fields
        accept widened input types (e.g. HashmapDict | Mapping). Each
        field's strategy emits its own assignment line — strategies that
        don't widen fall back to a direct `self.X = X` assign."""
        params: list[str] = ["self"]
        for p in self.c.params:
            match p:
                case TypeParamDef():
                    if p in self.type_params:
                        params.append(f"{self.scope.lookup(p)}: TypeInfo[{self.type_var_name(p)}]")
                case NatParamDef():
                    params.append(f"{self.scope.lookup(p)}: int")
        for f in self.c.fields:
            strat = self.strategies[IdentityKey(f)]
            ty = strat.init_param_type() or strat.py_type()
            if f.condition is not None:
                ty = f"{ty} | None"
            params.append(f"{self.scope.lookup(f)}: {ty}")
        sb.line(f"def __init__({', '.join(params)}) -> None:")
        with sb.block():
            for p in self.c.params:
                if isinstance(p, TypeParamDef) and p not in self.type_params:
                    continue
                name = self.scope.lookup(p)
                sb.line(f"self.{name} = {name}")
            for f in self.c.fields:
                strat = self.strategies[IdentityKey(f)]
                name = self.scope.lookup(f)
                strat.emit_init_assignment(f"self.{name}", name, sb)

    def _generate_serialize_to(
        self,
        sb: SourceBuilder,
    ) -> None:
        sb.line("@override")
        sb.line("def serialize_to(self, builder: Builder) -> None:")
        with sb.block():
            has_content = False
            for p in self.c.params:
                if isinstance(p, NatParamDef):
                    sb.line(f"assert self.{self.scope.lookup(p)} >= 0")
                    has_content = True
            for step in self.c.deser_steps:
                if not isinstance(step, CheckConstraint):
                    continue
                if step.left.references_type_arg or step.right.references_type_arg:
                    continue
                sb.line(f"assert {render_constraint(step, self.scope, use_self=True)}")
                has_content = True
            if self.c.tag_bits:
                tag_val = int(self.c.tag_bits, 2)
                sb.line(f"_ = builder.store_uint({tag_val}, {self.c.tag_len})")
                has_content = True
            for f in self.c.fields:
                name = self.scope.lookup(f)
                strat = self.strategies[IdentityKey(f)]
                if f.condition is not None:
                    sel = NatExpr(f.condition, self.scope).self_
                    sb.line(f"if {sel}:")
                    with sb.block():
                        strat.emit_conditional_assert(f"self.{name}", sb)
                        _ = strat.emit_serialize_assertions(f"self.{name}", sb)
                        strat.emit_store(f"self.{name}", "builder", sb)
                else:
                    _ = strat.emit_serialize_assertions(f"self.{name}", sb)
                    strat.emit_store(f"self.{name}", "builder", sb)
                has_content = True
            if not has_content:
                sb.line("pass")

    def _entry_param_names(self) -> list[str]:
        """Names of entry params (for forwarding as call arguments)."""
        names: list[str] = []
        for tlp in self.c.parent_type.type_level_params:
            if tlp.is_output:
                continue
            name = self.type_scope.lookup(tlp)
            if tlp.kind == ParamKind.NAT:
                names.append(name)
            else:
                expr = self.c.result_param_exprs.get(tlp.position)
                if isinstance(expr, TypeParamRef) and expr.param in self.type_params:
                    names.append(name)
        return names

    def _entry_params(self) -> list[str]:
        """Build the typed parameter list for load_from/deserialize from type-level entry params."""
        params: list[str] = []
        for tlp in self.c.parent_type.type_level_params:
            if tlp.is_output:
                continue
            name = self.type_scope.lookup(tlp)
            if tlp.kind == ParamKind.NAT:
                params.append(f"{name}: int")
            else:
                expr = self.c.result_param_exprs.get(tlp.position)
                assert isinstance(expr, TypeParamRef)
                if expr.param in self.type_params:
                    type_var = self.type_scope.lookup_generic(tlp)
                    params.append(f"{name}: TypeInfo[{type_var}]")
        return params

    def _return_type(self) -> str:
        if self.type_params:
            generic_vars = ", ".join(self.type_var_name(p) for p in self.type_params)
            return f"{self.cls_name}[{generic_vars}]"
        return self.cls_name

    def _generate_load_from(self, sb: SourceBuilder) -> None:
        params_str = ", ".join(["cs: Slice"] + self._entry_params())
        sb.line("@override")
        sb.line("@classmethod")
        sb.line(f"def load_from(cls, {params_str}) -> {self._return_type()}:")

        with sb.block():
            for tlp in self.c.parent_type.type_level_params:
                if not tlp.is_output and tlp.kind == ParamKind.NAT:
                    sb.line(f"assert {self.type_scope.lookup(tlp)} >= 0")
            if self.c.tag_bits:
                self.ctx.use("TlbModelError")
                tag_val = int(self.c.tag_bits, 2)
                sb.line(f"if cs.load_uint({self.c.tag_len}) != {tag_val}:")
                with sb.block():
                    sb.line("raise TlbModelError('tag mismatch')")
            ctor_args: list[str] = []
            for p in self.c.params:
                field = self.scope.lookup(p)
                local = self.scope.lookup_local(p)
                match p:
                    case TypeParamDef():
                        if p in self.type_params:
                            ctor_args.append(f"{field}={local}")
                    case NatParamDef():
                        ctor_args.append(f"{field}={local}" if field != local else local)
            for step in self.c.deser_steps:
                self._emit_deser_step(step, ctor_args, sb)
            sb.line(f"return cls({', '.join(ctor_args)})")

    def _emit_deser_step(
        self,
        step: ReadField | BindParam | BindOutputParam | SolveConstraint | CheckConstraint,
        ctor_args: list[str],
        sb: SourceBuilder,
    ) -> None:
        match step:
            case ReadField(field=f):
                strat = self.strategies[IdentityKey(f)]
                field_name = self.scope.lookup(f)
                var_name = self.scope.lookup_local(f)
                if f.condition is not None:
                    sel = NatExpr(f.condition, self.scope).local
                    sb.line(f"if {sel}:")
                    with sb.block():
                        strat.emit_load(var_name, "cs", sb)
                    sb.line("else:")
                    with sb.block():
                        sb.line(f"{var_name} = None")
                else:
                    strat.emit_load(var_name, "cs", sb)
                ctor_args.append(f"{field_name}={var_name}")

            case SolveConstraint(target_param=target_param, value=value):
                var_name = self.scope.lookup_local(target_param)
                value_expr = NatExpr(value, self.scope).local
                sb.line(f"{var_name} = {value_expr}")

                if not isinstance(value, NatTypeArg):
                    self.ctx.use("TlbModelError")
                    sb.line(f"if {var_name} < 0:")
                    with sb.block():
                        sb.line(
                            (
                                "raise TlbModelError("
                                f"f'nat parameter {target_param.name} is negative: {{{var_name}}}')"
                            )
                        )

            case BindOutputParam(target_param=target_param, extraction=extraction):
                self.ctx.use("TlbModelError")
                var_name = self.scope.lookup_local(target_param)
                source_name = self.scope.lookup_local(extraction.source_field)
                expr = source_name
                for inf_step in extraction.chain:
                    expr = self._emit_inference_access(inf_step, expr, sb)
                if extraction.chain:
                    strat = StrategyBuilder(self.ctx, self.scope).build(
                        extraction.chain[-1].concrete_arg
                    )
                else:
                    strat = self.strategies[IdentityKey(extraction.source_field)]
                expr = strat.emit_get_output(expr, extraction.result_param_position)
                sb.line(f"{var_name} = {expr}")
                sb.line(f"if {var_name} < 0:")
                with sb.block():
                    sb.line(
                        (
                            "raise TlbModelError("
                            f"f'nat parameter {target_param.name} is negative: {{{var_name}}}')"
                        )
                    )

            case CheckConstraint():
                self.ctx.use("TlbModelError")
                rendered = render_constraint(step, self.scope)
                sb.line(f"if not ({rendered}):")
                with sb.block():
                    sb.line(f"raise TlbModelError(f'constraint failed: {{{rendered}}}')")

            case BindParam(target_param=target_param, position=position):
                if target_param not in self.type_params:
                    return
                tlp = self.c.parent_type.type_level_params[position]
                type_name = self.scope.lookup_local(tlp)
                var_name = self.scope.lookup_local(target_param)
                if type_name != var_name:
                    sb.line(f"{var_name} = {type_name}")

    def _emit_inference_access(self, inf_step: InferenceStep, expr: str, sb: SourceBuilder) -> str:
        """Emit code to access the inference field on a (possibly multi-constructor) type.

        Returns the expression for the accessed field value.
        """
        cons_fields = inf_step.type.inference[inf_step.param_idx].constructor_field
        field_names: set[str] = set()
        for inf_cons, inf_field in cons_fields.items():
            field_names.add(self.ctx.lookup_field(inf_cons, inf_field))
        if len(field_names) == 1:
            return f"{expr}.{next(iter(field_names))}"
        tmp = self.ctx.tmp("_inf")
        items = list(cons_fields.items())
        for i, (inf_cons, inf_field) in enumerate(items):
            cons_name = self.ctx.lookup_constructor(inf_cons)
            field_name = self.ctx.lookup_field(inf_cons, inf_field)
            if i == 0:
                sb.line(f"if isinstance({expr}, {cons_name}):")
            elif i < len(items) - 1:
                sb.line(f"elif isinstance({expr}, {cons_name}):")
            else:
                sb.line("else:")
            with sb.block():
                sb.line(f"{tmp} = {expr}.{field_name}")
        return tmp

    def _generate_get_output(self, sb: SourceBuilder) -> None:
        """Generate get_output() override returning nat type-level param values by TLP position."""
        sb.line("@override")
        sb.line("def get_output(self, idx: int) -> int:")
        with sb.block():
            first = True
            for i, val in enumerate(self.c.nat_param_values):
                if val is None:
                    continue
                expr = NatExpr(val, self.scope).self_
                if first:
                    sb.line(f"if idx == {i}:")
                    first = False
                else:
                    sb.line(f"elif idx == {i}:")
                with sb.block():
                    sb.line(f"return {expr}")
            sb.line("raise ValueError(f'no nat param at index {idx}')")

    def _generate_check_type(self, sb: SourceBuilder) -> None:
        """Generate check_type() override that asserts type params match by TLP position."""
        self.ctx.use("TypeInfo")
        sb.line("@override")
        sb.line("def check_type[_T](self, idx: int, ti: TypeInfo[_T]) -> None:")
        with sb.block():
            first = True
            for tlp in self.c.parent_type.type_level_params:
                if tlp.kind != ParamKind.TYPE:
                    continue
                expr = self.c.result_param_exprs.get(tlp.position)
                if not isinstance(expr, TypeParamRef):
                    continue
                ti_name = self.scope.lookup(expr.param)
                if first:
                    sb.line(f"if idx == {tlp.position}:")
                    first = False
                else:
                    sb.line(f"elif idx == {tlp.position}:")
                with sb.block():
                    if expr.param in self.type_params:
                        sb.line(f"assert self.{ti_name} == ti")
                    sb.line("return")
            sb.line("raise ValueError(f'no type param at index {idx}')")

    def _generate_special_deserialize(self, sb: SourceBuilder) -> None:
        """Generate deserialize() override for special cell types (e.g. merkle proofs)."""
        self.ctx.use("Cell", "TlbModelError")
        params_str = ", ".join(["cell: Cell"] + self._entry_params())
        sb.line("@override")
        sb.line("@classmethod")
        sb.line(f"def deserialize(cls, /, {params_str}) -> {self._return_type()}:")
        with sb.block():
            sb.line("cs = cell.begin_parse()")
            sb.line("if not cs.is_special():")
            with sb.block():
                sb.line(
                    (
                        f"raise TlbModelError("
                        f"'expected special cell for {self.cls_name}, got ordinary cell')"
                    )
                )
            load_args = ", ".join(["cs"] + self._entry_param_names())
            result = self.ctx.tmp("_result")
            sb.line(f"{result} = cls.load_from({load_args})")
            sb.line("TlbModelError.raise_if_not_empty(cs)")
            sb.line(f"return {result}")

    def _generate_inline_properties(self, sb: SourceBuilder) -> None:
        """Generate property accessors for inlined anonymous record sub-fields."""
        builder = StrategyBuilder(self.ctx, self.scope, parent_type=self.c.parent_type)
        for parent_field, anon_type, is_cell_ref in self.inlined_fields:
            field_name = self.scope.lookup(parent_field)
            anon_cons = anon_type.constructors[0]
            anon_scope = self.ctx.get_constructor(anon_cons).scope
            for sf in anon_cons.fields:
                if sf.name is None:
                    continue
                prop_name = self.scope.lookup(sf)
                inner_name = anon_scope.lookup(sf)
                strat = builder.build(sf.type_expr)
                py_type = strat.py_type()
                if sf.condition is not None:
                    py_type = f"{py_type} | None"
                if is_cell_ref:
                    accessor = f"self.{field_name}.ref.{inner_name}"
                else:
                    accessor = f"self.{field_name}.{inner_name}"
                setter_name = self.scope.lookup_setter(sf)
                sb.blank()
                sb.line("@property")
                sb.line(f"def {prop_name}(self) -> {py_type}:")
                with sb.block():
                    sb.line(f"return {accessor}")
                sb.blank()
                sb.line(f"def {setter_name}(self, value: {py_type}) -> None:")
                with sb.block():
                    sb.line(f"{accessor} = value")
