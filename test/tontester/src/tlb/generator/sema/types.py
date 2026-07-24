"""Resolved IR types produced by semantic analysis.

All identifiers are resolved, nat vs type expressions are separated into
distinct union types, and deserialization plans are computed.
"""

import functools
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Protocol, TypeIs

from ..ast_nodes import CompareOp


class SemaError(Exception):
    pass


@dataclass(frozen=True)
class Module:
    name: str


class ParamKind(Enum):
    NAT = auto()
    TYPE = auto()


class WellKnownType(Enum):
    MAYBE = auto()
    UNIT = auto()
    BOOL = auto()
    BOOL_TRUE = auto()
    BOOL_FALSE = auto()
    UNARY = auto()
    BIT = auto()
    HASHMAP_E = auto()
    HASHMAP = auto()
    HASHMAP_AUG_E = auto()
    HASHMAP_AUG = auto()
    VAR_UINTEGER = auto()
    VAR_INTEGER = auto()


@dataclass(frozen=True)
class NatParamDef:
    name: str


@dataclass(frozen=True)
class TypeParamDef:
    name: str
    type_level_param: TypeLevelParam


type ParamDef = TypeParamDef | NatParamDef


@dataclass(frozen=True)
class TypeLevelParam:
    """Sentinel for a type-level parameter position.

    Used as a scope binding key so that NatTypeArg(position) can resolve
    to the correct Python variable name. One per position on the type.
    """

    position: int
    kind: ParamKind
    is_output: bool


@dataclass(frozen=True)
class NatLiteral:
    value: int
    references_params: bool = False
    references_type_arg: bool = False


@dataclass(frozen=True)
class NatParamRef:
    param: NatParamDef
    references_params: bool = True
    references_type_arg: bool = False


@dataclass(frozen=True)
class NatFieldValue:
    """Runtime nat value of an explicit field (e.g. len:(#< n) → len is a nat)."""

    field: ResolvedField
    references_params: bool = True
    references_type_arg: bool = False


class _NatBinOpMixin(Protocol):
    left: ResolvedNatExpr
    right: ResolvedNatExpr

    @functools.cached_property
    def references_params(self) -> bool:
        return self.left.references_params or self.right.references_params

    @functools.cached_property
    def references_type_arg(self) -> bool:
        return self.left.references_type_arg or self.right.references_type_arg


@dataclass(frozen=True)
class NatAdd(_NatBinOpMixin):
    left: ResolvedNatExpr
    right: ResolvedNatExpr


@dataclass(frozen=True)
class NatSub(_NatBinOpMixin):
    """Only produced by constraint solving (e.g. m = n - l)."""

    left: ResolvedNatExpr
    right: ResolvedNatExpr


@dataclass(frozen=True)
class NatMul(_NatBinOpMixin):
    left: ResolvedNatExpr
    right: ResolvedNatExpr


@dataclass(frozen=True)
class NatGetBit:
    value: ResolvedNatExpr
    bit: ResolvedNatExpr

    @functools.cached_property
    def references_params(self) -> bool:
        return self.value.references_params or self.bit.references_params

    @functools.cached_property
    def references_type_arg(self) -> bool:
        return self.value.references_type_arg or self.bit.references_type_arg


@dataclass(frozen=True)
class NatTypeArg:
    """Value of the nat type argument at the given result param position.

    Known from the caller at deserialization time.
    """

    param: TypeLevelParam
    references_params: bool = False
    references_type_arg: bool = True


type ResolvedNatExpr = (
    NatLiteral | NatParamRef | NatFieldValue | NatAdd | NatSub | NatMul | NatGetBit | NatTypeArg
)


@dataclass(frozen=True)
class TypeParamRef:
    param: TypeParamDef
    references_type_params: bool = True


@dataclass(frozen=True)
class TypeApply:
    type: ResolvedType
    arguments: list[ResolvedExpr]

    @functools.cached_property
    def references_type_params(self) -> bool:
        for a in self.arguments:
            if is_nat(a):
                if a.references_params:
                    return True
            elif a.references_type_params:
                return True
        return False


@dataclass(frozen=True)
class TupleType:
    """n * T — tuple of n values of type T."""

    count: ResolvedNatExpr
    element: ResolvedTypeExpr

    @functools.cached_property
    def references_type_params(self) -> bool:
        return self.count.references_params or self.element.references_type_params


@dataclass(frozen=True)
class CellRefType:
    """^T — value in a referenced cell."""

    inner: ResolvedTypeExpr

    @functools.cached_property
    def references_type_params(self) -> bool:
        return self.inner.references_type_params


@dataclass(frozen=True)
class AnonymousRecordType:
    """[field1:T1 field2:T2 ...] — inline anonymous record."""

    type: ResolvedType

    @functools.cached_property
    def references_type_params(self) -> bool:
        return any(f.type_expr.references_type_params for f in self.type.constructors[0].fields)


type ResolvedTypeExpr = TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType

type ResolvedExpr = ResolvedNatExpr | ResolvedTypeExpr


def is_nat(expr: ResolvedExpr) -> TypeIs[ResolvedNatExpr]:
    """Type guard: check whether a ResolvedExpr is a nat expression."""
    return isinstance(
        expr,
        NatLiteral
        | NatParamRef
        | NatFieldValue
        | NatAdd
        | NatSub
        | NatMul
        | NatGetBit
        | NatTypeArg,
    )


def is_type(expr: ResolvedExpr) -> TypeIs[ResolvedTypeExpr]:
    """Type guard: check whether a ResolvedExpr is a type expression."""
    return isinstance(
        expr,
        TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType,
    )


@dataclass(eq=False)
class ResolvedField:
    name: str | None
    type_expr: ResolvedTypeExpr
    is_nat_valued: bool = False
    condition: ResolvedNatExpr | None = None


@dataclass(frozen=True)
class InferenceStep:
    """One step through an inference-capable generic type param."""

    type: ResolvedType
    param_idx: int
    concrete_arg: ResolvedTypeExpr


@dataclass(frozen=True)
class OutputExtraction:
    """Full algorithm to extract a nat value from a deserialized field."""

    source_field: ResolvedField
    chain: list[InferenceStep]
    result_param_position: int


@dataclass(frozen=True)
class ReadField:
    field: ResolvedField


@dataclass(frozen=True)
class BindParam:
    """Bind a Type param from a type argument (always identity assignment)."""

    target_param: TypeParamDef
    position: int  # type arg position


@dataclass(frozen=True)
class BindOutputParam:
    target_param: NatParamDef
    extraction: OutputExtraction


@dataclass(frozen=True)
class SolveConstraint:
    target_param: NatParamDef
    value: ResolvedNatExpr


@dataclass(frozen=True)
class CheckConstraint:
    op: CompareOp
    left: ResolvedNatExpr
    right: ResolvedNatExpr


type DeserStep = ReadField | BindParam | BindOutputParam | SolveConstraint | CheckConstraint


@dataclass(frozen=True)
class ResolvedConstraint:
    """A resolved constraint expression (from { expr } in the source).

    For equality constraints with a negated variable, negated_param is set
    and the constraint can be solved during deserialization.
    """

    op: CompareOp
    left: ResolvedNatExpr
    right: ResolvedNatExpr
    negated_param: NatParamDef | None = None


type FieldOrConstraint = ResolvedField | ResolvedConstraint


def _collect_type_params(expr: ResolvedTypeExpr, out: set[TypeParamDef]) -> None:
    """Walk a type expression tree and collect all referenced TypeParamDefs."""
    match expr:
        case TypeParamRef(param=p):
            out.add(p)
        case TypeApply(arguments=args):
            for a in args:
                if is_type(a):
                    _collect_type_params(a, out)
        case TupleType(element=elem):
            _collect_type_params(elem, out)
        case CellRefType(inner=inner):
            _collect_type_params(inner, out)
        case AnonymousRecordType():
            pass


@dataclass(eq=False)
class ResolvedConstructor:
    name: str | None
    tag_bits: str
    tag_len: int
    parent_type: ResolvedType
    is_special: bool
    params: list[ParamDef]
    fields: list[ResolvedField]
    result_param_exprs: dict[int, ResolvedExpr] = field(default_factory=dict)
    source_order: list[FieldOrConstraint] = field(default_factory=list)
    deser_steps: list[DeserStep] = field(default_factory=list)
    nat_param_values: list[ResolvedNatExpr | None] = field(default_factory=list)

    @functools.cached_property
    def used_type_params(self) -> frozenset[TypeParamDef]:
        """Type params actually referenced by this constructor's field types."""
        result: set[TypeParamDef] = set()
        for f in self.fields:
            _collect_type_params(f.type_expr, result)
        return frozenset(result)


@dataclass(frozen=True)
class MatchTag:
    """Check/skip a known bit prefix (e.g. a 32-bit CRC tag)."""

    bits: str
    child: MatchTree
    reads_bits: bool = True


@dataclass(frozen=True)
class MatchBit:
    """Branch on the next serialized bit."""

    zero: MatchTree
    one: MatchTree
    reads_bits: bool = True


@dataclass(frozen=True)
class MatchConstraint:
    """Branch on a type-arg constraint."""

    condition: CheckConstraint
    if_true: MatchTree
    if_false: MatchTree

    @functools.cached_property
    def reads_bits(self) -> bool:
        return self.if_true.reads_bits or self.if_false.reads_bits


@dataclass(frozen=True)
class MatchConstructor:
    """Leaf — matched a constructor."""

    constructor: ResolvedConstructor
    reads_bits: bool = False


@dataclass(frozen=True)
class MatchFail:
    """No constructor matches — deserialization error."""

    reads_bits: bool = False


type MatchTree = MatchTag | MatchBit | MatchConstraint | MatchConstructor | MatchFail


@dataclass
class InferenceInfo:
    """Per-Type-param: can output params propagate through this param?"""

    is_capable: bool = False
    constructor_field: dict[ResolvedConstructor, ResolvedField] = field(default_factory=dict)


@dataclass(eq=False)
class ResolvedType:
    # Intrinsic properties
    name: str
    type_level_params: list[TypeLevelParam] = field(default_factory=list)
    constructors: list[ResolvedConstructor] = field(default_factory=list)

    produces_nat: bool = False
    is_builtin: bool = False
    is_special: bool = False  # all constructors have ! prefix (exotic cell type)

    origin_module: Module | None = None

    # Computed properties
    is_enum: bool = False
    is_typedef: bool = False
    typedef_target: ResolvedTypeExpr | None = None

    match_tree: MatchTree | None = None
    inference: list[InferenceInfo] = field(default_factory=list)
    well_known: WellKnownType | None = None

    @property
    def arity(self):
        return len(self.type_level_params)

    @property
    def has_sole_constructor(self) -> bool:
        return len(self.constructors) == 1

    @property
    def has_unnamed_sole_constructor(self) -> bool:
        return self.has_sole_constructor and self.constructors[0].name is None
