"""Detection of well-known TL-B type patterns.

All well-known types are defined in a single canonical TL-B schema string. At
module load time, this is parsed and resolved into reference ResolvedType objects.
User types are then compared structurally against these references.
"""

from ..lexer import Lexer
from ..parser import Parser
from .resolve import TypeRegistry, register_types, resolve_constructors
from .types import (
    AnonymousRecordType,
    CellRefType,
    Module,
    NatAdd,
    NatFieldValue,
    NatGetBit,
    NatLiteral,
    NatMul,
    NatParamRef,
    NatSub,
    NatTypeArg,
    ResolvedConstructor,
    ResolvedExpr,
    ResolvedNatExpr,
    ResolvedType,
    ResolvedTypeExpr,
    TupleType,
    TypeApply,
    TypeParamRef,
    WellKnownType,
    is_type,
)

_REFERENCE_MODULE = Module(name="<well-known>")

_REFERENCE_SCHEMA = """
unit$_ = Unit;
true$_ = True;
bool_false$0 = Bool;
bool_true$1 = Bool;
bool_false$0 = BoolFalse;
bool_true$1 = BoolTrue;
nothing$0 {X:Type} = Maybe X;
just$1 {X:Type} value:X = Maybe X;

bit#_ _:(## 1) = Bit;

unary_zero$0 = Unary ~0;
unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);

hml_short$0 {m:#} {n:#} len:(Unary ~n) s:(n * Bit) = HmLabel ~n m;
hml_long$10 {m:#} n:(#<= m) s:(n * Bit) = HmLabel ~n m;
hml_same$11 {m:#} v:Bit n:(#<= m) = HmLabel ~n m;

hmn_leaf#_ {X:Type} value:X = HashmapNode 0 X;
hmn_fork#_ {n:#} {X:Type} left:^(Hashmap n X)
           right:^(Hashmap n X) = HashmapNode (n + 1) X;

hm_edge#_ {n:#} {X:Type} {l:#} {m:#}
          label:(HmLabel ~l n) {n = (~m) + l}
          node:(HashmapNode m X) = Hashmap n X;

hme_empty$0 {n:#} {X:Type} = HashmapE n X;
hme_root$1 {n:#} {X:Type} root:^(Hashmap n X) = HashmapE n X;

ahmn_leaf#_ {X:Type} {Y:Type} extra:Y value:X = HashmapAugNode 0 X Y;
ahmn_fork#_ {n:#} {X:Type} {Y:Type} left:^(HashmapAug n X Y)
  right:^(HashmapAug n X Y) extra:Y = HashmapAugNode (n + 1) X Y;

ahm_edge#_ {n:#} {X:Type} {Y:Type} {l:#} {m:#}
  label:(HmLabel ~l n) {n = (~m) + l}
  node:(HashmapAugNode m X Y) = HashmapAug n X Y;

ahme_empty$0 {n:#} {X:Type} {Y:Type} extra:Y = HashmapAugE n X Y;
ahme_root$1 {n:#} {X:Type} {Y:Type} root:^(HashmapAug n X Y)
  extra:Y = HashmapAugE n X Y;

var_uint$_ {n:#} len:(#< n) value:(uint (len * 8)) = VarUInteger n;
var_int$_ {n:#} len:(#< n) value:(int (len * 8)) = VarInteger n;
"""

_WELL_KNOWN_NAMES: list[tuple[WellKnownType, str]] = [
    (WellKnownType.UNIT, "Unit"),
    (WellKnownType.UNIT, "True"),
    (WellKnownType.BOOL, "Bool"),
    (WellKnownType.BOOL_FALSE, "BoolFalse"),
    (WellKnownType.BOOL_TRUE, "BoolTrue"),
    (WellKnownType.MAYBE, "Maybe"),
    (WellKnownType.BIT, "Bit"),
    (WellKnownType.UNARY, "Unary"),
    (WellKnownType.HASHMAP_E, "HashmapE"),
    (WellKnownType.HASHMAP, "Hashmap"),
    (WellKnownType.HASHMAP_AUG_E, "HashmapAugE"),
    (WellKnownType.HASHMAP_AUG, "HashmapAug"),
    (WellKnownType.VAR_UINTEGER, "VarUInteger"),
    (WellKnownType.VAR_INTEGER, "VarInteger"),
]


def _build_references() -> list[tuple[WellKnownType, str, ResolvedType]]:
    tokens = Lexer(_REFERENCE_SCHEMA).tokenize()
    schema = Parser(tokens).parse()
    registry = TypeRegistry(_REFERENCE_MODULE)
    register_types(schema, registry)
    resolve_constructors(schema, registry)
    types_by_name = {t.name: t for t in registry.all_user_types()}
    result: list[tuple[WellKnownType, str, ResolvedType]] = []
    for wk, name in _WELL_KNOWN_NAMES:
        ref_type = types_by_name[name]
        result.append((wk, name, ref_type))
    return result


_REFERENCES = _build_references()


def classify_well_known(rt: ResolvedType) -> None:
    """Detect well-known type patterns and set rt.well_known."""
    for wk, name, ref_type in _REFERENCES:
        if rt.name == name and _types_equivalent(rt, ref_type):
            rt.well_known = wk
            return


def _types_equivalent(user: ResolvedType, ref: ResolvedType) -> bool:
    if len(user.type_level_params) != len(ref.type_level_params):
        return False
    for up, rp in zip(user.type_level_params, ref.type_level_params):
        if up.kind != rp.kind or up.is_output != rp.is_output:
            return False
    if len(user.constructors) != len(ref.constructors):
        return False
    user_cons = sorted(user.constructors, key=_cons_sort_key)
    ref_cons = sorted(ref.constructors, key=_cons_sort_key)
    for uc, rc in zip(user_cons, ref_cons):
        if not _constructors_equivalent(uc, rc):
            return False
    return True


def _cons_sort_key(c: ResolvedConstructor) -> tuple[str, str]:
    return (c.tag_bits, c.name or "")


def _constructors_equivalent(uc: ResolvedConstructor, rc: ResolvedConstructor) -> bool:
    if uc.name != rc.name:
        return False
    if uc.tag_bits != rc.tag_bits or uc.tag_len != rc.tag_len:
        return False
    if uc.is_special != rc.is_special:
        return False
    if len(uc.params) != len(rc.params):
        return False
    for up, rp in zip(uc.params, rc.params):
        if type(up) is not type(rp):
            return False
        if up.name != rp.name:
            return False
    if len(uc.fields) != len(rc.fields):
        return False
    for uf, rf in zip(uc.fields, rc.fields):
        if uf.name != rf.name:
            return False
        if uf.is_nat_valued != rf.is_nat_valued:
            return False
        if not _type_exprs_equivalent(uf.type_expr, rf.type_expr):
            return False
    if uc.result_param_exprs.keys() != rc.result_param_exprs.keys():
        return False
    for pos in rc.result_param_exprs:
        if not _exprs_equivalent(uc.result_param_exprs[pos], rc.result_param_exprs[pos]):
            return False
    return True


def _type_exprs_equivalent(
    user_expr: ResolvedTypeExpr,
    ref_expr: ResolvedTypeExpr,
) -> bool:
    match (user_expr, ref_expr):
        case (TypeParamRef() as u, TypeParamRef() as r):
            return u.param.type_level_param.position == r.param.type_level_param.position
        case (TypeApply() as u, TypeApply() as r):
            if r.type.is_builtin:
                if u.type is not r.type:
                    return False
            else:
                if u.type.name != r.type.name:
                    return False
            if len(u.arguments) != len(r.arguments):
                return False
            for ua, ra in zip(u.arguments, r.arguments):
                if not _exprs_equivalent(ua, ra):
                    return False
            return True
        case (CellRefType() as u, CellRefType() as r):
            return _type_exprs_equivalent(u.inner, r.inner)
        case (TupleType() as u, TupleType() as r):
            return _nat_exprs_equivalent(u.count, r.count) and _type_exprs_equivalent(
                u.element, r.element
            )
        case (AnonymousRecordType() as u, AnonymousRecordType() as r):
            return _types_equivalent(u.type, r.type)
        case _:
            return False


def _exprs_equivalent(user_expr: ResolvedExpr, ref_expr: ResolvedExpr) -> bool:
    if is_type(user_expr):
        if not is_type(ref_expr):
            return False
        return _type_exprs_equivalent(user_expr, ref_expr)
    if is_type(ref_expr):
        return False
    return _nat_exprs_equivalent(user_expr, ref_expr)


def _nat_exprs_equivalent(user_expr: ResolvedNatExpr, ref_expr: ResolvedNatExpr) -> bool:
    if type(user_expr) is not type(ref_expr):
        return False

    match (user_expr, ref_expr):
        case (NatLiteral() as u, NatLiteral() as r):
            return u.value == r.value
        case (NatParamRef() as u, NatParamRef() as r):
            return u.param.name == r.param.name
        case (NatFieldValue() as u, NatFieldValue() as r):
            assert u.field.name is not None and r.field.name is not None
            return u.field.name == r.field.name
        case (NatTypeArg() as u, NatTypeArg() as r):
            return u.param.position == r.param.position
        case (NatAdd() as u, NatAdd() as r):
            return _nat_exprs_equivalent(u.left, r.left) and _nat_exprs_equivalent(u.right, r.right)
        case (NatSub() as u, NatSub() as r):
            return _nat_exprs_equivalent(u.left, r.left) and _nat_exprs_equivalent(u.right, r.right)
        case (NatMul() as u, NatMul() as r):
            return _nat_exprs_equivalent(u.left, r.left) and _nat_exprs_equivalent(u.right, r.right)
        case (NatGetBit() as u, NatGetBit() as r):
            return _nat_exprs_equivalent(u.value, r.value) and _nat_exprs_equivalent(u.bit, r.bit)
        case _:  # pyright: ignore[reportAny] basedpyright gives up trying to compute this type
            return False
