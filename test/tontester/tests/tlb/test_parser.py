import pytest
from tlb.generator.ast_nodes import (
    Add,
    Apply,
    CellRef,
    Compare,
    CompareOp,
    Conditional,
    Constraint,
    Constructor,
    ExplicitField,
    GetBit,
    Identifier,
    ImplicitParam,
    Import,
    InlineRecord,
    IntConst,
    Multiply,
    NegatedIdentifier,
    ResultParam,
    Schema,
    Tag,
)
from tlb.generator.lexer import Lexer
from tlb.generator.parser import ParseError, Parser


def parse(text: str) -> Schema:
    tokens = Lexer(text).tokenize()
    return Parser(tokens).parse()


def parse_one(text: str) -> Constructor:
    schema = parse(text)
    assert len(schema.constructors) == 1
    return schema.constructors[0]


# ── Constructor head ──────────────────────────────────────────────────


class TestConstructorHead:
    def test_named_no_tag(self):
        c = parse_one("foo x:uint32 = Foo;")
        assert c.name == "foo"
        assert c.tag == Tag(is_auto=True)

    def test_named_hex_tag(self):
        c = parse_one("foo#4 = Foo;")
        assert c.name == "foo"
        assert c.tag == Tag(bits="0100")

    def test_named_bin_tag(self):
        c = parse_one("foo$10 = Foo;")
        assert c.name == "foo"
        assert c.tag == Tag(bits="10")

    def test_named_empty_tag(self):
        c = parse_one("foo#_ = Foo;")
        assert c.name == "foo"
        assert c.tag == Tag()

    def test_anonymous(self):
        c = parse_one("_ = Foo;")
        assert c.name is None
        assert c.tag == Tag()

    def test_anonymous_with_tag(self):
        c = parse_one("_#4 = Foo;")
        assert c.name is None
        assert c.tag == Tag(bits="0100")

    def test_special_prefix(self):
        c = parse_one("!foo#_ = Foo;")
        assert c.is_special is True
        assert c.name == "foo"

    def test_uppercase_constructor_rejected(self):
        with pytest.raises(ParseError, match="lowercase"):
            _ = parse("Foo = Foo;")


# ── Tag decoding ─────────────────────────────────────────────────────


class TestTagDecoding:
    def test_hex_single_digit(self):
        c = parse_one("foo#4 = T;")
        assert c.tag.bits == "0100"

    def test_hex_multi(self):
        c = parse_one("foo#a = T;")
        assert c.tag.bits == "1010"

    def test_hex_trailing_underscore(self):
        """#abc_ strips trailing zeros then the last 1."""
        c = parse_one("foo#abc_ = T;")
        # abc = 1010 1011 1100, strip trailing 00 -> 1010101111, strip last 1 -> 101010111
        assert c.tag.bits == "101010111"

    def test_bin(self):
        c = parse_one("foo$010 = T;")
        assert c.tag.bits == "010"

    def test_empty_hex(self):
        c = parse_one("foo#_ = T;")
        assert c.tag.bits == ""

    def test_empty_bin(self):
        c = parse_one("foo$_ = T;")
        assert c.tag.bits == ""


# ── Result type ───────────────────────────────────────────────────────


class TestResultType:
    def test_simple(self):
        c = parse_one("foo = Bar;")
        assert c.result_type == "Bar"

    def test_with_params(self):
        c = parse_one("foo#_ {n:#} {X:Type} = Bar n X;")
        assert c.result_type == "Bar"
        assert len(c.result_params) == 2
        assert c.result_params[0] == ResultParam(expr=Identifier("n"))
        assert c.result_params[1] == ResultParam(expr=Identifier("X"))

    def test_negated_params(self):
        c = parse_one("unary_zero$0 = Unary ~0;")
        assert c.result_params == [ResultParam(expr=IntConst(0), negated=True)]

    def test_negated_expr_param(self):
        c = parse_one("unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);")
        assert c.result_params[0].negated is True
        assert c.result_params[0].expr == Add(left=Identifier("n"), right=IntConst(1))

    def test_lowercase_result_type_rejected(self):
        with pytest.raises(ParseError, match="uppercase"):
            _ = parse("foo = bar;")


# ── Implicit parameters ──────────────────────────────────────────────


class TestImplicitParams:
    def test_nat_param(self):
        c = parse_one("foo#_ {n:#} = Foo n;")
        assert c.fields[0] == ImplicitParam(name="n", is_type=False)

    def test_type_param(self):
        c = parse_one("foo#_ {X:Type} = Foo X;")
        assert c.fields[0] == ImplicitParam(name="X", is_type=True)

    def test_multiple(self):
        c = parse_one("foo#_ {n:#} {X:Type} = Foo n X;")
        assert len(c.fields) == 2
        assert isinstance(c.fields[0], ImplicitParam)
        assert isinstance(c.fields[1], ImplicitParam)


# ── Constraints ───────────────────────────────────────────────────────


class TestConstraints:
    def test_equality(self):
        c = parse_one("foo#_ {n:#} {m:#} {l:#} {n = (~m) + l} = Foo n;")
        constraint = c.fields[3]
        assert isinstance(constraint, Constraint)
        assert isinstance(constraint.expr, Compare)
        assert constraint.expr.op == CompareOp.EQ

    def test_leq(self):
        c = parse_one("foo#_ {n:#} {n <= 30} = Foo n;")
        constraint = c.fields[1]
        assert isinstance(constraint, Constraint)
        assert isinstance(constraint.expr, Compare)
        assert constraint.expr.op == CompareOp.LE

    def test_geq(self):
        c = parse_one("foo#_ {n:#} {n >= 1} = Foo n;")
        constraint = c.fields[1]
        assert isinstance(constraint, Constraint)
        assert isinstance(constraint.expr, Compare)
        assert constraint.expr.op == CompareOp.GE


# ── Explicit fields ──────────────────────────────────────────────────


class TestExplicitFields:
    def test_named(self):
        c = parse_one("foo x:uint32 = Foo;")
        f = c.fields[0]
        assert isinstance(f, ExplicitField)
        assert f.name == "x"
        assert f.type_expr == Identifier("uint32")

    def test_unnamed(self):
        c = parse_one("_ (Message Any) = MessageAny;")
        f = c.fields[0]
        assert isinstance(f, ExplicitField)
        assert f.name is None
        assert isinstance(f.type_expr, Apply)

    def test_anonymous_name(self):
        """'_ :' means field with no name."""
        c = parse_one("_ _:MsgAddressInt = MsgAddress;")
        f = c.fields[0]
        assert isinstance(f, ExplicitField)
        assert f.name is None
        assert f.type_expr == Identifier("MsgAddressInt")

    def test_multiple_fields(self):
        c = parse_one("foo a:uint32 b:uint64 = Foo;")
        assert len(c.fields) == 2
        assert all(isinstance(f, ExplicitField) for f in c.fields)


# ── Expression parsing ────────────────────────────────────────────────


class TestExpressions:
    def test_application_in_parens(self):
        """Application requires parentheses in field position."""
        c = parse_one("foo x:(Maybe uint32) = Foo;")
        f = c.fields[0]
        assert isinstance(f, ExplicitField)
        assert f.type_expr == Apply(function=Identifier("Maybe"), arguments=[Identifier("uint32")])

    def test_multi_arg_application(self):
        c = parse_one("foo x:(Hashmap 256 Account) = Foo;")
        f = c.fields[0]
        assert isinstance(f, ExplicitField)
        assert f.type_expr == Apply(
            function=Identifier("Hashmap"),
            arguments=[IntConst(256), Identifier("Account")],
        )

    def test_cell_ref(self):
        c = parse_one("foo x:^Cell = Foo;")
        f = c.fields[0]
        assert isinstance(f, ExplicitField)
        assert f.type_expr == CellRef(inner=Identifier("Cell"))

    def test_cell_ref_application(self):
        c = parse_one("foo x:^(Hashmap 8 X) = Foo;")
        f = c.fields[0]
        assert isinstance(f, ExplicitField)
        assert isinstance(f.type_expr, CellRef)
        assert isinstance(f.type_expr.inner, Apply)

    def test_conditional(self):
        c = parse_one("foo flag:Bool x:flag?uint32 = Foo;")
        f = c.fields[1]
        assert isinstance(f, ExplicitField)
        assert f.type_expr == Conditional(
            selector=Identifier("flag"), type_expr=Identifier("uint32")
        )

    def test_conditional_cellref(self):
        c = parse_one("foo flag:Bool x:flag?^Cell = Foo;")
        f = c.fields[1]
        assert isinstance(f, ExplicitField)
        assert f.type_expr == Conditional(
            selector=Identifier("flag"), type_expr=CellRef(inner=Identifier("Cell"))
        )

    def test_getbit(self):
        c = parse_one("foo flags:uint8 x:flags.3?^Cell = Foo;")
        f = c.fields[1]
        assert isinstance(f, ExplicitField)
        assert isinstance(f.type_expr, Conditional)
        assert f.type_expr.selector == GetBit(value=Identifier("flags"), bit=IntConst(3))

    def test_addition(self):
        c = parse_one("foo#_ {n:#} {m:#} {n = m + 1} = Foo n;")
        constraint = c.fields[2]
        assert isinstance(constraint, Constraint)
        assert isinstance(constraint.expr, Compare)
        assert constraint.expr.right == Add(left=Identifier("m"), right=IntConst(1))

    def test_multiplication_tuple(self):
        c = parse_one("foo#_ {n:#} s:(n * Bit) = Foo n;")
        f = c.fields[1]
        assert isinstance(f, ExplicitField)
        assert f.type_expr == Multiply(left=Identifier("n"), right=Identifier("Bit"))

    def test_negated_ident(self):
        c = parse_one("foo#_ {n:#} {m:#} {n = (~m) + 1} = Foo n;")
        constraint = c.fields[2]
        assert isinstance(constraint, Constraint)
        assert isinstance(constraint.expr, Compare)
        add = constraint.expr.right
        assert isinstance(add, Add)
        assert add.left == NegatedIdentifier("m")

    def test_hash_as_identifier(self):
        """Bare # in expression becomes Identifier('#')."""
        c = parse_one("foo x:(## 1) = Foo;")
        f = c.fields[0]
        assert isinstance(f, ExplicitField)
        assert f.type_expr == Apply(function=Identifier("##"), arguments=[IntConst(1)])

    def test_hash_leq_application(self):
        c = parse_one("foo x:(#<= 30) = Foo;")
        f = c.fields[0]
        assert isinstance(f, ExplicitField)
        assert f.type_expr == Apply(function=Identifier("#<="), arguments=[IntConst(30)])

    def test_inline_record(self):
        c = parse_one("foo x:[a:uint32 b:uint64] = Foo;")
        f = c.fields[0]
        assert isinstance(f, ExplicitField)
        rec = f.type_expr
        assert isinstance(rec, InlineRecord)
        assert len(rec.fields) == 2
        assert isinstance(rec.fields[0], ExplicitField)
        assert rec.fields[0].name == "a"

    def test_caret_inline_record(self):
        c = parse_one("foo x:^[a:uint32 b:uint64] = Foo;")
        f = c.fields[0]
        assert isinstance(f, ExplicitField)
        assert isinstance(f.type_expr, CellRef)
        assert isinstance(f.type_expr.inner, InlineRecord)


# ── Field type is parsed at conditional level ─────────────────────────


class TestFieldTypePrecedence:
    def test_bare_ident_is_one_field(self):
        """Without parens, each bare identifier is a separate field."""
        c = parse_one("foo Maybe X = Foo;")
        assert len(c.fields) == 2
        assert isinstance(c.fields[0], ExplicitField)
        assert c.fields[0].type_expr == Identifier("Maybe")
        assert isinstance(c.fields[1], ExplicitField)
        assert c.fields[1].type_expr == Identifier("X")

    def test_parens_enable_application(self):
        c = parse_one("foo (Maybe X) = Foo;")
        assert len(c.fields) == 1
        f = c.fields[0]
        assert isinstance(f, ExplicitField)
        assert f.type_expr == Apply(function=Identifier("Maybe"), arguments=[Identifier("X")])


# ── Realistic constructors from block.tlb ─────────────────────────────


class TestBlockTlbConstructors:
    def test_hashmap_edge(self):
        c = parse_one(
            """
hm_edge#_ {n:#} {X:Type} {l:#} {m:#} label:(HmLabel ~l n) {n = (~m) + l} node:(HashmapNode m X) = Hashmap n X;"""
        )
        assert c.name == "hm_edge"
        assert c.tag == Tag()  # #_ = empty
        assert c.result_type == "Hashmap"
        assert len(c.result_params) == 2

        # Check label field
        label = [f for f in c.fields if isinstance(f, ExplicitField) and f.name == "label"]
        assert len(label) == 1
        assert isinstance(label[0].type_expr, Apply)

    def test_unary(self):
        schema = parse(
            "unary_zero$0 = Unary ~0;\nunary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);"
        )
        assert len(schema.constructors) == 2
        zero = schema.constructors[0]
        assert zero.result_params == [ResultParam(IntConst(0), negated=True)]
        succ = schema.constructors[1]
        assert succ.result_params[0].negated is True

    def test_maybe(self):
        schema = parse("nothing$0 {X:Type} = Maybe X;\njust$1 {X:Type} value:X = Maybe X;")
        assert len(schema.constructors) == 2

    def test_message(self):
        c = parse_one(
            """
message$_ {X:Type} info:CommonMsgInfo init:(Maybe (Either StateInit ^StateInit)) body:(Either X ^X) = Message X;"""
        )
        assert c.name == "message"
        assert c.result_type == "Message"
        init_field = [f for f in c.fields if isinstance(f, ExplicitField) and f.name == "init"]
        assert len(init_field) == 1
        # (Maybe (Either StateInit ^StateInit))
        assert isinstance(init_field[0].type_expr, Apply)

    def test_full_block_tlb(self):
        with open("crypto/block/block.tlb") as f:
            text = f.read()
        tokens = Lexer(text).tokenize()
        schema = Parser(tokens).parse()
        assert len(schema.constructors) > 300

        # Spot-check some types
        type_names = {c.result_type for c in schema.constructors}
        assert "Hashmap" in type_names
        assert "HashmapE" in type_names
        assert "Message" in type_names
        assert "Block" in type_names


# ── Error reporting ───────────────────────────────────────────────────


class TestImports:
    def test_no_imports(self):
        schema = parse("foo = Foo;")
        assert schema.imports == []

    def test_single_import(self):
        schema = parse("//@import block.tlb\n")
        assert schema.imports == [Import(path="block.tlb")]
        assert schema.constructors == []

    def test_import_then_constructor(self):
        schema = parse("//@import block.tlb\nfoo = Foo;")
        assert schema.imports == [Import(path="block.tlb")]
        assert len(schema.constructors) == 1
        assert schema.constructors[0].name == "foo"

    def test_multiple_imports(self):
        schema = parse("//@import a.tlb\n//@import b.tlb\nfoo = Foo;")
        assert schema.imports == [Import(path="a.tlb"), Import(path="b.tlb")]

    def test_import_after_constructor_rejected(self):
        with pytest.raises(ParseError, match="before any constructor"):
            _ = parse("foo = Foo;\n//@import block.tlb\n")

    def test_import_between_constructors_rejected(self):
        with pytest.raises(ParseError, match="before any constructor"):
            _ = parse("foo = Foo;\n//@import block.tlb\nbar = Bar;\n")


class TestErrors:
    def test_missing_semicolon(self):
        with pytest.raises(ParseError, match="';'"):
            _ = parse("foo = Foo")

    def test_missing_equals(self):
        with pytest.raises(ParseError):
            _ = parse("foo ;")

    def test_bad_implicit_type(self):
        with pytest.raises(ParseError, match="'#' or 'Type'"):
            _ = parse("foo {n:uint32} = Foo;")

    def test_missing_rparen(self):
        with pytest.raises(ParseError, match="'\\)'"):
            _ = parse("foo x:(Maybe X = Foo;")
