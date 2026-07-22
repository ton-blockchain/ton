"""Match tree construction for constructor dispatch.

Algorithm:
1. Try partial constraint split on nat type args (may reduce but not fully resolve)
2. Expand typedef chains: map each constructor to a set of (inner_constructor, tag_offset)
   pairs by following first-field types until all have unconsumed tag bits
3. Consume common bit prefix
4. Split on next diverging bit, recurse

Errors if any constructor's expansion runs out of tag bits (ambiguous).
"""

from ..ast_nodes import CompareOp
from .types import (
    CellRefType,
    CheckConstraint,
    MatchBit,
    MatchConstraint,
    MatchConstructor,
    MatchFail,
    MatchTag,
    MatchTree,
    NatLiteral,
    NatTypeArg,
    ParamKind,
    ResolvedConstructor,
    ResolvedType,
    SemaError,
    TypeApply,
    TypeLevelParam,
)

MAX_DEPTH = 10


def build_match_tree(resolved_type: ResolvedType) -> MatchTree:
    if not resolved_type.constructors:
        return MatchFail()
    if len(resolved_type.constructors) == 1:
        c = resolved_type.constructors[0]
        if c.tag_len > 0:
            return MatchTag(bits=c.tag_bits, child=MatchConstructor(constructor=c))
        return MatchConstructor(constructor=c)

    cons_by_id: dict[int, ResolvedConstructor] = {}
    _register_constructors(resolved_type, cons_by_id)

    state: dict[int, _ExpState] = {}
    for c in resolved_type.constructors:
        state[id(c)] = _ExpState(
            constructor=c,
            expansions={(id(c), 0)},
        )

    return _dispatch(list(state.values()), resolved_type, 0, cons_by_id)


class _ExpState:
    """Tracks where we are in a constructor's expanded tag.

    Each entry in `expansions` is (inner_constructor_id, tag_offset), meaning
    this constructor could currently be at `tag_offset` bits into
    `inner_constructor`'s tag.
    """

    constructor: ResolvedConstructor
    expansions: set[tuple[int, int]]

    def __init__(
        self,
        constructor: ResolvedConstructor,
        expansions: set[tuple[int, int]],
    ) -> None:
        self.constructor = constructor
        self.expansions = expansions


def _register_constructors(
    resolved_type: ResolvedType, cons_by_id: dict[int, ResolvedConstructor]
) -> None:
    """Register all constructors (including nested types) for id-based lookup."""
    visited: set[int] = set()
    _register_type(resolved_type, visited, cons_by_id)


def _register_type(
    t: ResolvedType, visited: set[int], cons_by_id: dict[int, ResolvedConstructor]
) -> None:
    if id(t) in visited:
        return
    visited.add(id(t))
    for c in t.constructors:
        cons_by_id[id(c)] = c
        for f in c.fields:
            if isinstance(f.type_expr, TypeApply):
                _register_type(f.type_expr.type, visited, cons_by_id)


def _dispatch(
    states: list[_ExpState],
    resolved_type: ResolvedType,
    depth: int,
    cons_by_id: dict[int, ResolvedConstructor],
) -> MatchTree:
    if len(states) == 0:
        return MatchFail()
    if len(states) == 1:
        return MatchConstructor(constructor=states[0].constructor)
    if depth >= MAX_DEPTH:
        raise SemaError(
            (
                f"type '{resolved_type.name}': constructor disambiguation exceeded "
                f"max depth {MAX_DEPTH}"
            )
        )

    split = _try_constraint_split(states, resolved_type, cons_by_id)
    if split is not None:
        return split

    _expand_all(states, cons_by_id)

    common = _common_prefix(states, cons_by_id)
    if common:
        _advance_all(states, len(common))
        child = _dispatch(states, resolved_type, depth + 1, cons_by_id)
        return MatchTag(bits=common, child=child)

    _check_all_have_bits(states, resolved_type, cons_by_id)
    return _dispatch_on_bits(states, resolved_type, depth, cons_by_id)


def _dispatch_on_bits(
    states: list[_ExpState],
    resolved_type: ResolvedType,
    depth: int,
    cons_by_id: dict[int, ResolvedConstructor],
) -> MatchTree:
    """Split states on the next bit and recurse."""
    zero_states: list[_ExpState] = []
    one_states: list[_ExpState] = []

    for s in states:
        zero_exps: set[tuple[int, int]] = set()
        one_exps: set[tuple[int, int]] = set()
        for cid, offset in s.expansions:
            c = cons_by_id[cid]
            bit = c.tag_bits[offset]
            if bit == "0":
                zero_exps.add((cid, offset + 1))
            else:
                one_exps.add((cid, offset + 1))
        if zero_exps:
            zero_states.append(_ExpState(s.constructor, zero_exps))
        if one_exps:
            one_states.append(_ExpState(s.constructor, one_exps))

    return MatchBit(
        zero=_dispatch(zero_states, resolved_type, depth + 1, cons_by_id),
        one=_dispatch(one_states, resolved_type, depth + 1, cons_by_id),
    )


def _expand_all(states: list[_ExpState], cons_by_id: dict[int, ResolvedConstructor]) -> None:
    """Expand typedef chains until all expansions have unconsumed tag bits.

    When an (inner_constructor, offset) has consumed all tag bits, follow
    through the first inline field's type to get the next level of
    inner constructors (each starting at offset 0).
    """
    for _ in range(100):  # convergence limit
        any_changed = False
        for s in states:
            new_expansions: set[tuple[int, int]] = set()
            changed = False
            for cid, offset in s.expansions:
                c = cons_by_id[cid]
                if offset < len(c.tag_bits):
                    new_expansions.add((cid, offset))
                else:
                    changed = True
                    inner = _follow_first_field(c)
                    if inner:
                        for ic in inner:
                            cons_by_id[id(ic)] = ic
                            new_expansions.add((id(ic), 0))
                    else:
                        # Can't extend — put back as-is (will trigger error)
                        new_expansions.add((cid, offset))

            if changed:
                s.expansions = new_expansions
                any_changed = True

        if not any_changed:
            break


def _follow_first_field(c: ResolvedConstructor) -> list[ResolvedConstructor] | None:
    """Follow a constructor's first inline field to get the target type's constructors."""
    for f in c.fields:
        if isinstance(f.type_expr, CellRefType):
            continue
        if isinstance(f.type_expr, TypeApply):
            target = f.type_expr.type
            if target.constructors:
                return target.constructors
        return None
    return None


def _check_all_have_bits(
    states: list[_ExpState],
    resolved_type: ResolvedType,
    cons_by_id: dict[int, ResolvedConstructor],
) -> None:
    """Verify every expansion has at least 1 unconsumed tag bit."""
    for s in states:
        for cid, offset in s.expansions:
            c = cons_by_id[cid]
            if offset >= len(c.tag_bits):
                raise SemaError(
                    (
                        f"type '{resolved_type.name}': constructor '{s.constructor.name}' "
                        "is ambiguous — tag bits exhausted during disambiguation"
                    )
                )


def _common_prefix(states: list[_ExpState], cons_by_id: dict[int, ResolvedConstructor]) -> str:
    """Find the longest common bit prefix across all expansions."""
    all_bits: list[str] = []
    for s in states:
        for cid, offset in s.expansions:
            c = cons_by_id[cid]
            all_bits.append(c.tag_bits[offset:])

    if not all_bits:
        return ""

    ref = all_bits[0]
    length = 0
    while length < len(ref):
        bit = ref[length]
        if all(length < len(b) and b[length] == bit for b in all_bits):
            length += 1
        else:
            break

    return ref[:length]


def _advance_all(states: list[_ExpState], n: int) -> None:
    """Advance all expansions by n bits."""
    for s in states:
        s.expansions = {(cid, offset + n) for cid, offset in s.expansions}


def _try_constraint_split(
    states: list[_ExpState],
    resolved_type: ResolvedType,
    cons_by_id: dict[int, ResolvedConstructor],
) -> MatchTree | None:
    """Try splitting on nat type-arg constraints. Returns None if not useful."""
    parent = resolved_type

    for tlp in parent.type_level_params:
        if tlp.kind != ParamKind.NAT or tlp.is_output:
            continue
        pos = tlp.position

        values: list[tuple[_ExpState, int | None]] = []
        for s in states:
            values.append((s, _constant_value_at(s.constructor, pos)))

        consts: list[tuple[_ExpState, int | None]] = [(s, v) for s, v in values if v is not None]
        non_consts: list[tuple[_ExpState, int | None]] = [(s, v) for s, v in values if v is None]

        if not consts:
            continue

        const_vals = sorted({v for _, v in consts if v is not None})
        if len(const_vals) < 2 and not non_consts:
            continue
        if not const_vals:
            continue

        result = _build_split(tlp, consts, non_consts, resolved_type, cons_by_id)
        if result is not None:
            return result

    return None


def _constant_value_at(c: ResolvedConstructor, position: int) -> int | None:
    expr = c.result_param_exprs.get(position)
    if expr is not None and isinstance(expr, NatLiteral):
        return expr.value
    return None


def _build_split(
    tlp: TypeLevelParam,
    consts: list[tuple[_ExpState, int | None]],
    non_consts: list[tuple[_ExpState, int | None]],
    resolved_type: ResolvedType,
    cons_by_id: dict[int, ResolvedConstructor],
) -> MatchTree | None:
    const_vals = sorted(set(v for _, v in consts if v is not None))

    if len(const_vals) == 1 and non_consts:
        val = const_vals[0]
        condition = CheckConstraint(
            op=CompareOp.EQ, left=NatTypeArg(param=tlp), right=NatLiteral(val)
        )
        true_states = [s for s, v in consts if v == val]
        false_states = [s for s, _ in non_consts]
        return MatchConstraint(
            condition=condition,
            if_true=_dispatch(true_states, resolved_type, 0, cons_by_id),
            if_false=_dispatch(false_states, resolved_type, 0, cons_by_id),
        )

    if len(const_vals) >= 2:
        mid_idx = len(const_vals) // 2
        mid_val = const_vals[mid_idx - 1]
        condition = CheckConstraint(
            op=CompareOp.LE, left=NatTypeArg(param=tlp), right=NatLiteral(mid_val)
        )
        true_states = [s for s, v in consts if v is not None and v <= mid_val]
        false_states = [s for s, v in consts if v is not None and v > mid_val]
        false_states += [s for s, _ in non_consts]
        return MatchConstraint(
            condition=condition,
            if_true=_dispatch(true_states, resolved_type, 0, cons_by_id),
            if_false=_dispatch(false_states, resolved_type, 0, cons_by_id),
        )

    return None
