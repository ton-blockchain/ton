from dataclasses import dataclass, field
from enum import Enum, auto

# ── Expression nodes ──────────────────────────────────────────────────


@dataclass
class IntConst:
    """Natural number literal: 0, 256, etc."""

    value: int


@dataclass
class Identifier:
    """Named reference — could be a type, field, or built-in like '#', '##', '#<', '#<='.

    Resolved to a concrete meaning in sema.
    """

    name: str


@dataclass
class NegatedIdentifier:
    """~ident inside an expression. Only valid on implicit parameters."""

    name: str


@dataclass
class Apply:
    """Type/function application: Hashmap n X, ## 5, Maybe X.

    The function and all its arguments are collected in one node
    (left-associative flattening of binary application).
    """

    function: TypeExpr
    arguments: list[TypeExpr]


@dataclass
class Add:
    """Natural number addition: a + b."""

    left: TypeExpr
    right: TypeExpr


@dataclass
class Multiply:
    """Multiplication or tuple.

    At parse time we cannot distinguish nat*nat multiplication from
    nat*Type tuple construction. Sema resolves this.
    """

    left: TypeExpr
    right: TypeExpr


@dataclass
class GetBit:
    """Bit selection: value.bit. Both operands must be nat (checked in sema)."""

    value: TypeExpr
    bit: TypeExpr


@dataclass
class Conditional:
    """Conditional type: selector?type_expr.

    If selector is nonzero, a value of type_expr is present; otherwise zero bits.
    """

    selector: TypeExpr
    type_expr: TypeExpr


@dataclass
class CellRef:
    """Cell reference: ^inner. The value is stored in a separate cell."""

    inner: TypeExpr


class CompareOp(Enum):
    EQ = auto()
    LT = auto()
    LE = auto()
    GT = auto()
    GE = auto()


@dataclass
class Compare:
    """Comparison constraint: a = b, a <= b, etc. Used inside { } constraints."""

    op: CompareOp
    left: TypeExpr
    right: TypeExpr


@dataclass
class InlineRecord:
    """Anonymous inline record: [field1:Type1 field2:Type2 ...].

    Fields are serialized inline (without ^) or in a referenced cell (with ^).
    """

    fields: list["FieldDef"]


type TypeExpr = (
    IntConst
    | Identifier
    | NegatedIdentifier
    | Apply
    | Add
    | Multiply
    | GetBit
    | Conditional
    | CellRef
    | Compare
    | InlineRecord
)


# ── Field definitions ─────────────────────────────────────────────────


@dataclass
class ExplicitField:
    """An explicit (serialized) field. name is None for unnamed fields."""

    name: str | None
    type_expr: TypeExpr


@dataclass
class ImplicitParam:
    """Implicit type or nat parameter: {name : Type} or {name : #}."""

    name: str
    is_type: bool  # True = Type, False = #


@dataclass
class Constraint:
    """Constraint inside { }: an expression that must hold (typically a comparison)."""

    expr: TypeExpr


type FieldDef = ExplicitField | ImplicitParam | Constraint


# ── Constructor tag ───────────────────────────────────────────────────


@dataclass
class Tag:
    """Constructor tag as parsed.

    bits:    decoded binary string (e.g. "0100" for #4). Empty if no explicit bits.
    is_auto: True when a named constructor has no explicit tag (CRC32 computed in sema).
    """

    bits: str = ""
    is_auto: bool = False


# ── Result parameter ──────────────────────────────────────────────────


@dataclass
class ResultParam:
    """A parameter in the result type position (after =).

    negated=True means the parameter is an output (~).
    """

    expr: TypeExpr
    negated: bool = False


# ── Constructor definition ────────────────────────────────────────────


@dataclass
class Constructor:
    """A single constructor definition line.

    name is None for anonymous constructors (_).
    """

    name: str | None
    tag: Tag
    fields: list[FieldDef]
    result_type: str
    result_params: list[ResultParam]
    is_special: bool = False


# ── Imports ───────────────────────────────────────────────────────────


@dataclass
class Import:
    """A `//@import <path>` directive at the top of a schema file.

    The path is opaque to sema/parser — drivers map it to an analyzed module.
    """

    path: str


# ── Top-level schema ─────────────────────────────────────────────────


@dataclass
class Schema:
    constructors: list[Constructor] = field(default_factory=list)
    imports: list[Import] = field(default_factory=list)
