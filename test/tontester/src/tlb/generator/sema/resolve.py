"""Type registration and expression resolution.

Phase 1: Register all type names and determine arities/param kinds.
Phase 2: Resolve each constructor's fields and expressions.
"""

from dataclasses import dataclass, field

from ..ast_nodes import (
    Add,
    Apply,
    CellRef,
    Compare,
    CompareOp,
    Conditional,
    Constraint,
    Constructor,
    ExplicitField,
    FieldDef,
    GetBit,
    Identifier,
    ImplicitParam,
    InlineRecord,
    IntConst,
    Multiply,
    NegatedIdentifier,
    Schema,
    TypeExpr,
)
from .builtins import CellRef_type, NatLess_type, create_builtin_registry
from .types import (
    AnonymousRecordType,
    CellRefType,
    Module,
    NatAdd,
    NatFieldValue,
    NatGetBit,
    NatLiteral,
    NatMul,
    NatParamDef,
    NatParamRef,
    NatSub,
    ParamDef,
    ParamKind,
    ResolvedConstraint,
    ResolvedConstructor,
    ResolvedExpr,
    ResolvedField,
    ResolvedNatExpr,
    ResolvedType,
    ResolvedTypeExpr,
    SemaError,
    TupleType,
    TypeApply,
    TypeLevelParam,
    TypeParamDef,
    TypeParamRef,
)


class TypeRegistry:
    current_module: Module
    _types: dict[str, ResolvedType]
    _anon_types: list[ResolvedType]

    def __init__(self, current_module: Module) -> None:
        self.current_module = current_module
        self._types = create_builtin_registry()
        self._anon_types = []

    def lookup(self, name: str) -> ResolvedType | None:
        return self._types.get(name)

    def register(self, name: str) -> ResolvedType:
        """Register a type defined in the current module, or return the
        existing entry. Builtins are returned untouched — callers detect
        and reject redefinitions."""
        existing = self._types.get(name)
        if existing is not None:
            return existing
        t = ResolvedType(name=name, origin_module=self.current_module)
        self._types[name] = t
        return t

    def register_anonymous(self) -> ResolvedType:
        t = ResolvedType(name="", origin_module=self.current_module)
        self._anon_types.append(t)
        return t

    def add_imported(self, m: AnalyzedModule) -> None:
        """Pre-populate the registry with types from an already-analyzed module.

        Only types native to `m` are imported (no transitive re-export). Names
        already defined in the current module silently win — the import is
        skipped. A collision between two different imported modules with no
        local shadow is an ambiguity error.
        """
        if m.module == self.current_module:
            raise SemaError(f"module '{m.module.name}' cannot import itself")
        for t in m.types:
            if t.is_builtin:
                continue
            if t.origin_module != m.module:
                # Defensive: types that m itself imported are not re-exported.
                continue
            existing = self._types.get(t.name)
            if existing is t:
                continue  # idempotent — same module imported twice
            if existing is None:
                self._types[t.name] = t
                continue
            assert not existing.is_builtin, (
                f"import '{t.name}' from '{m.module.name}' collides with a builtin"
            )
            if existing.origin_module == self.current_module:
                continue  # local definition shadows the import
            assert existing.origin_module is not None
            raise SemaError(
                (
                    f"ambiguous import: '{t.name}' is imported from both "
                    f"'{existing.origin_module.name}' and '{m.module.name}' "
                    "with no local definition to disambiguate"
                )
            )

    def all_user_types(self) -> list[ResolvedType]:
        """Types defined by the current module (excluding imports and builtins)."""
        return [
            t
            for t in self._types.values()
            if not t.is_builtin and t.origin_module == self.current_module
        ] + self._anon_types


@dataclass
class AnalyzedModule:
    """The result of running sema on a single TL-B module.

    Drivers build these incrementally and pass them to `analyze` of dependent
    modules via the `imports` dict.
    """

    module: Module
    registry: TypeRegistry
    types: list[ResolvedType] = field(default_factory=list)


def register_types(schema: Schema, registry: TypeRegistry) -> None:
    """First pass: group constructors by result type, determine arity and param kinds."""
    by_type: dict[str, list[Constructor]] = {}
    for c in schema.constructors:
        by_type.setdefault(c.result_type, []).append(c)

    for type_name, constructors in by_type.items():
        resolved_type = registry.register(type_name)
        if resolved_type.is_builtin:
            raise SemaError(f"cannot redefine built-in type '{type_name}'")

        arity = len(constructors[0].result_params)
        for c in constructors[1:]:
            if len(c.result_params) != arity:
                raise SemaError(
                    (
                        f"inconsistent arity for type '{type_name}': "
                        f"constructor '{c.name}' has {len(c.result_params)} params, "
                        f"expected {arity}"
                    )
                )

        param_kinds = _determine_param_kinds(type_name, constructors, arity)

        output_positions: list[int] = []
        for i in range(arity):
            negated_count = sum(1 for c in constructors if c.result_params[i].negated)
            if negated_count == len(constructors):
                output_positions.append(i)
            elif negated_count > 0:
                raise SemaError(
                    (
                        f"type '{type_name}': result param at position {i} is negated (~) "
                        f"in {negated_count} of {len(constructors)} constructors; "
                        "must be all or none"
                    )
                )
        resolved_type.type_level_params = [
            TypeLevelParam(position=i, kind=param_kinds[i], is_output=(i in output_positions))
            for i in range(arity)
        ]

        seen_names: set[str] = set()
        for c in constructors:
            if c.name is not None:
                if c.name in seen_names:
                    raise SemaError(f"type '{type_name}': duplicate constructor name '{c.name}'")
                seen_names.add(c.name)

        special_count = sum(1 for c in constructors if c.is_special)
        if 0 < special_count < len(constructors):
            raise SemaError(
                (
                    f"type '{type_name}': {special_count} of {len(constructors)} constructors "
                    "are marked special (!); must be all or none"
                )
            )


def _determine_param_kinds(
    type_name: str, constructors: list[Constructor], arity: int
) -> list[ParamKind]:
    """Determine the ParamKind for each type parameter position."""
    kinds: list[ParamKind | None] = [None] * arity

    for c in constructors:
        implicit_kinds: dict[str, ParamKind] = {}
        for f in c.fields:
            if isinstance(f, ImplicitParam):
                implicit_kinds[f.name] = ParamKind.TYPE if f.is_type else ParamKind.NAT

        for i, rp in enumerate(c.result_params):
            if rp.negated:
                kind = ParamKind.NAT
            elif isinstance(rp.expr, Identifier) and rp.expr.name in implicit_kinds:
                kind = implicit_kinds[rp.expr.name]
            elif isinstance(rp.expr, IntConst):
                kind = ParamKind.NAT
            else:
                kind = ParamKind.NAT

            if kinds[i] is None:
                kinds[i] = kind
            elif kinds[i] != kind:
                raise SemaError(f"inconsistent param kind at position {i} of type '{type_name}'")

    return [k or ParamKind.NAT for k in kinds]


def resolve_constructors(schema: Schema, registry: TypeRegistry) -> None:
    """Second pass: resolve all constructor fields and expressions."""
    for c in schema.constructors:
        resolved = _resolve_constructor(c, registry)
        resolved.parent_type.constructors.append(resolved)


def check_type_arities(user_types: list[ResolvedType]) -> None:
    """Check type application arities after all types are fully registered."""
    for t in user_types:
        for c in t.constructors:
            for f in c.fields:
                _check_type_apply_arity(f.type_expr)
            for expr in c.result_param_exprs.values():
                if isinstance(expr, TypeApply | TupleType | CellRefType | AnonymousRecordType):
                    _check_type_apply_arity(expr)


type ScopeEntry = tuple[ParamDef, None] | tuple[None, ResolvedField]


class _Scope:
    entries: dict[str, ScopeEntry]

    def __init__(self) -> None:
        self.entries = {}

    def add_param(self, param: ParamDef) -> None:
        if param.name in self.entries:
            raise SemaError(f"duplicate parameter name '{param.name}'")
        self.entries[param.name] = (param, None)

    def add_field(self, name: str, field: ResolvedField) -> None:
        if name in self.entries:
            raise SemaError(f"duplicate field name '{name}'")
        self.entries[name] = (None, field)

    def lookup(self, name: str) -> ScopeEntry | None:
        return self.entries.get(name)


def _resolve_constructor(c: Constructor, registry: TypeRegistry) -> ResolvedConstructor:
    parent_type = registry.lookup(c.result_type)
    assert parent_type is not None

    scope = _Scope()

    # Build name → TypeLevelParam mapping for TYPE params from result params AST
    type_param_tlp: dict[str, TypeLevelParam] = {}
    for i, rp in enumerate(c.result_params):
        tlp = parent_type.type_level_params[i]
        if isinstance(rp.expr, Identifier) and tlp.kind == ParamKind.TYPE and not rp.negated:
            type_param_tlp[rp.expr.name] = tlp

    params: list[ParamDef] = []
    for f in c.fields:
        if isinstance(f, ImplicitParam):
            if f.name in scope.entries:
                raise SemaError(f"duplicate parameter name '{f.name}'")
            p: ParamDef
            if f.is_type:
                if f.name not in type_param_tlp:
                    raise SemaError(
                        (
                            f"Type parameter '{f.name}' not found in result params of "
                            f"constructor '{c.name}'"
                        )
                    )
                p = TypeParamDef(name=f.name, type_level_param=type_param_tlp[f.name])
            else:
                p = NatParamDef(name=f.name)
            params.append(p)
            scope.add_param(p)

    fields: list[ResolvedField] = []
    source_order: list[ResolvedField | ResolvedConstraint] = []
    for f in c.fields:
        if isinstance(f, ExplicitField):
            resolved_field = _resolve_field(f, scope, registry)
            fields.append(resolved_field)
            source_order.append(resolved_field)
            if f.name is not None:
                scope.add_field(f.name, resolved_field)
        elif isinstance(f, Constraint):
            rc = _resolve_constraint(f, scope, registry, params)
            source_order.append(rc)

    result_param_exprs: dict[int, ResolvedExpr] = {}
    nat_param_values: list[ResolvedNatExpr | None] = []
    for i, rp in enumerate(c.result_params):
        tlp = parent_type.type_level_params[i]
        if tlp.is_output:
            # Output params CAN reference fields (e.g. hml_long's ~n)
            nat_param_values.append(_resolve_nat_expr(rp.expr, scope, registry))
        elif tlp.kind == ParamKind.NAT:
            expr = _resolve_expr(rp.expr, scope, registry)
            _check_no_field_refs_in_expr(
                expr, f"result param at position {i} of constructor '{c.name}'"
            )
            result_param_exprs[i] = expr
            assert not isinstance(
                expr, TypeApply | TypeParamRef | CellRefType | TupleType | AnonymousRecordType
            )
            nat_param_values.append(expr)
        else:
            expr = _resolve_expr(rp.expr, scope, registry)
            _check_no_field_refs_in_expr(
                expr, f"result param at position {i} of constructor '{c.name}'"
            )
            result_param_exprs[i] = expr
            nat_param_values.append(None)

    tag_bits = c.tag.bits
    tag_len = len(tag_bits)

    return ResolvedConstructor(
        name=c.name,
        tag_bits=tag_bits,
        tag_len=tag_len,
        parent_type=parent_type,
        is_special=c.is_special,
        params=params,
        fields=fields,
        result_param_exprs=result_param_exprs,
        source_order=source_order,
        nat_param_values=nat_param_values,
    )


def insert_implicit_constraints(user_types: list[ResolvedType]) -> None:
    """Insert implicit constraints into source_order for all constructors.

    Must run after check_type_arities (which validates param kinds).
    """
    for t in user_types:
        for c in t.constructors:
            c.source_order = _insert_implicit_constraints(c.source_order)


def _insert_implicit_constraints(
    source_order: list[ResolvedField | ResolvedConstraint],
) -> list[ResolvedField | ResolvedConstraint]:
    """Rebuild source_order inserting implicit constraints before fields that need them.

    #< n requires n > 0 (no values exist below 0, and (0-1).bit_length() is wrong).
    """
    result: list[ResolvedField | ResolvedConstraint] = []
    for item in source_order:
        if isinstance(item, ResolvedField):
            _collect_nat_less_constraints(item.type_expr, result)
        result.append(item)
    return result


def _collect_nat_less_constraints(
    expr: ResolvedTypeExpr, out: list[ResolvedField | ResolvedConstraint]
) -> None:
    """Recursively scan a type expression for #< n with non-constant n."""
    match expr:
        case TypeApply(type=t, arguments=args) if t is NatLess_type:
            arg = args[0]
            assert not isinstance(
                arg, TypeApply | TypeParamRef | CellRefType | TupleType | AnonymousRecordType
            )
            if not isinstance(arg, NatLiteral):
                out.append(ResolvedConstraint(op=CompareOp.GT, left=arg, right=NatLiteral(0)))
        case TypeApply(arguments=args):
            for a in args:
                if isinstance(a, TypeApply | TupleType | CellRefType | AnonymousRecordType):
                    _collect_nat_less_constraints(a, out)
        case TupleType(element=element):
            _collect_nat_less_constraints(element, out)
        case CellRefType(inner=inner):
            _collect_nat_less_constraints(inner, out)
        case AnonymousRecordType() | TypeParamRef():
            pass


def _resolve_field(f: ExplicitField, scope: _Scope, registry: TypeRegistry) -> ResolvedField:
    condition: ResolvedNatExpr | None = None
    raw_expr = f.type_expr
    if isinstance(raw_expr, Conditional):
        condition = _resolve_nat_expr(raw_expr.selector, scope, registry)
        raw_expr = raw_expr.type_expr
    type_expr = _resolve_type_expr(raw_expr, scope, registry)
    is_nat = _is_nat_valued(type_expr)
    return ResolvedField(
        name=f.name, type_expr=type_expr, is_nat_valued=is_nat, condition=condition
    )


def _resolve_constraint(
    c: Constraint, scope: _Scope, registry: TypeRegistry, params: list[ParamDef]
) -> ResolvedConstraint:
    """Resolve a constraint expression, detecting negated (output) variables."""
    expr = c.expr
    if not isinstance(expr, Compare):
        raise SemaError("constraint must be a comparison expression")

    left = _resolve_nat_expr(expr.left, scope, registry)
    right = _resolve_nat_expr(expr.right, scope, registry)

    negated = _find_negated_in_ast(expr, params)

    return ResolvedConstraint(op=expr.op, left=left, right=right, negated_param=negated)


def _find_negated_in_ast(expr: Compare, params: list[ParamDef]) -> NatParamDef | None:
    """Find a ~param in a comparison's AST subtree. Errors if there are multiple."""
    found: list[NatParamDef] = []

    def scan(e: TypeExpr) -> None:
        if isinstance(e, NegatedIdentifier):
            for p in params:
                if p.name == e.name and isinstance(p, NatParamDef):
                    found.append(p)
                    return
        if isinstance(e, Add | Multiply):
            scan(e.left)
            scan(e.right)

    scan(expr.left)
    scan(expr.right)

    if len(found) > 1:
        names = ", ".join(p.name for p in found)
        raise SemaError(f"constraint has multiple negated params: {names}; only one allowed")

    return found[0] if found else None


def _is_nat_valued(expr: ResolvedTypeExpr) -> bool:
    if isinstance(expr, TypeApply):
        return expr.type.produces_nat
    return False


def _check_no_field_refs_in_expr(expr: ResolvedExpr, context: str) -> None:
    """Verify a resolved expression doesn't reference explicit fields."""
    if isinstance(expr, NatFieldValue):
        raise SemaError(f"{context}: cannot reference field '{expr.field.name}' in result param")
    if isinstance(expr, NatAdd | NatMul | NatSub):
        _check_no_field_refs_in_expr(expr.left, context)
        _check_no_field_refs_in_expr(expr.right, context)
    if isinstance(expr, NatGetBit):
        _check_no_field_refs_in_expr(expr.value, context)
        _check_no_field_refs_in_expr(expr.bit, context)


def _check_type_apply_arity(expr: ResolvedTypeExpr) -> None:
    """Check that TypeApply nodes have the correct number of arguments."""
    if isinstance(expr, TypeApply):
        expected = expr.type.arity
        actual = len(expr.arguments)
        if actual != expected:
            raise SemaError(f"type '{expr.type.name}' expects {expected} arguments, got {actual}")
        for tlp, arg in zip(expr.type.type_level_params, expr.arguments, strict=True):
            if tlp.kind == ParamKind.NAT and isinstance(arg, TypeParamRef):
                raise SemaError(
                    (
                        f"type '{expr.type.name}' expects a nat argument at position "
                        f"{tlp.position}, got Type parameter '{arg.param.name}'"
                    )
                )
        if expr.type is NatLess_type:
            arg = expr.arguments[0]
            if isinstance(arg, NatLiteral) and arg.value == 0:
                raise SemaError("#< 0 is invalid: no values exist below 0")
        for arg in expr.arguments:
            if isinstance(arg, TypeApply | TupleType | CellRefType | AnonymousRecordType):
                _check_type_apply_arity(arg)
    elif isinstance(expr, TupleType):
        _check_type_apply_arity(expr.element)
    elif isinstance(expr, CellRefType):
        _check_type_apply_arity(expr.inner)
    elif isinstance(expr, AnonymousRecordType):
        for c in expr.type.constructors:
            for f in c.fields:
                _check_type_apply_arity(f.type_expr)


def _resolve_expr(expr: TypeExpr, scope: _Scope, registry: TypeRegistry) -> ResolvedExpr:
    match expr:
        case IntConst(value=v):
            return NatLiteral(v)

        case Identifier(name=name):
            return _resolve_identifier(name, scope, registry)

        case NegatedIdentifier(name=name):
            return _resolve_negated_identifier(name, scope)

        case Apply(function=func, arguments=args):
            resolved_func = _resolve_expr(func, scope, registry)
            resolved_args: list[ResolvedExpr] = []
            for a in args:
                resolved_args.append(_resolve_expr(a, scope, registry))
            if isinstance(resolved_func, TypeApply):
                return TypeApply(
                    type=resolved_func.type,
                    arguments=resolved_func.arguments + resolved_args,
                )
            if isinstance(resolved_func, AnonymousRecordType):
                return TypeApply(
                    type=resolved_func.type,
                    arguments=resolved_args,
                )
            raise SemaError("cannot apply non-type expression")

        case Add(left=left, right=right):
            return NatAdd(
                left=_resolve_nat_expr(left, scope, registry),
                right=_resolve_nat_expr(right, scope, registry),
            )

        case Multiply(left=left, right=right):
            rl = _resolve_expr(left, scope, registry)
            rr = _resolve_expr(right, scope, registry)
            if _is_resolved_nat(rl) and _is_resolved_nat(rr):
                return NatMul(left=_as_nat(rl), right=_as_nat(rr))
            elif _is_resolved_nat(rl) and not _is_resolved_nat(rr):
                return TupleType(count=_as_nat(rl), element=_as_type(rr))
            else:
                raise SemaError("left operand of '*' must be a nat expression")

        case GetBit(value=value, bit=bit):
            return NatGetBit(
                value=_resolve_nat_expr(value, scope, registry),
                bit=_resolve_nat_expr(bit, scope, registry),
            )

        case Conditional():
            raise SemaError(
                "conditional (?) can only appear at field level, not nested in type expressions"
            )

        case CellRef(inner=Identifier(name="Cell")):
            return TypeApply(type=CellRef_type, arguments=[])

        case CellRef(inner=inner):
            return CellRefType(inner=_resolve_type_expr(inner, scope, registry))

        case Compare():
            raise SemaError("comparison expressions are only valid inside constraints")

        case InlineRecord(fields=field_defs):
            return _resolve_inline_record(field_defs, registry)


def _resolve_identifier(name: str, scope: _Scope, registry: TypeRegistry) -> ResolvedExpr:
    entry = scope.lookup(name)
    if entry is not None:
        param, field = entry
        if param is not None:
            match param:
                case NatParamDef():
                    return NatParamRef(param)
                case TypeParamDef():
                    return TypeParamRef(param)
        assert field is not None
        if field.condition is not None:
            raise SemaError(f"conditional field '{name}' cannot be used in expressions")
        if field.is_nat_valued:
            return NatFieldValue(field)
        else:
            raise SemaError(f"field '{name}' is not nat-valued and cannot be used in expressions")

    resolved_type = registry.lookup(name)
    if resolved_type is not None:
        return TypeApply(type=resolved_type, arguments=[])

    raise SemaError(f"undefined type or identifier '{name}'")


def _resolve_negated_identifier(name: str, scope: _Scope) -> NatParamRef:
    entry = scope.lookup(name)
    if entry is None:
        raise SemaError(f"undefined identifier '~{name}'")
    param, _ = entry
    if param is None:
        raise SemaError(f"'~{name}' must refer to an implicit parameter, not a field")
    if not isinstance(param, NatParamDef):
        raise SemaError(f"cannot negate Type parameter '{name}'")
    return NatParamRef(param)


def _resolve_nat_expr(expr: TypeExpr, scope: _Scope, registry: TypeRegistry) -> ResolvedNatExpr:
    resolved = _resolve_expr(expr, scope, registry)
    return _as_nat(resolved)


def _resolve_type_expr(expr: TypeExpr, scope: _Scope, registry: TypeRegistry) -> ResolvedTypeExpr:
    resolved = _resolve_expr(expr, scope, registry)
    return _as_type(resolved)


def _resolve_inline_record(
    field_defs: list[FieldDef], registry: TypeRegistry
) -> AnonymousRecordType:
    anon_type = registry.register_anonymous()
    fields: list[ResolvedField] = []
    params: list[ParamDef] = []

    inner_scope = _Scope()

    # Pre-count TYPE params to create TypeLevelParams first
    type_param_count = sum(1 for f in field_defs if isinstance(f, ImplicitParam) and f.is_type)
    type_level_params = [
        TypeLevelParam(position=i, kind=ParamKind.TYPE, is_output=False)
        for i in range(type_param_count)
    ]
    anon_type.type_level_params = type_level_params

    type_param_idx = 0
    for f in field_defs:
        if isinstance(f, ExplicitField):
            rf = _resolve_field(f, inner_scope, registry)
            fields.append(rf)
            if f.name is not None:
                inner_scope.add_field(f.name, rf)
        elif isinstance(f, ImplicitParam):
            p: ParamDef
            if f.is_type:
                p = TypeParamDef(name=f.name, type_level_param=type_level_params[type_param_idx])
                type_param_idx += 1
            else:
                p = NatParamDef(name=f.name)
            params.append(p)
            inner_scope.add_param(p)

    type_params = [p for p in params if isinstance(p, TypeParamDef)]

    result_param_exprs: dict[int, ResolvedExpr] = {}
    for i, p in enumerate(type_params):
        result_param_exprs[i] = TypeParamRef(p)

    constructor = ResolvedConstructor(
        name=None,
        tag_bits="",
        tag_len=0,
        parent_type=anon_type,
        is_special=False,
        params=params,
        fields=fields,
        source_order=list(fields),
        result_param_exprs=result_param_exprs,
    )
    anon_type.constructors.append(constructor)
    return AnonymousRecordType(type=anon_type)


def _is_resolved_nat(expr: ResolvedExpr) -> bool:
    return isinstance(expr, NatLiteral | NatParamRef | NatFieldValue | NatAdd | NatMul | NatGetBit)


def _as_nat(expr: ResolvedExpr) -> ResolvedNatExpr:
    if _is_resolved_nat(expr):
        assert not isinstance(
            expr,
            TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType,
        )
        return expr
    raise SemaError("expected nat expression, got type expression")


def _as_type(expr: ResolvedExpr) -> ResolvedTypeExpr:
    if not _is_resolved_nat(expr):
        assert isinstance(
            expr,
            TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType,
        )
        return expr
    raise SemaError("expected type expression, got nat expression")
