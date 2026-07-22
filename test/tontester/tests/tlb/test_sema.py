import pytest
from tlb.generator.ast_nodes import CompareOp
from tlb.generator.sema import analyze_text
from tlb.generator.sema.types import (
    BindOutputParam,
    BindParam,
    CellRefType,
    CheckConstraint,
    MatchBit,
    MatchConstraint,
    MatchConstructor,
    MatchFail,
    MatchTag,
    Module,
    NatAdd,
    NatFieldValue,
    NatGetBit,
    NatLiteral,
    NatMul,
    NatParamDef,
    NatParamRef,
    NatSub,
    NatTypeArg,
    ParamKind,
    ReadField,
    ResolvedType,
    SemaError,
    SolveConstraint,
    TupleType,
    TypeApply,
    TypeParamRef,
)

_TEST_MODULE = Module("<test>")


def types_by_name(text: str) -> dict[str, ResolvedType]:
    user_types = analyze_text(text, current_module=_TEST_MODULE).types
    return {t.name: t for t in user_types}


# ── Basic type registration ──────────────────────────────────────────


class TestTypeRegistration:
    def test_simple_type(self):
        ts = types_by_name("unit$_ = Unit;")
        assert "Unit" in ts
        assert ts["Unit"].arity == 0
        assert len(ts["Unit"].constructors) == 1
        assert ts["Unit"].constructors[0].name == "unit"

    def test_multi_constructor(self):
        ts = types_by_name("bool_false$0 = Bool; bool_true$1 = Bool;")
        assert ts["Bool"].arity == 0
        assert len(ts["Bool"].constructors) == 2

    def test_parameterized_type(self):
        ts = types_by_name("nothing$0 {X:Type} = Maybe X;\njust$1 {X:Type} value:X = Maybe X;")
        assert ts["Maybe"].arity == 1
        assert [p.kind for p in ts["Maybe"].type_level_params] == [ParamKind.TYPE]

    def test_nat_param(self):
        ts = types_by_name("foo$_ {n:#} x:(## n) = Foo n;")
        assert ts["Foo"].arity == 1
        assert [p.kind for p in ts["Foo"].type_level_params] == [ParamKind.NAT]

    def test_output_params(self):
        ts = types_by_name(
            "unary_zero$0 = Unary ~0; unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);"
        )
        assert [p for p in ts["Unary"].type_level_params if p.is_output][0].position == 0


# ── Expression resolution ────────────────────────────────────────────


class TestExpressionResolution:
    def test_type_apply(self):
        ts = types_by_name("""
            nothing$0 {X:Type} = Maybe X;
            just$1 {X:Type} value:X = Maybe X;
            foo$_ x:(Maybe uint32) = Foo;
        """)
        foo_cons = ts["Foo"].constructors[0]
        field_type = foo_cons.fields[0].type_expr
        assert isinstance(field_type, TypeApply)
        assert field_type.type is ts["Maybe"]
        assert len(field_type.arguments) == 1

    def test_cell_ref(self):
        """^Cell resolves to TypeApply(CellRef_type) — opaque cell reference builtin."""
        ts = types_by_name("foo$_ x:^Cell = Foo;")
        field_type = ts["Foo"].constructors[0].fields[0].type_expr
        assert isinstance(field_type, TypeApply)
        assert field_type.type.name == "^Cell"
        assert field_type.arguments == []

    def test_cell_ref_on_nat_producing_type(self):
        """^uint32 is valid — cell reference to a nat-valued type."""
        ts = types_by_name("foo$_ x:^uint32 = Foo;")
        field_type = ts["Foo"].constructors[0].fields[0].type_expr
        assert isinstance(field_type, CellRefType)
        assert isinstance(field_type.inner, TypeApply)

    def test_conditional(self):
        ts = types_by_name("foo$_ {n:#} x:n?uint32 = Foo n;")
        con = ts["Foo"].constructors[0]
        n_param = con.params[0]
        assert isinstance(n_param, NatParamDef)
        field = con.fields[0]
        assert field.condition == NatParamRef(n_param)
        assert isinstance(field.type_expr, TypeApply)

    def test_conditional_not_nested(self):
        """Conditional can't appear nested in a type expression."""
        with pytest.raises(SemaError, match="conditional.*field level"):
            _ = analyze_text(
                "nothing$0 {X:Type} = Maybe X;\n"
                + "just$1 {X:Type} value:X = Maybe X;\n"
                + "foo$_ {n:#} x:(Maybe n?uint32) = Foo n;",
                current_module=_TEST_MODULE,
            )

    def test_unconditional_field_has_none_condition(self):
        """Regular fields have condition=None."""
        ts = types_by_name("foo$_ x:uint32 = Foo;")
        assert ts["Foo"].constructors[0].fields[0].condition is None

    def test_tuple_nat_times_type(self):
        """n * Bit is a tuple: n values of Bit."""
        ts = types_by_name("bit$_ (## 1) = Bit; foo$_ {n:#} s:(n * Bit) = Foo n;")
        con = ts["Foo"].constructors[0]
        n_param = con.params[0]
        assert isinstance(n_param, NatParamDef)
        field_type = con.fields[0].type_expr
        assert isinstance(field_type, TupleType)
        assert field_type.count == NatParamRef(n_param)
        assert isinstance(field_type.element, TypeApply)
        assert field_type.element.type is ts["Bit"]

    def test_tuple_nat_times_nat_is_natmul(self):
        """n * m where both are nat → NatMul, not TupleType."""
        ts = types_by_name("foo$_ {n:#} {m:#} x:(## (n * m)) = Foo n m;")
        con = ts["Foo"].constructors[0]
        field_type = con.fields[0].type_expr
        # (## (n * m)): ## applied to (n * m) which is NatMul
        assert isinstance(field_type, TypeApply)
        assert len(field_type.arguments) == 1
        assert isinstance(field_type.arguments[0], NatMul)

    def test_tuple_type_times_type_error(self):
        """Type * Type is an error."""
        with pytest.raises(SemaError, match="nat expression"):
            _ = analyze_text(
                "foo$_ x:(Bit * Bit) = Foo; bit$_ (## 1) = Bit;", current_module=_TEST_MODULE
            )

    def test_getbit(self):
        """flags.3 is a NatGetBit expression."""
        ts = types_by_name("foo$_ flags:(## 8) x:flags.3?uint32 = Foo;")
        con = ts["Foo"].constructors[0]
        field = con.fields[1]
        assert isinstance(field.condition, NatGetBit)
        assert isinstance(field.type_expr, TypeApply)

    def test_nat_valued_field(self):
        ts = types_by_name("foo$_ {n:#} len:(#< n) = Foo n;")
        field = ts["Foo"].constructors[0].fields[0]
        assert field.is_nat_valued is True

    def test_type_param_ref(self):
        ts = types_by_name("just$1 {X:Type} value:X = Maybe X;")
        field_type = ts["Maybe"].constructors[0].fields[0].type_expr
        assert isinstance(field_type, TypeParamRef)

    def test_undefined_type_error(self):
        with pytest.raises(SemaError, match="undefined"):
            _ = analyze_text("foo$_ x:Nonexistent = Foo;", current_module=_TEST_MODULE)

    def test_undefined_lowercase_error(self):
        with pytest.raises(SemaError, match="undefined"):
            _ = analyze_text("foo$_ {n:#} x:(## unknown) = Foo n;", current_module=_TEST_MODULE)

    def test_empty_schema(self):
        user_types = analyze_text("", current_module=_TEST_MODULE).types
        assert user_types == []


# ── Tag computation ──────────────────────────────────────────────────


class TestTags:
    def test_explicit_tag(self):
        ts = types_by_name("bool_false$0 = Bool; bool_true$1 = Bool;")
        cons = ts["Bool"].constructors
        assert cons[0].tag_bits == "0"
        assert cons[1].tag_bits == "1"

    def test_auto_tag_is_32_bits(self):
        ts = types_by_name("foo x:uint32 = Foo;")
        assert ts["Foo"].constructors[0].tag_len == 32

    def test_empty_tag(self):
        ts = types_by_name("unit$_ = Unit;")
        assert ts["Unit"].constructors[0].tag_bits == ""
        assert ts["Unit"].constructors[0].tag_len == 0

    def test_auto_tag_matches_cpp_reference(self):
        """CRC32 auto-tags match the C++ tlbc reference implementation (block-auto.cpp)."""
        with open("/home/danklishch/code/ton/src/crypto/block/block.tlb") as f:
            types = analyze_text(f.read(), current_module=_TEST_MODULE).types
        by_name = {t.name: t for t in types}

        # Values from block-auto.cpp check_tag methods
        checks = [
            ("BlockExtra", "block_extra", 0x4A33F6FD, 32),
            ("McBlockExtra", "masterchain_block_extra", 0xCCA5, 16),
            ("ComplaintDescr", "no_blk_gen", 0x450E8BD9, 32),
            ("ComplaintDescr", "no_blk_gen_diff", 0xC737B0CA, 32),
        ]
        for type_name, cons_name, expected_tag, expected_len in checks:
            con = next(c for c in by_name[type_name].constructors if c.name == cons_name)
            assert con.tag_len == expected_len, f"{cons_name}: tag_len"
            assert int(con.tag_bits, 2) == expected_tag, (
                f"{cons_name}: expected 0x{expected_tag:08x}, got 0x{int(con.tag_bits, 2):08x}"
            )


# ── Match tree ────────────────────────────────────────────────────────


class TestMatchTree:
    def test_single_constructor_no_tag(self):
        ts = types_by_name("unit$_ = Unit;")
        tree = ts["Unit"].match_tree
        assert isinstance(tree, MatchConstructor)
        assert tree.constructor.name == "unit"

    def test_single_constructor_with_tag(self):
        ts = types_by_name("foo#4 = Foo;")
        tree = ts["Foo"].match_tree
        assert isinstance(tree, MatchTag)
        assert tree.bits == "0100"
        assert isinstance(tree.child, MatchConstructor)

    def test_two_constructors_bit_split(self):
        ts = types_by_name("bool_false$0 = Bool; bool_true$1 = Bool;")
        tree = ts["Bool"].match_tree
        assert isinstance(tree, MatchBit)
        assert isinstance(tree.zero, MatchConstructor)
        assert isinstance(tree.one, MatchConstructor)
        assert tree.zero.constructor.name == "bool_false"
        assert tree.one.constructor.name == "bool_true"

    def test_shared_tag_param_disambiguation(self):
        """HashmapNode: both constructors have empty tags, disambiguated by params."""
        ts = types_by_name(
            """
dummy$_ {n:#} {X:Type} = Hashmap n X;
hmn_leaf#_ {X:Type} value:X = HashmapNode 0 X;
hmn_fork#_ {n:#} {X:Type} left:^(Hashmap n X) right:^(Hashmap n X) = HashmapNode (n + 1) X;
"""
        )
        tree = ts["HashmapNode"].match_tree
        # Should be a constraint-based split on arg_0
        assert isinstance(tree, MatchConstraint)
        assert isinstance(tree.condition, CheckConstraint)

    def test_typedef_follows_through(self):
        """Typedef constructor follows through to the inner type for dispatch."""
        ts = types_by_name(
            """
inner#ab x:uint32 = Inner;
other#cd y:uint64 = Other;
_ Inner = Outer;
_ Other = Outer;
"""
        )
        tree = ts["Outer"].match_tree
        # Should split on bits since Inner (#ab) and Other (#cd) have different tags
        assert tree is not None
        assert not isinstance(tree, MatchFail)

    def test_multi_constant_disambiguation(self):
        """ConfigParam-like: many constructors with different constant params."""
        ts = types_by_name(
            """
_ x:uint32 = P 0;
_ x:uint64 = P 1;
_ x:bits256 = P 2;
"""
        )
        tree = ts["P"].match_tree
        # Should build a constraint tree splitting on arg_0
        assert tree is not None
        assert not isinstance(tree, MatchFail)

    def test_empty_type(self):
        """Type with no constructors."""
        ts = types_by_name("foo$_ = Foo; bar$_ = Bar;")
        # Both are fine — just single constructors
        assert isinstance(ts["Foo"].match_tree, MatchConstructor)

    def test_three_way_bit_split(self):
        """Three constructors with different bit prefixes."""
        ts = types_by_name(
            """
a$00 = T;
b$01 = T;
c$1 = T;
"""
        )
        tree = ts["T"].match_tree
        assert isinstance(tree, MatchBit)
        # bit 0: split further into 00 vs 01
        assert isinstance(tree.zero, MatchBit)
        # bit 1: single constructor
        assert isinstance(tree.one, MatchConstructor)
        assert tree.one.constructor.name == "c"


# ── Inference capability ─────────────────────────────────────────────


class TestInference:
    def test_pair_is_inference_capable(self):
        ts = types_by_name("pair$_ {X:Type} {Y:Type} first:X second:Y = Pair X Y;")
        pair = ts["Pair"]
        assert len(pair.inference) == 2
        assert pair.inference[0].is_capable is True
        assert pair.inference[1].is_capable is True

    def test_maybe_not_inference_capable(self):
        ts = types_by_name("nothing$0 {X:Type} = Maybe X;\njust$1 {X:Type} value:X = Maybe X;")
        maybe = ts["Maybe"]
        assert len(maybe.inference) == 1
        assert maybe.inference[0].is_capable is False


# ── Classification ───────────────────────────────────────────────────


class TestClassification:
    def test_enum(self):
        ts = types_by_name("bool_false$0 = Bool; bool_true$1 = Bool;")
        assert ts["Bool"].is_enum is True

    def test_not_enum(self):
        ts = types_by_name("foo$_ x:uint32 = Foo;")
        assert ts["Foo"].is_enum is False

    def test_typedef(self):
        ts = types_by_name("_ uint32 = Coins;")
        assert ts["Coins"].is_typedef is True
        assert isinstance(ts["Coins"].typedef_target, TypeApply)

    def test_not_typedef_with_tag(self):
        ts = types_by_name("foo x:uint32 = Foo;")
        # Has auto-tag, so not a typedef
        assert ts["Foo"].is_typedef is False


# ── Deser plan ───────────────────────────────────────────────────────


class TestDeserPlan:
    def test_basic_fields(self):
        ts = types_by_name("foo$_ a:uint32 b:uint64 = Foo;")
        steps = ts["Foo"].constructors[0].deser_steps
        reads = [s for s in steps if isinstance(s, ReadField)]
        assert len(reads) == 2

    def test_entry_bindings(self):
        """Nat params are bound from type args via SolveConstraint(identity)."""
        ts = types_by_name("foo$_ {n:#} x:(## n) = Foo n;")
        steps = ts["Foo"].constructors[0].deser_steps
        # First step should bind n from NatTypeArg(0)
        assert isinstance(steps[0], SolveConstraint)
        assert steps[0].target_param.name == "n"
        assert isinstance(steps[0].value, NatTypeArg)
        assert steps[0].value.param.position == 0

    def test_type_param_binding(self):
        """Type params are bound via BindParam."""
        ts = types_by_name("just$1 {X:Type} value:X = Maybe X;")
        steps = ts["Maybe"].constructors[0].deser_steps
        binds = [s for s in steps if isinstance(s, BindParam)]
        assert len(binds) == 1
        assert binds[0].target_param.name == "X"
        assert binds[0].position == 0

    def test_complex_result_param(self):
        """Complex result param (n + 1) derives n from type arg."""
        ts = types_by_name(
            """
dummy$_ {n:#} {X:Type} = Hashmap n X;
hmn_leaf#_ {X:Type} value:X = HashmapNode 0 X;
hmn_fork#_ {n:#} {X:Type} left:^(Hashmap n X) right:^(Hashmap n X) = HashmapNode (n + 1) X;
"""
        )
        fork = ts["HashmapNode"].constructors[1]
        steps = fork.deser_steps

        # n should be solved from type_arg_0: n = NatTypeArg(0) - 1
        solves = [s for s in steps if isinstance(s, SolveConstraint)]
        n_solve = [s for s in solves if s.target_param.name == "n"]
        assert len(n_solve) == 1
        assert isinstance(n_solve[0].value, NatSub)
        assert isinstance(n_solve[0].value.left, NatTypeArg)
        assert n_solve[0].value.left.param.position == 0

    def test_constant_result_param(self):
        """Constant result param (0) becomes a CheckConstraint."""
        ts = types_by_name(
            """
dummy$_ {n:#} {X:Type} = Hashmap n X;
hmn_leaf#_ {X:Type} value:X = HashmapNode 0 X;
hmn_fork#_ {n:#} {X:Type} left:^(Hashmap n X) right:^(Hashmap n X) = HashmapNode (n + 1) X;
"""
        )
        leaf = ts["HashmapNode"].constructors[0]
        steps = leaf.deser_steps
        checks = [s for s in steps if isinstance(s, CheckConstraint)]
        # Should check NatTypeArg(0) == 0
        assert len(checks) >= 1
        c = checks[0]
        assert isinstance(c.left, NatTypeArg)
        assert isinstance(c.right, NatLiteral)
        assert c.right.value == 0

    def test_constraint_check(self):
        """Non-negated constraint becomes a CheckConstraint."""
        ts = types_by_name("foo$_ {n:#} {n <= 30} x:(## n) = Foo n;")
        steps = ts["Foo"].constructors[0].deser_steps
        checks = [s for s in steps if isinstance(s, CheckConstraint)]
        assert len(checks) >= 1

    def test_constraint_solve(self):
        """Constraint with ~var becomes SolveConstraint."""
        ts = types_by_name(
            """
unary_zero$0 = Unary ~0;
unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);
hm_edge#_ {n:#} {X:Type} {l:#} {m:#} label:(HmLabel ~l n) {n = (~m) + l} node:(HashmapNode m X) = Hashmap n X;
hmn_leaf#_ {X:Type} value:X = HashmapNode 0 X;
hmn_fork#_ {n:#} {X:Type} left:^(Hashmap n X) right:^(Hashmap n X) = HashmapNode (n + 1) X;
hml_short$0 {m:#} {n:#} len:(Unary ~n) {n <= m} s:(n * Bit) = HmLabel ~n m;
hml_long$10 {m:#} n:(#<= m) s:(n * Bit) = HmLabel ~n m;
hml_same$11 {m:#} v:Bit n:(#<= m) = HmLabel ~n m;
bit$_ (## 1) = Bit;
"""
        )
        hm_edge = ts["Hashmap"].constructors[0]
        steps = hm_edge.deser_steps

        # Entry: bind n and X from type args
        # Then: ReadField(label), BindOutputParam(l), SolveConstraint(m), ReadField(node)
        reads = [s for s in steps if isinstance(s, ReadField)]
        assert reads[0].field.name == "label"
        assert reads[1].field.name == "node"

        binds = [s for s in steps if isinstance(s, BindOutputParam)]
        assert any(b.target_param.name == "l" for b in binds)

        solves = [s for s in steps if isinstance(s, SolveConstraint)]
        m_solve = [s for s in solves if s.target_param.name == "m"]
        assert len(m_solve) == 1
        assert isinstance(m_solve[0].value, NatSub)

    def test_output_param_binding(self):
        """Output params are extracted from fields with ~ arguments."""
        ts = types_by_name(
            """
unary_zero$0 = Unary ~0;
unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);
hml_short$0 {m:#} {n:#} len:(Unary ~n) {n <= m} s:(n * Bit) = HmLabel ~n m;
bit$_ (## 1) = Bit;
"""
        )
        hml_short = ts["HmLabel"].constructors[0]
        steps = hml_short.deser_steps

        binds = [s for s in steps if isinstance(s, BindOutputParam)]
        assert any(
            b.target_param.name == "n" and b.extraction.result_param_position == 0 for b in binds
        )

    def test_nat_param_values(self):
        """Nat param values are resolved nat expressions indexed by TLP position."""
        ts = types_by_name(
            "unary_zero$0 = Unary ~0;\nunary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);\n"
        )
        zero = ts["Unary"].constructors[0]
        assert len(zero.nat_param_values) == 1
        assert isinstance(zero.nat_param_values[0], NatLiteral)
        assert zero.nat_param_values[0].value == 0

        succ = ts["Unary"].constructors[1]
        assert len(succ.nat_param_values) == 1
        assert isinstance(succ.nat_param_values[0], NatAdd)

    def test_inference_through_generic(self):
        """Output param inferred through an inference-capable generic type."""
        ts = types_by_name(
            """
unary_zero$0 = Unary ~0;
unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);
pair$_ {X:Type} {Y:Type} first:X second:Y = Pair X Y;
foo$_ {n:#} x:(Pair (Unary ~n) uint32) y:(## n) = Foo ~n;
"""
        )
        foo_con = ts["Foo"].constructors[0]
        binds = [s for s in foo_con.deser_steps if isinstance(s, BindOutputParam)]
        assert len(binds) == 1
        assert binds[0].target_param.name == "n"
        assert len(binds[0].extraction.chain) == 1
        assert binds[0].extraction.chain[0].type is ts["Pair"]
        assert binds[0].extraction.result_param_position == 0

    def test_inference_double_nesting(self):
        """Output param inferred through two levels of generic types."""
        ts = types_by_name(
            """
unary_zero$0 = Unary ~0;
unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);
pair$_ {X:Type} {Y:Type} first:X second:Y = Pair X Y;
bar$_ {n:#} x:(Pair (Pair (Unary ~n) uint32) uint64) y:(## n) = Bar ~n;
"""
        )
        bar_con = ts["Bar"].constructors[0]
        binds = [s for s in bar_con.deser_steps if isinstance(s, BindOutputParam)]
        assert len(binds) == 1
        assert binds[0].target_param.name == "n"
        assert len(binds[0].extraction.chain) == 2
        assert binds[0].extraction.result_param_position == 0

    def test_no_inference_through_maybe(self):
        """Maybe is NOT inference-capable, so using it to infer an output param is an error."""
        with pytest.raises(SemaError, match="cannot be computed"):
            _ = analyze_text(
                """
nothing$0 {X:Type} = Maybe X;
just$1 {X:Type} value:X = Maybe X;
unary_zero$0 = Unary ~0;
unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);
baz$_ {n:#} x:(Maybe (Unary ~n)) = Baz ~n;
""",
                current_module=_TEST_MODULE,
            )

    def test_output_consistency_error(self):
        """All constructors must agree on which positions are output (~)."""
        with pytest.raises(SemaError, match="negated"):
            _ = analyze_text(
                """
foo$0 {n:#} x:uint32 = T ~n;
bar$1 {n:#} x:uint32 = T n;
""",
                current_module=_TEST_MODULE,
            )

    def test_nat_less_nonconst_implicit_constraint(self):
        """#< n with non-constant n generates an implicit n > 0 constraint."""
        types = analyze_text("foo$_ {n:#} x:(#< n) = Foo n;", current_module=_TEST_MODULE).types
        con = types[0].constructors[0]
        n_param = con.params[0]
        assert isinstance(n_param, NatParamDef)
        assert con.deser_steps == [
            SolveConstraint(
                target_param=n_param,
                value=NatTypeArg(param=types[0].type_level_params[0]),
            ),
            CheckConstraint(
                op=CompareOp.GT,
                left=NatParamRef(n_param),
                right=NatLiteral(0),
            ),
            ReadField(field=con.fields[0]),
        ]

    def test_nat_less_nonconst_nested_in_generic(self):
        """#< n nested inside a generic type still gets the implicit n > 0 constraint."""
        types = analyze_text(
            "nothing$0 {X:Type} = Maybe X;\n"
            + "just$1 {X:Type} value:X = Maybe X;\n"
            + "foo$_ {n:#} x:(Maybe (#< n)) = Foo n;",
            current_module=_TEST_MODULE,
        ).types
        foo_type = next(t for t in types if t.name == "Foo")
        con = foo_type.constructors[0]
        n_param = con.params[0]
        assert isinstance(n_param, NatParamDef)
        check_steps = [
            s for s in con.deser_steps if isinstance(s, CheckConstraint) and s.op == CompareOp.GT
        ]
        assert len(check_steps) == 1
        assert check_steps[0].left == NatParamRef(n_param)
        assert check_steps[0].right == NatLiteral(0)

    def test_field_constraint_leq(self):
        """Constraint { flags <= 1 } on an explicit field becomes a CheckConstraint."""
        ts = types_by_name("foo$_ flags:(## 8) { flags <= 1 } x:uint32 = Foo;")
        con = ts["Foo"].constructors[0]
        f_flags = con.fields[0]
        f_x = con.fields[1]
        assert con.deser_steps == [
            ReadField(field=f_flags),
            CheckConstraint(op=CompareOp.LE, left=NatFieldValue(f_flags), right=NatLiteral(1)),
            ReadField(field=f_x),
        ]

    def test_field_constraint_geq(self):
        """Constraint { vert_seq_no >= vert_seqno_incr } on fields."""
        ts = types_by_name(
            "foo$_ vert_seqno_incr:(## 1) seq_no:# vert_seq_no:# "
            + "{ vert_seq_no >= vert_seqno_incr } = Foo;"
        )
        con = ts["Foo"].constructors[0]
        f_incr = con.fields[0]
        f_seq = con.fields[1]
        f_vert = con.fields[2]
        assert con.deser_steps == [
            ReadField(field=f_incr),
            ReadField(field=f_seq),
            ReadField(field=f_vert),
            CheckConstraint(
                op=CompareOp.GE, left=NatFieldValue(f_vert), right=NatFieldValue(f_incr)
            ),
        ]

    def test_negated_param_solved_from_constraint(self):
        """{ ~prev_seq_no + 1 = seq_no } solves prev_seq_no = seq_no - 1."""
        ts = types_by_name("foo$_ seq_no:# { prev_seq_no:# } { ~prev_seq_no + 1 = seq_no } = Foo;")
        con = ts["Foo"].constructors[0]
        f_seq = con.fields[0]
        p_prev = con.params[0]
        assert isinstance(p_prev, NatParamDef)
        assert con.deser_steps == [
            ReadField(field=f_seq),
            SolveConstraint(
                target_param=p_prev,
                value=NatSub(left=NatFieldValue(f_seq), right=NatLiteral(1)),
            ),
        ]


# ── Multiple types interaction ────────────────────────────────────────


class TestMultipleTypes:
    def test_cross_type_references(self):
        ts = types_by_name(
            """
nothing$0 {X:Type} = Maybe X;
just$1 {X:Type} value:X = Maybe X;
foo$_ x:(Maybe uint32) = Foo;"""
        )
        field_type = ts["Foo"].constructors[0].fields[0].type_expr
        assert isinstance(field_type, TypeApply)
        assert field_type.type is ts["Maybe"]

    def test_either_type(self):
        ts = types_by_name(
            """
left$0 {X:Type} {Y:Type} value:X = Either X Y;
right$1 {X:Type} {Y:Type} value:Y = Either X Y;
"""
        )
        assert ts["Either"].arity == 2
        assert [p.kind for p in ts["Either"].type_level_params] == [ParamKind.TYPE, ParamKind.TYPE]
        # Neither X nor Y should be inference-capable
        assert len(ts["Either"].inference) == 2
        assert ts["Either"].inference[0].is_capable is False
        assert ts["Either"].inference[1].is_capable is False

    def test_shared_tag_constructors(self):
        """Types where constructors share empty tags (disambiguated by params)."""
        ts = types_by_name(
            """
dummy$_ {n:#} {X:Type} = Hashmap n X;
hmn_leaf#_ {X:Type} value:X = HashmapNode 0 X;
hmn_fork#_ {n:#} {X:Type} left:^(Hashmap n X) right:^(Hashmap n X) = HashmapNode (n + 1) X;
"""
        )
        tree = ts["HashmapNode"].match_tree
        # Must use constraint-based disambiguation, not MatchFail
        assert tree is not None
        assert not isinstance(tree, MatchFail)


# ── Semantic errors ───────────────────────────────────────────────────


class TestSemaErrors:
    def test_inconsistent_arity(self):
        with pytest.raises(SemaError, match="inconsistent arity"):
            _ = analyze_text("foo$0 x:uint32 = T x; bar$1 = T;", current_module=_TEST_MODULE)

    def test_builtin_redefinition(self):
        with pytest.raises(SemaError, match="cannot redefine"):
            _ = analyze_text("foo$_ = Cell;", current_module=_TEST_MODULE)

    def test_duplicate_constructor_name(self):
        with pytest.raises(SemaError, match="duplicate constructor name"):
            _ = analyze_text("foo$0 = T; foo$1 = T;", current_module=_TEST_MODULE)

    def test_duplicate_param_name(self):
        with pytest.raises(SemaError, match="duplicate parameter name"):
            _ = analyze_text("foo$_ {n:#} {n:Type} = T;", current_module=_TEST_MODULE)

    def test_type_param_not_in_result(self):
        """Type param declared but not used in result params."""
        with pytest.raises(SemaError, match="not found in result params"):
            _ = analyze_text("foo$_ {X:Type} x:uint32 = T;", current_module=_TEST_MODULE)

    def test_duplicate_field_name(self):
        with pytest.raises(SemaError, match="duplicate field name"):
            _ = analyze_text("foo$_ a:uint32 a:uint64 = T;", current_module=_TEST_MODULE)

    def test_nat_less_zero(self):
        """#< 0 is invalid — no values exist below 0."""
        with pytest.raises(SemaError, match="#< 0 is invalid"):
            _ = analyze_text("foo$_ n:(#< 0) = T;", current_module=_TEST_MODULE)

    def test_nat_less_type_param(self):
        """#< requires a nat argument, not a type."""
        with pytest.raises(SemaError, match="expects a nat argument"):
            _ = analyze_text("foo$_ {T:Type} n:(#< T) = Foo T;", current_module=_TEST_MODULE)

    def test_nat_leq_type_param(self):
        """#<= requires a nat argument, not a type."""
        with pytest.raises(SemaError, match="expects a nat argument"):
            _ = analyze_text("foo$_ {T:Type} n:(#<= T) = Foo T;", current_module=_TEST_MODULE)

    def test_nat_less_type_param_nested(self):
        """#< with Type param nested inside a generic is still rejected."""
        with pytest.raises(SemaError, match="expects a nat argument"):
            _ = analyze_text(
                "nothing$0 {X:Type} = Maybe X;\n"
                + "just$1 {X:Type} value:X = Maybe X;\n"
                + "foo$_ {T:Type} x:(Maybe (#< T)) = Foo T;",
                current_module=_TEST_MODULE,
            )

    def test_wrong_arity(self):
        """Applying a type with wrong number of arguments."""
        with pytest.raises(SemaError, match="expects 1 arguments, got 2"):
            _ = analyze_text(
                """
                nothing$0 {X:Type} = Maybe X;
                just$1 {X:Type} value:X = Maybe X;
                foo$_ x:(Maybe uint32 uint64) = Foo;
            """,
                current_module=_TEST_MODULE,
            )

    def test_field_ref_in_input_result_param(self):
        """Non-output result params cannot reference explicit fields."""
        with pytest.raises(SemaError, match="cannot reference field"):
            _ = analyze_text("foo$_ x:uint32 = T x;", current_module=_TEST_MODULE)

    def test_output_value_can_reference_field(self):
        """Output values CAN reference nat-valued fields (like hml_long)."""
        ts = types_by_name("""
            foo$_ {m:#} n:(#<= m) = T ~n m;
        """)
        con = ts["T"].constructors[0]
        assert len(con.nat_param_values) == 2
        assert isinstance(con.nat_param_values[0], NatFieldValue)
        assert isinstance(con.nat_param_values[1], NatParamRef)

    def test_uncomputable_param(self):
        """Param that can't be bound from type args, fields, or constraints."""
        with pytest.raises(SemaError, match="cannot be computed"):
            _ = analyze_text("foo$_ {n:#} x:uint32 = T;", current_module=_TEST_MODULE)

    def test_output_consistency(self):
        """All constructors must agree on ~ positions."""
        with pytest.raises(SemaError, match="negated"):
            _ = analyze_text(
                """
                foo$0 {n:#} x:uint32 = T ~n;
                bar$1 {n:#} x:uint32 = T n;
            """,
                current_module=_TEST_MODULE,
            )

    def test_multiple_negated_in_constraint(self):
        """Constraint with two ~ params should error."""
        with pytest.raises(SemaError, match="multiple negated"):
            _ = analyze_text(
                """
                foo$_ {n:#} {m:#} {l:#} {(~n) = (~m) + l} x:uint32 = T;
            """,
                current_module=_TEST_MODULE,
            )

    def test_zero_arity_applied_with_args(self):
        """Applying a zero-arity type with arguments should error."""
        with pytest.raises(SemaError, match="expects 0 arguments"):
            _ = analyze_text(
                """
                foo$_ = Foo;
                bar$_ x:(Foo uint32) = Bar;
            """,
                current_module=_TEST_MODULE,
            )

    def test_special_consistency_error(self):
        """All constructors must agree on ! (special cell) prefix."""
        with pytest.raises(SemaError, match="special"):
            _ = analyze_text(
                """
                !merkle$0 x:uint32 = T;
                normal$1 y:uint64 = T;
            """,
                current_module=_TEST_MODULE,
            )

    def test_conditional_field_in_constraint(self):
        """Conditional field cannot be used in constraints."""
        with pytest.raises(SemaError, match="conditional field"):
            _ = analyze_text(
                "foo$_ flag:(## 1) n:flag?# { n = 1 } = Foo;", current_module=_TEST_MODULE
            )

    def test_conditional_field_in_type_arg(self):
        """Conditional field cannot be used as type argument."""
        with pytest.raises(SemaError, match="conditional field"):
            _ = analyze_text(
                "pair$_ {X:Type} {Y:Type} first:X second:Y = Pair X Y;\n"
                + "foo$_ flag:(## 1) n:flag?# inner:(Pair (## n) uint32) = Foo;",
                current_module=_TEST_MODULE,
            )

    def test_conditional_field_output_param(self):
        """Output param cannot be inferred from a conditional field."""
        with pytest.raises(SemaError, match="conditional field"):
            _ = analyze_text(
                "unary_zero$0 = Unary ~0;\n"
                + "unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);\n"
                + "bit$_ (## 1) = Bit;\n"
                + "foo$_ {n:#} flag:(## 1) x:flag?(Unary ~n) data:(n * Bit) = Foo ~n;",
                current_module=_TEST_MODULE,
            )

    def test_conditional_field_breaks_inference(self):
        """Inference through a type whose exposing field is conditional should fail."""
        with pytest.raises(SemaError, match="cannot be computed"):
            _ = analyze_text(
                "unary_zero$0 = Unary ~0;\n"
                + "unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);\n"
                + "generic_opt$_ {X:Type} has_x:(## 1) x:has_x?X = GenericOpt X;\n"
                + "bar$_ {n:#} inner:(GenericOpt (Unary ~n)) = Bar ~n;",
                current_module=_TEST_MODULE,
            )

    def test_special_all_marked(self):
        """All constructors with ! is fine."""
        ts = types_by_name("""
            !a$0 x:uint32 = T;
            !b$1 y:uint64 = T;
        """)
        assert ts["T"].is_special is True

    def test_normal_not_special(self):
        ts = types_by_name("foo$_ x:uint32 = T;")
        assert ts["T"].is_special is False

    def test_multiplicative_constraint_unsolvable(self):
        """Constraints with ~ inside multiplication can't be solved."""
        with pytest.raises(SemaError, match="cannot be computed"):
            _ = analyze_text(
                "foo$_ {n:#} {m:#} {n = (~m) * 2} x:uint32 = T n;",
                current_module=_TEST_MODULE,
            )

    def test_duplicate_output_param(self):
        """Same param at multiple output positions should error."""
        with pytest.raises(SemaError, match="multiple output"):
            _ = analyze_text(
                """
                pair$_ {X:Type} {Y:Type} first:X second:Y = Pair X Y;
                unary_zero$0 = Unary ~0;
                unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);
                infer_both$_ {n:#} {m:#} pair:(Pair (Unary ~n) (Unary ~m)) = InferBoth ~n ~m;
                foo$_ {n:#} x:(InferBoth ~n ~n) = Foo;
            """,
                current_module=_TEST_MODULE,
            )

    def test_duplicate_output_param_across_fields(self):
        """Same param bound from output positions in two separate fields should error."""
        with pytest.raises(SemaError, match="multiple output"):
            _ = analyze_text(
                """
                unary_zero$0 = Unary ~0;
                unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);
                foo$_ {n:#} x:(Unary ~n) y:(Unary ~n) = Foo;
            """,
                current_module=_TEST_MODULE,
            )


# ── Inline records ───────────────────────────────────────────────────


class TestInlineRecords:
    def test_inline_record_creates_anonymous_type(self):
        """Inline record [a:uint32 b:uint8] produces an anonymous ResolvedType."""
        types = analyze_text(
            "foo$_ inner:[a:uint32 b:uint8] = Foo;", current_module=_TEST_MODULE
        ).types
        # Should have Foo and an anonymous type
        anon_types = [t for t in types if t.name == ""]
        assert len(anon_types) == 1
        anon = anon_types[0]
        assert len(anon.constructors) == 1
        assert len(anon.constructors[0].fields) == 2
        assert anon.constructors[0].fields[0].name == "a"
        assert anon.constructors[0].fields[1].name == "b"

    def test_inline_record_has_match_tree(self):
        """Anonymous types go through match tree building."""
        types = analyze_text("foo$_ inner:[a:uint32] = Foo;", current_module=_TEST_MODULE).types
        anon = [t for t in types if t.name == ""][0]
        assert anon.match_tree is not None
        assert isinstance(anon.match_tree, MatchConstructor)

    def test_inline_record_has_deser_plan(self):
        """Anonymous types go through deser plan generation."""
        types = analyze_text(
            "foo$_ inner:[a:uint32 b:uint8] = Foo;", current_module=_TEST_MODULE
        ).types
        anon = [t for t in types if t.name == ""][0]
        steps = anon.constructors[0].deser_steps
        reads = [s for s in steps if isinstance(s, ReadField)]
        assert len(reads) == 2

    def test_inline_record_in_cell_ref(self):
        """^[x:uint32 y:uint32] creates a cell-referenced anonymous type."""
        types = analyze_text(
            "foo$_ inner:^[x:uint32 y:uint32] = Foo;", current_module=_TEST_MODULE
        ).types
        ts = {t.name: t for t in types}
        foo_field = ts["Foo"].constructors[0].fields[0]
        assert isinstance(foo_field.type_expr, CellRefType)

    def test_nested_inline_records(self):
        """Inline record can contain another inline record."""
        types = analyze_text(
            "foo$_ inner:[a:uint32 b:[x:uint8 y:uint8]] = Foo;", current_module=_TEST_MODULE
        ).types
        anon_types = [t for t in types if t.name == ""]
        assert len(anon_types) == 2  # outer and inner

    def test_inline_record_scope_isolation(self):
        """Type params from outer scope don't leak into inline records."""
        with pytest.raises(SemaError, match="undefined"):
            _ = analyze_text("foo$_ {T:Type} inner:[a:T] = Foo T;", current_module=_TEST_MODULE)

    def test_inline_record_with_own_type_param(self):
        """Inline record with its own type param, applied to outer T."""
        types = analyze_text(
            "foo$_ {T:Type} inner:([{X:Type} a:X] T) = Foo T;", current_module=_TEST_MODULE
        ).types
        anon = [t for t in types if t.name == ""][0]
        assert anon.arity == 1
        assert [p.kind for p in anon.type_level_params] == [ParamKind.TYPE]

    def test_unbound_type_var_in_inline_record(self):
        """Referencing undefined type inside inline record is an error."""
        with pytest.raises(SemaError, match="undefined"):
            _ = analyze_text("foo$_ inner:[a:T] = Foo;", current_module=_TEST_MODULE)

    def test_inline_record_nat_param_is_internal(self):
        """Nat params in inline records are internal (arity=0), resolved by deser plan."""
        types = analyze_text(
            "unary_zero$0 = Unary ~0;\n"
            + "unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);\n"
            + "bit$_ (## 1) = Bit;\n"
            + "foo$_ x:[{n:#} len:(Unary ~n) data:(n * Bit)] = Foo;",
            current_module=_TEST_MODULE,
        ).types
        anon = [t for t in types if t.name == ""][0]
        assert anon.arity == 0  # nat params don't contribute to arity
        assert anon.constructors[0].params[0].name == "n"
        # n is bound via BindOutputParam in the deser plan
        binds = [s for s in anon.constructors[0].deser_steps if isinstance(s, BindOutputParam)]
        assert len(binds) == 1
        assert binds[0].target_param.name == "n"

    def test_inline_record_mixed_params(self):
        """Inline record with both type param (external) and nat param (internal)."""
        types = analyze_text(
            "unary_zero$0 = Unary ~0;\n"
            + "unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);\n"
            + "foo$_ {T:Type} x:([{X:Type} {n:#} a:X len:(Unary ~n)] T) = Foo T;",
            current_module=_TEST_MODULE,
        ).types
        anon = [t for t in types if t.name == ""][0]
        assert anon.arity == 1  # only type param X contributes
        assert [p.kind for p in anon.type_level_params] == [ParamKind.TYPE]


# ── Full block.tlb ───────────────────────────────────────────────────


class TestImports:
    def test_no_imports_origin_module_set(self):
        m = Module("foo")
        types = analyze_text("foo$_ = Foo;", current_module=m).types
        foo = next(t for t in types if t.name == "Foo")
        assert foo.origin_module == m

    def test_anonymous_types_carry_origin_module(self):
        m = Module("inline")
        types = analyze_text("foo$_ x:[a:uint32] = Foo;", current_module=m).types
        anon = next(t for t in types if t.name == "")
        assert anon.origin_module == m

    def test_import_makes_type_visible(self):
        block_mod = Module("block")
        block = analyze_text("currency$_ amount:uint64 = Currency;", current_module=block_mod)
        types = analyze_text(
            "//@import block.tlb\nwallet$_ bal:Currency = Wallet;",
            current_module=Module("user"),
            imports={"block.tlb": block},
        ).types
        wallet = next(t for t in types if t.name == "Wallet")
        field_type = wallet.constructors[0].fields[0].type_expr
        assert isinstance(field_type, TypeApply)
        assert field_type.type.name == "Currency"
        assert field_type.type.origin_module == block_mod
        # Imported types are not in the consuming module's type list.
        assert "Currency" not in {t.name for t in types}

    def test_unresolved_import(self):
        with pytest.raises(SemaError, match="unresolved import"):
            _ = analyze_text(
                "//@import missing.tlb\nfoo$_ = Foo;",
                current_module=Module("user"),
                imports={},
            )

    def test_self_import_rejected(self):
        block_mod = Module("block")
        block = analyze_text("foo$_ = Foo;", current_module=block_mod)
        with pytest.raises(SemaError, match="cannot import itself"):
            _ = analyze_text(
                "//@import self.tlb\nbar$_ = Bar;",
                current_module=block_mod,
                imports={"self.tlb": block},
            )

    def test_local_shadows_import_silently(self):
        block_mod = Module("block")
        block = analyze_text("currency$_ amount:uint64 = Currency;", current_module=block_mod)
        user_mod = Module("user")
        types = analyze_text(
            "//@import block.tlb\ncurrency$_ x:uint32 = Currency;",
            current_module=user_mod,
            imports={"block.tlb": block},
        ).types
        currency = next(t for t in types if t.name == "Currency")
        assert currency.origin_module == user_mod
        field_type = currency.constructors[0].fields[0].type_expr
        assert isinstance(field_type, TypeApply)
        assert field_type.type.name == "uint32"

    def test_ambiguous_import_without_local_shadow(self):
        a_mod, b_mod = Module("a"), Module("b")
        a = analyze_text("foo$_ = Foo;", current_module=a_mod)
        b = analyze_text("foo$_ = Foo;", current_module=b_mod)
        with pytest.raises(SemaError, match="ambiguous import"):
            _ = analyze_text(
                "//@import a.tlb\n//@import b.tlb\nbar$_ = Bar;",
                current_module=Module("user"),
                imports={"a.tlb": a, "b.tlb": b},
            )

    def test_ambiguous_import_resolved_by_local_shadow(self):
        a_mod, b_mod = Module("a"), Module("b")
        a = analyze_text("foo$_ x:uint8 = Foo;", current_module=a_mod)
        b = analyze_text("foo$_ y:uint16 = Foo;", current_module=b_mod)
        user_mod = Module("user")
        types = analyze_text(
            "//@import a.tlb\n//@import b.tlb\nfoo$_ z:uint32 = Foo;",
            current_module=user_mod,
            imports={"a.tlb": a, "b.tlb": b},
        ).types
        foo = next(t for t in types if t.name == "Foo")
        assert foo.origin_module == user_mod

    def test_no_transitive_reexport(self):
        base_mod, mid_mod = Module("base"), Module("mid")
        base = analyze_text("bar$_ = Bar;", current_module=base_mod)
        mid = analyze_text(
            "//@import base.tlb\nmid$_ b:Bar = Mid;",
            current_module=mid_mod,
            imports={"base.tlb": base},
        )
        # Importing only mid does NOT make Bar visible.
        with pytest.raises(SemaError, match="undefined type"):
            _ = analyze_text(
                "//@import mid.tlb\nu$_ b:Bar = U;",
                current_module=Module("user"),
                imports={"mid.tlb": mid},
            )

    def test_idempotent_double_import(self):
        # The same AnalyzedModule reachable under two import paths is fine.
        block_mod = Module("block")
        block = analyze_text("foo$_ = Foo;", current_module=block_mod)
        _ = analyze_text(
            "//@import block.tlb\n//@import alias.tlb\nbar$_ x:Foo = Bar;",
            current_module=Module("user"),
            imports={"block.tlb": block, "alias.tlb": block},
        )


class TestBlockTlb:
    def test_analyze_block_tlb(self):
        with open("crypto/block/block.tlb") as f:
            text = f.read()
        user_types = analyze_text(text, current_module=_TEST_MODULE).types
        type_names = {t.name for t in user_types}
        assert "Hashmap" in type_names
        assert "HashmapE" in type_names
        assert "Message" in type_names
        assert "Block" in type_names
        assert len(user_types) > 100
