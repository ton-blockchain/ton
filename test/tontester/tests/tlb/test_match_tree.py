"""Tests for match tree construction: bit dispatch, typedef following, and constraint disambiguation."""

import pytest
from tlb.generator.ast_nodes import CompareOp
from tlb.generator.sema import analyze_text
from tlb.generator.sema.types import (
    CheckConstraint,
    MatchBit,
    MatchConstraint,
    MatchConstructor,
    MatchFail,
    MatchTag,
    MatchTree,
    Module,
    NatLiteral,
    NatTypeArg,
    ResolvedConstructor,
    ResolvedType,
    SemaError,
)

_TEST_MODULE = Module("<test>")


def analyze_type(
    text: str, type_name: str
) -> tuple[MatchTree, dict[str | None, ResolvedConstructor], ResolvedType]:
    user_types = analyze_text(text, current_module=_TEST_MODULE).types
    ts = {t.name: t for t in user_types}
    t = ts[type_name]
    assert t.match_tree is not None
    return t.match_tree, {c.name: c for c in t.constructors}, t


def analyze_cons(
    text: str, type_name: str
) -> tuple[MatchTree, list[ResolvedConstructor], ResolvedType]:
    """Like analyze_type but returns constructors as list (for anonymous constructors)."""
    user_types = analyze_text(text, current_module=_TEST_MODULE).types
    ts = {t.name: t for t in user_types}
    t = ts[type_name]
    assert t.match_tree is not None
    return t.match_tree, t.constructors, t


def L(c: ResolvedConstructor) -> MatchConstructor:
    return MatchConstructor(constructor=c)


def EQ(t: ResolvedType, pos: int, val: int) -> CheckConstraint:
    return CheckConstraint(
        op=CompareOp.EQ, left=NatTypeArg(param=t.type_level_params[pos]), right=NatLiteral(val)
    )


def LE(t: ResolvedType, pos: int, val: int) -> CheckConstraint:
    return CheckConstraint(
        op=CompareOp.LE, left=NatTypeArg(param=t.type_level_params[pos]), right=NatLiteral(val)
    )


def tree_depth(tree: MatchTree) -> int:
    match tree:
        case MatchConstructor() | MatchFail():
            return 0
        case MatchBit(zero=z, one=o):
            return 1 + max(tree_depth(z), tree_depth(o))
        case MatchConstraint(if_true=t, if_false=f):
            return 1 + max(tree_depth(t), tree_depth(f))
        case MatchTag(child=c):
            return 1 + tree_depth(c)


# ── Single constructor ────────────────────────────────────────────────


class TestSingleConstructor:
    def test_no_tag(self):
        tree, c, _ = analyze_type("unit$_ = Unit;", "Unit")
        assert tree == L(c["unit"])

    def test_with_hex_tag(self):
        tree, c, _ = analyze_type("foo#4 = Foo;", "Foo")
        assert tree == MatchTag(bits="0100", child=L(c["foo"]))

    def test_with_binary_tag(self):
        tree, c, _ = analyze_type("foo$101 = Foo;", "Foo")
        assert tree == MatchTag(bits="101", child=L(c["foo"]))

    def test_empty_explicit_tag(self):
        tree, c, _ = analyze_type("foo#_ = Foo;", "Foo")
        assert tree == L(c["foo"])

    def test_undefined_type_error(self):
        """Referencing an undefined type is an error."""
        with pytest.raises(SemaError, match="undefined type"):
            _ = analyze_text("foo$_ x:^Bar = Foo;", current_module=_TEST_MODULE)


# ── Two-constructor bit dispatch ──────────────────────────────────────


class TestTwoConstructorBits:
    def test_single_bit_split(self):
        tree, c, _ = analyze_type("a$0 = T; b$1 = T;", "T")
        assert tree == MatchBit(zero=L(c["a"]), one=L(c["b"]))

    def test_two_bit_tags(self):
        tree, c, _ = analyze_type("a$00 = T; b$11 = T;", "T")
        assert tree == MatchBit(zero=L(c["a"]), one=L(c["b"]))

    def test_common_prefix_extracted(self):
        tree, c, _ = analyze_type("a$100 = T; b$101 = T;", "T")
        assert tree == MatchTag(bits="10", child=MatchBit(zero=L(c["a"]), one=L(c["b"])))

    def test_hex_tags(self):
        tree, c, _ = analyze_type("foo#4 = T; bar#5 = T;", "T")
        assert tree == MatchTag(bits="010", child=MatchBit(zero=L(c["foo"]), one=L(c["bar"])))

    def test_long_common_prefix(self):
        tree, c, _ = analyze_type("a$11110 = T; b$11111 = T;", "T")
        assert tree == MatchTag(bits="1111", child=MatchBit(zero=L(c["a"]), one=L(c["b"])))


# ── Multi-constructor bit dispatch ────────────────────────────────────


class TestMultiConstructorBits:
    def test_three_constructors(self):
        tree, c, _ = analyze_type("a$00 = T; b$01 = T; c$1 = T;", "T")
        assert tree == MatchBit(
            zero=MatchBit(zero=L(c["a"]), one=L(c["b"])),
            one=L(c["c"]),
        )

    def test_four_balanced(self):
        tree, c, _ = analyze_type("a$00 = T; b$01 = T; c$10 = T; d$11 = T;", "T")
        assert tree == MatchBit(
            zero=MatchBit(zero=L(c["a"]), one=L(c["b"])),
            one=MatchBit(zero=L(c["c"]), one=L(c["d"])),
        )

    def test_eight_constructors(self):
        tree, c, _ = analyze_type(
            """
            a$000 = T; b$001 = T; c$010 = T; d$011 = T;
            e$100 = T; f$101 = T; g$110 = T; h$111 = T;
        """,
            "T",
        )
        assert tree == MatchBit(
            zero=MatchBit(
                zero=MatchBit(zero=L(c["a"]), one=L(c["b"])),
                one=MatchBit(zero=L(c["c"]), one=L(c["d"])),
            ),
            one=MatchBit(
                zero=MatchBit(zero=L(c["e"]), one=L(c["f"])),
                one=MatchBit(zero=L(c["g"]), one=L(c["h"])),
            ),
        )

    def test_unbalanced_prefix_lengths(self):
        tree, c, _ = analyze_type("a$0 = T; b$10 = T; c$11 = T;", "T")
        assert tree == MatchBit(
            zero=L(c["a"]),
            one=MatchBit(zero=L(c["b"]), one=L(c["c"])),
        )

    def test_three_with_common_prefix(self):
        tree, c, _ = analyze_type("a$100 = T; b$101 = T; c$11 = T;", "T")
        assert tree == MatchTag(
            bits="1",
            child=MatchBit(
                zero=MatchBit(zero=L(c["a"]), one=L(c["b"])),
                one=L(c["c"]),
            ),
        )

    def test_inmsg_pattern(self):
        tree, c, _ = analyze_type("a$000 = T; b$010 = T; c$011 = T; d$100 = T; e$101 = T;", "T")
        assert tree == MatchBit(
            zero=MatchBit(
                zero=L(c["a"]),
                one=MatchBit(zero=L(c["b"]), one=L(c["c"])),
            ),
            one=MatchTag(bits="0", child=MatchBit(zero=L(c["d"]), one=L(c["e"]))),
        )


# ── Typedef following ─────────────────────────────────────────────────


class TestTypedefFollowing:
    def test_simple_typedef(self):
        """Two anonymous typedef constructors forwarding to tagged inner types."""
        tree, cons, _ = analyze_cons(
            """
            inner#ab x:uint32 = Inner;
            other#cd y:uint64 = Other;
            _ Inner = Outer;
            _ Other = Outer;
        """,
            "Outer",
        )
        c_inner, c_other = cons[0], cons[1]
        # #ab = 10101011, #cd = 11001101. Common prefix "1". Next bit: 0 (inner) vs 1 (other).
        # Once split, each branch has one constructor → leaf (no remaining tag bits emitted).
        assert tree == MatchTag(bits="1", child=MatchBit(zero=L(c_inner), one=L(c_other)))

    def test_chain_typedef(self):
        """A -> B -> C: two-level chain. C has $0, other has $1."""
        tree, c, _ = analyze_type(
            """
            c1$0 x:uint32 = C;
            _ C = B;
            _ B = A;
            other$1 y:uint32 = A;
        """,
            "A",
        )
        assert tree == MatchBit(zero=L(c[None]), one=L(c["other"]))

    def test_typedef_to_multictor(self):
        """Inner has $00/$01/$10. Outer anonymous expands all 3. other has #ff = 11111111."""
        tree, cons, _ = analyze_cons(
            """
            x$00 = Inner;
            y$01 = Inner;
            z$10 = Inner;
            _ Inner = Outer;
            other#ff q:uint32 = Outer;
        """,
            "Outer",
        )
        c_anon, c_other = cons[0], cons[1]
        # First bit: 0 → anon only. 1 → anon (via $10) or other (via #ff).
        # Under 1: 0 → anon. 1 → other. Leaves don't emit remaining bits.
        assert tree == MatchBit(
            zero=L(c_anon),
            one=MatchBit(zero=L(c_anon), one=L(c_other)),
        )

    def test_typedef_to_multictor_prefix_conflict(self):
        with pytest.raises(SemaError, match="ambiguous"):
            _ = analyze_text(
                """
                x$00 = Inner;
                y$01 = Inner;
                z$1 = Inner;
                _ Inner = Outer;
                other#ff q:uint32 = Outer;
            """,
                current_module=_TEST_MODULE,
            )

    def test_recursive_type_no_hang(self):
        tree, c, _ = analyze_type(
            """
            unary_zero$0 = Unary ~0;
            unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);
        """,
            "Unary",
        )
        assert tree == MatchBit(zero=L(c["unary_zero"]), one=L(c["unary_succ"]))

    def test_cellref_field_not_followed(self):
        with pytest.raises(SemaError, match="ambiguous"):
            _ = analyze_text(
                """
                a#_ x:^(Inner) = T;
                b#_ y:^(Other) = T;
                inner#ab z:uint32 = Inner;
                other#cd z:uint32 = Other;
            """,
                current_module=_TEST_MODULE,
            )

    def test_msgaddress_pattern(self):
        """Two anonymous typedefs to MsgAddressInt ($10/$11) and MsgAddressExt ($00/$01)."""
        tree, cons, _ = analyze_cons(
            """
            addr_none$00 = MsgAddressExt;
            addr_extern$01 len:(## 9) = MsgAddressExt;
            addr_std$10 x:uint32 = MsgAddressInt;
            addr_var$11 x:uint32 = MsgAddressInt;
            _ _:MsgAddressInt = MsgAddress;
            _ _:MsgAddressExt = MsgAddress;
        """,
            "MsgAddress",
        )
        c_int, c_ext = cons[0], cons[1]
        assert tree == MatchBit(
            zero=L(c_ext),
            one=L(c_int),
        )

    def test_typedef_with_same_inner_tag(self):
        with pytest.raises(SemaError, match="ambiguous"):
            _ = analyze_text(
                """
                inner1#ab x:uint32 = A;
                inner2#ab y:uint32 = B;
                _ A = T;
                _ B = T;
            """,
                current_module=_TEST_MODULE,
            )

    def test_interleaved_typedef_expansion(self):
        """A:$01/$11, B:$10/$00. Expands into 4 leaves, all bit dispatch."""
        tree, cons, _ = analyze_cons(
            """
            a1$01 = A; a2$11 = A;
            b1$10 = B; b2$00 = B;
            _ A = C;
            _ B = C;
        """,
            "C",
        )
        c_a, c_b = cons[0], cons[1]
        assert tree == MatchBit(
            zero=MatchBit(zero=L(c_b), one=L(c_a)),
            one=MatchBit(zero=L(c_b), one=L(c_a)),
        )

    def test_typedef_shared_first_bit(self):
        """A:$0/$10, B:$11. Under bit 0: A only. Under bit 1: 0→A, 1→B."""
        tree, cons, _ = analyze_cons(
            """
            a1$0 = A; a2$10 = A;
            b1$11 = B;
            _ A = C;
            _ B = C;
        """,
            "C",
        )
        c_a, c_b = cons[0], cons[1]
        assert tree == MatchBit(
            zero=L(c_a),
            one=MatchBit(zero=L(c_a), one=L(c_b)),
        )


# ── Constraint-based disambiguation ──────────────────────────────────


class TestConstraintDispatch:
    def test_hashmapnode_zero_vs_succ(self):
        tree, c, t = analyze_type(
            """
            dummy$_ {n:#} {X:Type} = Hashmap n X;
            hmn_leaf#_ {X:Type} value:X = HashmapNode 0 X;
            hmn_fork#_ {n:#} {X:Type} left:^(Hashmap n X)
                       right:^(Hashmap n X) = HashmapNode (n + 1) X;
        """,
            "HashmapNode",
        )
        assert tree == MatchConstraint(
            condition=EQ(t, 0, 0),
            if_true=L(c["hmn_leaf"]),
            if_false=L(c["hmn_fork"]),
        )

    def test_two_constants(self):
        tree, cons, t = analyze_cons("_ x:uint32 = P 0; _ x:uint64 = P 1;", "P")
        c0, c1 = cons[0], cons[1]
        assert tree == MatchConstraint(condition=LE(t, 0, 0), if_true=L(c0), if_false=L(c1))

    def test_three_constants(self):
        tree, cons, t = analyze_cons(
            """
            _ x:uint32 = P 0;
            _ x:uint64 = P 1;
            _ x:bits256 = P 2;
        """,
            "P",
        )
        c0, c1, c2 = cons[0], cons[1], cons[2]
        # Split at median: LE(0, 0) → {0} vs {1, 2}. Then {1, 2} splits LE(0, 1).
        assert tree == MatchConstraint(
            condition=LE(t, 0, 0),
            if_true=L(c0),
            if_false=MatchConstraint(condition=LE(t, 0, 1), if_true=L(c1), if_false=L(c2)),
        )

    def test_five_constants(self):
        tree, cons, t = analyze_cons(
            """
            _ x:uint32 = P 0;
            _ x:uint64 = P 1;
            _ x:bits256 = P 2;
            _ y:uint32 = P 3;
            _ y:uint64 = P 4;
        """,
            "P",
        )
        c0, c1, c2, c3, c4 = cons[0], cons[1], cons[2], cons[3], cons[4]
        # Split at median(0,1,2,3,4) → LE(0, 1): {0,1} vs {2,3,4}
        assert tree == MatchConstraint(
            condition=LE(t, 0, 1),
            if_true=MatchConstraint(condition=LE(t, 0, 0), if_true=L(c0), if_false=L(c1)),
            if_false=MatchConstraint(
                condition=LE(t, 0, 2),
                if_true=L(c2),
                if_false=MatchConstraint(condition=LE(t, 0, 3), if_true=L(c3), if_false=L(c4)),
            ),
        )

    def test_ten_constants_log_depth(self):
        """10 ConfigParam-like constructors produce a balanced tree."""
        defs = "".join(f"_ x:uint32 = P {i};\n" for i in range(10))
        tree, _, _ = analyze_type(defs, "P")
        assert tree_depth(tree) <= 6  # log2(10) ≈ 3.3, with overhead ~5-6

    def test_constant_with_shared_value_error(self):
        with pytest.raises(SemaError, match="ambiguous"):
            _ = analyze_text("a#_ x:uint32 = T 0; b#_ y:uint64 = T 0;", current_module=_TEST_MODULE)

    def test_split_on_second_param(self):
        tree, cons, t = analyze_cons(
            """
            a#_ x:uint32 = T 0 0;
            b#_ x:uint64 = T 0 1;
        """,
            "T",
        )
        c0, c1 = cons[0], cons[1]
        assert tree == MatchConstraint(condition=LE(t, 1, 0), if_true=L(c0), if_false=L(c1))

    def test_no_nat_params_error(self):
        with pytest.raises(SemaError, match="ambiguous"):
            _ = analyze_text(
                """
                a#_ {X:Type} x:X = T X;
                b#_ {X:Type} y:X = T X;
            """,
                current_module=_TEST_MODULE,
            )


# ── Mixed dispatch ────────────────────────────────────────────────────


class TestMixedDispatch:
    def test_constraint_then_bits(self):
        tree, c, t = analyze_type(
            """
            a$0 x:uint32 = T 0;
            b$1 x:uint64 = T 0;
            c$0 y:uint32 = T 1;
            d$1 y:uint64 = T 1;
        """,
            "T",
        )
        assert tree == MatchConstraint(
            condition=LE(t, 0, 0),
            if_true=MatchBit(zero=L(c["a"]), one=L(c["b"])),
            if_false=MatchBit(zero=L(c["c"]), one=L(c["d"])),
        )

    def test_shardstate_no_inner_tag(self):
        with pytest.raises(SemaError, match="ambiguous"):
            _ = analyze_text(
                """
                _ x:uint32 = ShardStateUnsplit;
                _ ShardStateUnsplit = ShardState;
                split_state#5f327da5 left:^uint32 right:^uint32 = ShardState;
            """,
                current_module=_TEST_MODULE,
            )

    def test_shardstate_with_tagged_inner(self):
        """Inner type has #ab, split_state has #cd. First bit 1 (common), then diverge."""
        tree, cons, _ = analyze_cons(
            """
            inner#ab x:uint32 = ShardStateUnsplit;
            _ ShardStateUnsplit = ShardState;
            split_state#cd left:^uint32 right:^uint32 = ShardState;
        """,
            "ShardState",
        )
        c_anon, c_split = cons[0], cons[1]
        # #ab = 10101011, #cd = 11001101. Common prefix "1", then 0 vs 1.
        assert tree == MatchTag(bits="1", child=MatchBit(zero=L(c_anon), one=L(c_split)))

    def test_hashmape(self):
        tree, c, _ = analyze_type(
            """
            dummy$_ {n:#} {X:Type} = Hashmap n X;
            hme_empty$0 {n:#} {X:Type} = HashmapE n X;
            hme_root$1 {n:#} {X:Type} root:^(Hashmap n X) = HashmapE n X;
        """,
            "HashmapE",
        )
        assert tree == MatchBit(zero=L(c["hme_empty"]), one=L(c["hme_root"]))

    def test_hmlabel(self):
        tree, c, _ = analyze_type(
            """
            hml_short$0 {m:#} {n:#} len:(Unary ~n) = HmLabel ~n m;
            hml_long$10 {m:#} n:(#<= m) = HmLabel ~n m;
            hml_same$11 {m:#} v:uint32 n:(#<= m) = HmLabel ~n m;
            unary_zero$0 = Unary ~0;
            unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);
        """,
            "HmLabel",
        )
        assert tree == MatchBit(
            zero=L(c["hml_short"]),
            one=MatchBit(zero=L(c["hml_long"]), one=L(c["hml_same"])),
        )

    def test_either(self):
        tree, c, _ = analyze_type(
            """
            left$0 {X:Type} {Y:Type} value:X = Either X Y;
            right$1 {X:Type} {Y:Type} value:Y = Either X Y;
        """,
            "Either",
        )
        assert tree == MatchBit(zero=L(c["left"]), one=L(c["right"]))


# ── Edge cases ────────────────────────────────────────────────────────


class TestEdgeCases:
    def test_single_bit_tags(self):
        tree0, c0, _ = analyze_type("a$0 = T;", "T")
        assert tree0 == MatchTag(bits="0", child=L(c0["a"]))
        tree1, c1, _ = analyze_type("a$1 = T;", "T")
        assert tree1 == MatchTag(bits="1", child=L(c1["a"]))

    def test_32bit_hex_tag(self):
        tree, c, _ = analyze_type("foo#12345678 = T;", "T")
        assert isinstance(tree, MatchTag)
        assert len(tree.bits) == 32
        assert tree.child == L(c["foo"])

    def test_two_32bit_tags(self):
        tree, _, _ = analyze_type("a#12345678 = T; b#abcdef01 = T;", "T")
        # #12345678 = 00010010..., #abcdef01 = 10101011...
        # First bit differs → MatchBit
        assert isinstance(tree, MatchBit)
        # a starts with 0, b starts with 1
        # Full structure depends on common prefix within each branch
        assert tree_depth(tree) <= 10


# ── Full block.tlb ───────────────────────────────────────────────────


class TestBlockTlb:
    @pytest.fixture(scope="class")
    def block_types(self) -> dict[str, ResolvedType]:
        with open("crypto/block/block.tlb") as f:
            text = f.read()
        user_types = analyze_text(text, current_module=_TEST_MODULE).types
        return {t.name: t for t in user_types}

    def test_all_types_have_match_trees(self, block_types: dict[str, ResolvedType]):
        for t in block_types.values():
            assert t.match_tree is not None, f"{t.name} has no match tree"
            assert not isinstance(t.match_tree, MatchFail), f"{t.name} has MatchFail"

    def test_bool(self, block_types: dict[str, ResolvedType]):
        assert isinstance(block_types["Bool"].match_tree, MatchBit)

    def test_maybe(self, block_types: dict[str, ResolvedType]):
        assert isinstance(block_types["Maybe"].match_tree, MatchBit)

    def test_either(self, block_types: dict[str, ResolvedType]):
        assert isinstance(block_types["Either"].match_tree, MatchBit)

    def test_hashmape(self, block_types: dict[str, ResolvedType]):
        assert isinstance(block_types["HashmapE"].match_tree, MatchBit)

    def test_unary(self, block_types: dict[str, ResolvedType]):
        assert isinstance(block_types["Unary"].match_tree, MatchBit)

    def test_hashmapnode(self, block_types: dict[str, ResolvedType]):
        assert isinstance(block_types["HashmapNode"].match_tree, MatchConstraint)

    def test_commonmsginfo(self, block_types: dict[str, ResolvedType]):
        assert isinstance(block_types["CommonMsgInfo"].match_tree, MatchBit)

    def test_msgaddress(self, block_types: dict[str, ResolvedType]):
        tree = block_types["MsgAddress"].match_tree
        assert tree is not None
        assert not isinstance(tree, MatchFail)

    def test_hmlabel(self, block_types: dict[str, ResolvedType]):
        assert isinstance(block_types["HmLabel"].match_tree, MatchBit)
