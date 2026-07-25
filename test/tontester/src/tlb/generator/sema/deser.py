"""Deserialization plan generation and inference capability classification."""

from ..ast_nodes import CompareOp
from .types import (
    AnonymousRecordType,
    BindOutputParam,
    BindParam,
    CellRefType,
    CheckConstraint,
    DeserStep,
    InferenceInfo,
    InferenceStep,
    NatAdd,
    NatFieldValue,
    NatGetBit,
    NatLiteral,
    NatMul,
    NatParamDef,
    NatParamRef,
    NatSub,
    NatTypeArg,
    OutputExtraction,
    ParamDef,
    ParamKind,
    ReadField,
    ResolvedConstraint,
    ResolvedConstructor,
    ResolvedExpr,
    ResolvedField,
    ResolvedNatExpr,
    ResolvedType,
    ResolvedTypeExpr,
    SemaError,
    SolveConstraint,
    TupleType,
    TypeApply,
    TypeParamDef,
    TypeParamRef,
    is_nat,
)


def classify_inference(resolved_type: ResolvedType) -> list[InferenceInfo]:
    """For each Type parameter, check if output params can propagate through it.

    A Type param is inference-capable if EVERY constructor has exactly one
    non-conditional field whose type is directly TypeParamRef to the
    corresponding implicit param, and no other fields reference that param.
    """
    result: list[InferenceInfo] = []

    for tlp in resolved_type.type_level_params:
        if tlp.kind != ParamKind.TYPE:
            continue
        i = tlp.position

        info = InferenceInfo()
        if not resolved_type.constructors:
            result.append(info)
            continue

        all_have = True
        for constructor in resolved_type.constructors:
            param = _param_for_type_position(constructor, i)
            if param is None:
                all_have = False
                break
            field = _field_exposing_param(constructor, param)
            if field is None:
                all_have = False
                break
            info.constructor_field[constructor] = field

        info.is_capable = all_have
        result.append(info)

    return result


def _param_for_type_position(
    constructor: ResolvedConstructor, position: int
) -> TypeParamDef | None:
    """Find the implicit Type param at the given type-parameter position."""
    type_idx = 0
    for tlp in constructor.parent_type.type_level_params:
        if tlp.position == position:
            count = 0
            for p in constructor.params:
                if isinstance(p, TypeParamDef):
                    if count == type_idx:
                        return p
                    count += 1
            return None
        if tlp.kind == ParamKind.TYPE:
            type_idx += 1
    return None


def _field_exposing_param(
    constructor: ResolvedConstructor, param: TypeParamDef
) -> ResolvedField | None:
    """Find a non-conditional field whose type is directly TypeParamRef(param)."""
    for field in constructor.fields:
        if field.condition is not None:
            continue
        if isinstance(field.type_expr, TypeParamRef) and field.type_expr.param is param:
            return field
    return None


def build_deser_plan(constructor: ResolvedConstructor) -> list[DeserStep]:
    """Generate ordered deserialization steps for a constructor.

    First binds all params from type args (entry constraints), then processes
    fields and constraints in source order (left-to-right).
    """
    steps: list[DeserStep] = []
    known_params: set[ParamDef] = set()

    _emit_entry_bindings(constructor, known_params, steps)

    for item in constructor.source_order:
        if isinstance(item, ResolvedField):
            steps.append(ReadField(field=item))
            _emit_bindings(item, constructor, known_params, steps)
        else:
            _process_constraint(item, known_params, steps)

    unbound = set(constructor.params) - known_params
    if unbound:
        names = ", ".join(sorted(p.name for p in unbound))
        raise SemaError(
            (
                f"constructor '{constructor.name}' of type '{constructor.parent_type.name}': "
                f"parameter(s) {names} cannot be computed during deserialization"
            )
        )

    constructor.deser_steps = steps
    _validate_deser_plan(constructor)
    return steps


def _emit_entry_bindings(
    constructor: ResolvedConstructor,
    known_params: set[ParamDef],
    steps: list[DeserStep],
) -> None:
    """Bind implicit params from type arguments at the start of deserialization.

    For each non-output result param position:
    - If the expression is a bare param ref (NatParamRef/TypeParamRef): bind directly
    - If it's a constant (NatLiteral): emit a CheckConstraint
    - If it's a complex expression: solve for the unknown param(s)
    """
    for position, expr in constructor.result_param_exprs.items():
        match expr:
            case NatParamRef(param=param):
                steps.append(
                    SolveConstraint(
                        target_param=param,
                        value=NatTypeArg(param=constructor.parent_type.type_level_params[position]),
                    )
                )
                known_params.add(param)
            case TypeParamRef(param=param):
                steps.append(BindParam(target_param=param, position=position))
                known_params.add(param)
            case NatLiteral():
                steps.append(
                    CheckConstraint(
                        op=CompareOp.EQ,
                        left=NatTypeArg(param=constructor.parent_type.type_level_params[position]),
                        right=expr,
                    )
                )
            case TypeApply():
                raise SemaError(
                    (
                        f"constructor '{constructor.name}' of type '{constructor.parent_type.name}': "
                        f"result param at position {position} is a type application; "
                        "Type-kinded result params must be bare type parameter references"
                    )
                )
            case _:
                _solve_entry_expr(expr, position, constructor, known_params, steps)


def _solve_entry_expr(
    expr: ResolvedExpr,
    position: int,
    constructor: ResolvedConstructor,
    known_params: set[ParamDef],
    steps: list[DeserStep],
) -> None:
    """Solve a complex result param expression for unknown params.

    E.g. position 0 has expr (n + 1): solve NatTypeArg(0) = n + 1 → n = NatTypeArg(0) - 1.
    """
    if not is_nat(expr):
        raise SemaError(
            f"constructor '{constructor.name}': non-nat expression at result position {position}"
        )
    nat_expr = _as_nat(expr)

    target = _find_unknown_nat_param(nat_expr, known_params)
    if target is None:
        steps.append(
            CheckConstraint(
                op=CompareOp.EQ,
                left=NatTypeArg(param=constructor.parent_type.type_level_params[position]),
                right=nat_expr,
            )
        )
        return

    solved = _isolate_param(
        nat_expr, NatTypeArg(param=constructor.parent_type.type_level_params[position]), target
    )
    if solved is not None:
        steps.append(SolveConstraint(target_param=target, value=solved))
        known_params.add(target)
    else:
        raise SemaError(
            (
                f"constructor '{constructor.name}' of type '{constructor.parent_type.name}': "
                f"cannot solve result param expression at position {position} for '{target.name}'"
            )
        )


def _find_unknown_nat_param(expr: ResolvedNatExpr, known: set[ParamDef]) -> NatParamDef | None:
    """Find a NatParamRef in a nat expression that isn't in the known set."""
    match expr:
        case NatParamRef(param=param) if param not in known:
            return param
        case (
            NatAdd(left=left, right=right)
            | NatSub(left=left, right=right)
            | NatMul(left=left, right=right)
        ):
            return _find_unknown_nat_param(left, known) or _find_unknown_nat_param(right, known)
        case NatGetBit(value=value, bit=bit):
            return _find_unknown_nat_param(value, known) or _find_unknown_nat_param(bit, known)
        case _:
            return None


def _as_nat(expr: ResolvedExpr) -> ResolvedNatExpr:
    assert is_nat(expr)
    assert not isinstance(
        expr, TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType
    )
    return expr


def _emit_bindings(
    field: ResolvedField,
    constructor: ResolvedConstructor,
    known_params: set[ParamDef],
    steps: list[DeserStep],
) -> None:
    """After reading a field, emit BindOutputParam for extractable output params."""
    _scan_for_outputs(field, field.type_expr, constructor, known_params, steps, [])


def _scan_for_outputs(
    source_field: ResolvedField,
    type_expr: ResolvedTypeExpr,
    constructor: ResolvedConstructor,
    known_params: set[ParamDef],
    steps: list[DeserStep],
    chain: list[InferenceStep],
) -> None:
    """Recursively scan a type expression for output param bindings."""
    if not isinstance(type_expr, TypeApply):
        return

    applied_type = type_expr.type

    for arg_idx, arg in enumerate(type_expr.arguments):
        if not isinstance(arg, NatParamRef):
            continue
        tlp = applied_type.type_level_params[arg_idx]
        if not tlp.is_output:
            continue
        if source_field.condition is not None:
            raise SemaError(
                (
                    f"constructor '{constructor.name}' of type '{constructor.parent_type.name}': "
                    f"output parameter '{arg.param.name}' cannot be inferred from "
                    f"conditional field '{source_field.name}'"
                )
            )
        if arg.param in known_params:
            raise SemaError(
                (
                    f"constructor '{constructor.name}' of type '{constructor.parent_type.name}': "
                    f"parameter '{arg.param.name}' is bound from multiple output (~) positions"
                )
            )
        extraction = OutputExtraction(
            source_field=source_field,
            chain=list(chain),
            result_param_position=arg_idx,
        )
        steps.append(BindOutputParam(target_param=arg.param, extraction=extraction))
        known_params.add(arg.param)

    for arg_idx, arg in enumerate(type_expr.arguments):
        if not isinstance(arg, TypeApply):
            continue
        inf_idx = _inference_index_for_param(applied_type, arg_idx)
        if inf_idx is not None and inf_idx < len(applied_type.inference):
            if applied_type.inference[inf_idx].is_capable:
                new_chain = chain + [
                    InferenceStep(type=applied_type, param_idx=arg_idx, concrete_arg=arg)
                ]
                _scan_for_outputs(source_field, arg, constructor, known_params, steps, new_chain)


def _inference_index_for_param(resolved_type: ResolvedType, arg_position: int) -> int | None:
    """Map an argument position to an inference info index (for Type params only)."""
    if arg_position >= len(resolved_type.type_level_params):
        return None
    if resolved_type.type_level_params[arg_position].kind != ParamKind.TYPE:
        return None
    count = 0
    for i in range(arg_position):
        if resolved_type.type_level_params[i].kind == ParamKind.TYPE:
            count += 1
    return count


def _process_constraint(
    constraint: ResolvedConstraint,
    known_params: set[ParamDef],
    steps: list[DeserStep],
) -> None:
    """Process a resolved constraint: either solve for a ~variable or emit a check."""
    if constraint.negated_param is not None and constraint.negated_param not in known_params:
        solved = _solve_for_negated(constraint)
        if solved is not None:
            steps.append(SolveConstraint(target_param=constraint.negated_param, value=solved))
            known_params.add(constraint.negated_param)
            return

    steps.append(CheckConstraint(op=constraint.op, left=constraint.left, right=constraint.right))


def _solve_for_negated(constraint: ResolvedConstraint) -> ResolvedNatExpr | None:
    """Solve an equality constraint for its negated variable.

    Handles patterns like:
        n = (~m) + l      → m = n - l
        n = l + (~m)      → m = n - l
        n = (~m) + l + 1  → m = n - l - 1
        (~m) = n          → m = n
    """
    assert constraint.negated_param is not None

    if constraint.op != constraint.op.EQ:
        return None  # Can only solve equalities

    target = constraint.negated_param

    left_has = _expr_references_param(constraint.left, target)
    right_has = _expr_references_param(constraint.right, target)

    if left_has and not right_has:
        return _isolate_param(constraint.left, constraint.right, target)
    elif right_has and not left_has:
        return _isolate_param(constraint.right, constraint.left, target)

    return None


def _expr_references_param(expr: ResolvedNatExpr, param: NatParamDef) -> bool:
    """Check if a resolved nat expression references a specific param."""
    match expr:
        case NatParamRef(param=p):
            return p is param
        case (
            NatAdd(left=left, right=right)
            | NatSub(left=left, right=right)
            | NatMul(left=left, right=right)
        ):
            return _expr_references_param(left, param) or _expr_references_param(right, param)
        case NatGetBit(value=value, bit=bit):
            return _expr_references_param(value, param) or _expr_references_param(bit, param)
        case _:
            return False


def _isolate_param(
    side_with_target: ResolvedNatExpr,
    other_side: ResolvedNatExpr,
    target: NatParamDef,
) -> ResolvedNatExpr | None:
    """Isolate target from: side_with_target = other_side.

    Returns an expression for target's value.
    """
    if isinstance(side_with_target, NatParamRef) and side_with_target.param is target:
        return other_side

    if isinstance(side_with_target, NatAdd):
        if _expr_references_param(side_with_target.left, target):
            rest = side_with_target.right
            return _isolate_param(
                side_with_target.left, NatSub(left=other_side, right=rest), target
            )
        if _expr_references_param(side_with_target.right, target):
            rest = side_with_target.left
            return _isolate_param(
                side_with_target.right, NatSub(left=other_side, right=rest), target
            )

    return None


def _validate_deser_plan(constructor: ResolvedConstructor) -> None:
    """Assert that every expression in the deser plan only references
    values that are known at the point of evaluation."""
    known_params: set[ParamDef] = set()
    known_fields: set[int] = set()

    for step in constructor.deser_steps:
        match step:
            case SolveConstraint():
                _assert_nat_deps_met(step.value, known_params, known_fields, constructor)
                known_params.add(step.target_param)
            case BindParam():
                known_params.add(step.target_param)
            case CheckConstraint():
                _assert_nat_deps_met(step.left, known_params, known_fields, constructor)
                _assert_nat_deps_met(step.right, known_params, known_fields, constructor)
            case ReadField():
                known_fields.add(id(step.field))
            case BindOutputParam():
                assert id(step.extraction.source_field) in known_fields, (
                    f"BindOutputParam references unread field '{step.extraction.source_field.name}'"
                )
                known_params.add(step.target_param)

    for nv in constructor.nat_param_values:
        if nv is not None:
            _assert_nat_deps_met(nv, known_params, known_fields, constructor)


def _assert_nat_deps_met(
    expr: ResolvedNatExpr,
    known_params: set[ParamDef],
    known_fields: set[int],
    constructor: ResolvedConstructor,
) -> None:
    """Assert all param/field references in a nat expression are known."""
    match expr:
        case NatParamRef(param=param):
            assert param in known_params, (
                f"constructor '{constructor.name}': expression references unbound param '{param.name}'"
            )
        case NatFieldValue(field=field):
            assert id(field) in known_fields, (
                f"constructor '{constructor.name}': expression references unread field '{field.name}'"
            )
        case (
            NatAdd(left=left, right=right)
            | NatSub(left=left, right=right)
            | NatMul(left=left, right=right)
        ):
            _assert_nat_deps_met(left, known_params, known_fields, constructor)
            _assert_nat_deps_met(right, known_params, known_fields, constructor)
        case NatGetBit(value=value, bit=bit):
            _assert_nat_deps_met(value, known_params, known_fields, constructor)
            _assert_nat_deps_met(bit, known_params, known_fields, constructor)
        case NatLiteral() | NatTypeArg():
            pass  # no deps
