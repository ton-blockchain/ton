"""CRC32 auto-tag computation."""

import zlib

from ..ast_nodes import (
    Add,
    Apply,
    CellRef,
    Compare,
    CompareOp,
    Conditional,
    Constructor,
    ExplicitField,
    GetBit,
    Identifier,
    ImplicitParam,
    InlineRecord,
    IntConst,
    Multiply,
    NegatedIdentifier,
    TypeExpr,
)
from .types import ResolvedType

# ── CRC32 auto-tag computation ───────────────────────────────────────


def compute_auto_tag(c: Constructor) -> str:
    """Compute the CRC32-based 32-bit tag for a constructor.

    Reconstructs the canonical text (same format as reference tlbc with
    mode=10: no braces, no tag) and CRC32s it.
    """
    text = _canonical_text(c)
    crc = zlib.crc32(text.encode("ascii")) & 0xFFFFFFFF
    return f"{crc:032b}"


def _canonical_text(c: Constructor) -> str:
    """Reconstruct canonical text for CRC32 computation.

    Format: name field1 field2 ... = TypeName param1 param2;
    No braces around implicits, no tag.
    """
    parts: list[str] = []

    # Constructor name (empty string if anonymous)
    parts.append(c.name or "_")

    # All fields (implicit, explicit, constraints) without braces
    for f in c.fields:
        if isinstance(f, ImplicitParam):
            kind = "Type" if f.is_type else "#"
            parts.append(f"{f.name}:{kind}")
        elif isinstance(f, ExplicitField):
            type_str = _expr_to_str(f.type_expr, 95)
            if f.name is not None:
                parts.append(f"{f.name}:{type_str}")
            else:
                parts.append(type_str)
        else:
            parts.append(_expr_to_str(f.expr, 0))

    # Result type
    result = c.result_type
    for rp in c.result_params:
        prefix = "~" if rp.negated else ""
        result += " " + prefix + _expr_to_str(rp.expr, 100)

    return " ".join(parts) + " = " + result


def _expr_to_str(expr: TypeExpr, prio: int) -> str:
    """Convert a parse AST expression to canonical text with precedence."""
    match expr:
        case IntConst(value=v):
            return str(v)

        case Identifier(name=name):
            return name

        case NegatedIdentifier(name=name):
            return f"~{name}"

        case Apply(function=func, arguments=args):
            inner = _expr_to_str(func, 90)
            for a in args:
                inner += " " + _expr_to_str(a, 91)
            return f"({inner})" if prio > 95 and args else inner

        case Add(left=left, right=right):
            inner = f"{_expr_to_str(left, 20)} + {_expr_to_str(right, 21)}"
            return f"({inner})" if prio > 20 else inner

        case Multiply(left=left, right=right):
            inner = f"{_expr_to_str(left, 30)} * {_expr_to_str(right, 31)}"
            return f"({inner})" if prio > 30 else inner

        case GetBit(value=value, bit=bit):
            inner = f"{_expr_to_str(value, 98)}.{_expr_to_str(bit, 98)}"
            return f"({inner})" if prio > 97 else inner

        case Conditional(selector=sel, type_expr=te):
            inner = f"{_expr_to_str(sel, 96)}?{_expr_to_str(te, 96)}"
            return f"({inner})" if prio > 95 else inner

        case CellRef(inner=inner_expr):
            return f"^{_expr_to_str(inner_expr, 100)}"

        case Compare(op=op, left=left, right=right):
            op_str = {
                CompareOp.EQ: "=",
                CompareOp.LT: "<",
                CompareOp.LE: "<=",
                CompareOp.GT: ">",
                CompareOp.GE: ">=",
            }[op]
            return f"{_expr_to_str(left, 20)} {op_str} {_expr_to_str(right, 20)}"

        case InlineRecord(fields=fields):
            parts: list[str] = []
            for f in fields:
                if isinstance(f, ExplicitField):
                    s = _expr_to_str(f.type_expr, 95)
                    if f.name is not None:
                        s = f"{f.name}:{s}"
                    parts.append(s)
                elif isinstance(f, ImplicitParam):
                    kind = "Type" if f.is_type else "#"
                    parts.append(f"{f.name}:{kind}")
            return "[" + " ".join(parts) + "]"


# ── Assign tags ──────────────────────────────────────────────────────


def assign_tags(resolved_type: ResolvedType, ast_constructors: list[Constructor]) -> None:
    """Assign CRC32 auto-tags to constructors that need them."""
    for rc, ac in zip(resolved_type.constructors, ast_constructors, strict=True):
        if ac.tag.is_auto:
            rc.tag_bits = compute_auto_tag(ac)
            rc.tag_len = 32
