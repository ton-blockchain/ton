import pytest
from tlb.generator.lexer import Lexer, LexerError, Token, TokenType


def lex(text: str) -> list[Token]:
    return Lexer(text).tokenize()


def types(text: str) -> list[TokenType]:
    return [t.type for t in lex(text) if t.type != TokenType.EOF]


def values(text: str) -> list[str]:
    return [t.value for t in lex(text) if t.type != TokenType.EOF]


class TestIdentifiers:
    def test_lowercase(self):
        assert types("foo") == [TokenType.IDENT]
        assert values("foo") == ["foo"]

    def test_uppercase(self):
        assert types("Hashmap") == [TokenType.IDENT]
        assert values("Hashmap") == ["Hashmap"]

    def test_with_underscores(self):
        assert values("ihr_disabled") == ["ihr_disabled"]
        assert types("ihr_disabled") == [TokenType.IDENT]

    def test_with_digits(self):
        assert values("uint256") == ["uint256"]

    def test_multiple(self):
        assert values("foo Bar baz") == ["foo", "Bar", "baz"]
        assert types("foo Bar baz") == [TokenType.IDENT] * 3

    def test_type_keyword(self):
        assert types("Type") == [TokenType.TYPE_KW]

    def test_underscore_standalone(self):
        assert types("_") == [TokenType.UNDERSCORE]

    def test_underscore_not_ident_prefix(self):
        """Underscore is always its own token, not part of the next identifier."""
        assert types("_foo") == [TokenType.UNDERSCORE, TokenType.IDENT]


class TestNatConst:
    def test_single_digit(self):
        assert types("0") == [TokenType.NAT_CONST]
        assert values("0") == ["0"]

    def test_multi_digit(self):
        assert values("256") == ["256"]

    def test_adjacent_to_ident(self):
        assert types("32 Bit") == [TokenType.NAT_CONST, TokenType.IDENT]


class TestHashTokens:
    def test_bare_hash(self):
        assert types("#") == [TokenType.HASH]

    def test_hash_hash(self):
        assert types("##") == [TokenType.HASH_HASH]

    def test_hash_lt(self):
        assert types("#<") == [TokenType.HASH_LT]

    def test_hash_leq(self):
        assert types("#<=") == [TokenType.HASH_LEQ]

    def test_hex_tag(self):
        assert types("#4") == [TokenType.HEX_TAG]
        assert values("#4") == ["#4"]

    def test_hex_tag_multi(self):
        assert values("#fffffffe") == ["#fffffffe"]

    def test_hex_tag_trailing_underscore(self):
        assert values("#abc_") == ["#abc_"]
        assert types("#abc_") == [TokenType.HEX_TAG]

    def test_hex_tag_empty(self):
        assert types("#_") == [TokenType.HEX_TAG]
        assert values("#_") == ["#_"]


class TestBinTags:
    def test_bin_tag(self):
        assert types("$10") == [TokenType.BIN_TAG]
        assert values("$10") == ["$10"]

    def test_bin_tag_single_bit(self):
        assert values("$0") == ["$0"]

    def test_bin_tag_empty(self):
        assert types("$_") == [TokenType.BIN_TAG]
        assert values("$_") == ["$_"]

    def test_bin_tag_trailing_underscore(self):
        assert values("$010_") == ["$010_"]

    def test_bare_dollar(self):
        """Bare $ is treated as empty binary tag."""
        assert types("$ ") == [TokenType.BIN_TAG]


class TestComparisonTokens:
    def test_equals(self):
        assert types("=") == [TokenType.EQUALS]

    def test_leq(self):
        assert types("<=") == [TokenType.LEQ]

    def test_geq(self):
        assert types(">=") == [TokenType.GEQ]

    def test_lt(self):
        assert types("<") == [TokenType.LT]

    def test_gt(self):
        assert types(">") == [TokenType.GT]


class TestSimpleTokens:
    def test_all_simple(self):
        assert types("{ } ( ) [ ] : ; ? ~ ^ + * ! .") == [
            TokenType.LBRACE,
            TokenType.RBRACE,
            TokenType.LPAREN,
            TokenType.RPAREN,
            TokenType.LBRACKET,
            TokenType.RBRACKET,
            TokenType.COLON,
            TokenType.SEMICOLON,
            TokenType.QUESTION,
            TokenType.TILDE,
            TokenType.CARET,
            TokenType.PLUS,
            TokenType.STAR,
            TokenType.BANG,
            TokenType.DOT,
        ]


class TestComments:
    def test_line_comment(self):
        assert types("foo // comment\nbar") == [TokenType.IDENT, TokenType.IDENT]
        assert values("foo // comment\nbar") == ["foo", "bar"]

    def test_block_comment(self):
        assert values("foo /* comment */ bar") == ["foo", "bar"]

    def test_nested_block_comment_not_supported(self):
        """Block comments don't nest — first */ ends the comment."""
        toks = lex("foo /* a /* b */ c")
        vals = [t.value for t in toks if t.type != TokenType.EOF]
        assert vals == ["foo", "c"]

    def test_unterminated_block_comment(self):
        with pytest.raises(LexerError, match="unterminated"):
            _ = lex("foo /* never closed")


class TestConstructorLineLexing:
    """Lex complete constructor definition lines from block.tlb."""

    def test_simple_constructor(self):
        toks = types("unit$_ = Unit;")
        assert toks == [
            TokenType.IDENT,
            TokenType.BIN_TAG,
            TokenType.EQUALS,
            TokenType.IDENT,
            TokenType.SEMICOLON,
        ]

    def test_tagged_constructor(self):
        toks = types("msg_envelope#4 cur_addr:IntermediateAddress = MsgEnvelope;")
        assert toks == [
            TokenType.IDENT,
            TokenType.HEX_TAG,
            TokenType.IDENT,
            TokenType.COLON,
            TokenType.IDENT,
            TokenType.EQUALS,
            TokenType.IDENT,
            TokenType.SEMICOLON,
        ]

    def test_implicit_params(self):
        toks = types("{n:#} {X:Type}")
        assert toks == [
            TokenType.LBRACE,
            TokenType.IDENT,
            TokenType.COLON,
            TokenType.HASH,
            TokenType.RBRACE,
            TokenType.LBRACE,
            TokenType.IDENT,
            TokenType.COLON,
            TokenType.TYPE_KW,
            TokenType.RBRACE,
        ]

    def test_anonymous_constructor(self):
        toks = types("_ _:MsgAddressInt = MsgAddress;")
        assert toks == [
            TokenType.UNDERSCORE,
            TokenType.UNDERSCORE,
            TokenType.COLON,
            TokenType.IDENT,
            TokenType.EQUALS,
            TokenType.IDENT,
            TokenType.SEMICOLON,
        ]

    def test_hash_leq_in_expr(self):
        toks = types("(#<= 30)")
        assert toks == [
            TokenType.LPAREN,
            TokenType.HASH_LEQ,
            TokenType.NAT_CONST,
            TokenType.RPAREN,
        ]

    def test_negated_output(self):
        toks = types("= Unary ~0;")
        assert toks == [
            TokenType.EQUALS,
            TokenType.IDENT,
            TokenType.TILDE,
            TokenType.NAT_CONST,
            TokenType.SEMICOLON,
        ]


class TestImportDirective:
    def _import_token(self, text: str) -> Token:
        toks = [t for t in lex(text) if t.type != TokenType.EOF]
        assert len(toks) == 1
        assert toks[0].type == TokenType.IMPORT
        return toks[0]

    def test_basic(self):
        tok = self._import_token("//@import block.tlb")
        assert tok.value == "block.tlb"

    def test_trailing_newline(self):
        tok = self._import_token("//@import block.tlb\n")
        assert tok.value == "block.tlb"

    def test_extra_whitespace(self):
        tok = self._import_token("//@import   block.tlb   \n")
        assert tok.value == "block.tlb"

    def test_path_with_slashes(self):
        tok = self._import_token("//@import path/to/block.tlb\n")
        assert tok.value == "path/to/block.tlb"

    def test_position_tracking(self):
        tok = self._import_token("//@import block.tlb\n")
        assert tok.line == 1
        assert tok.col == 1

    def test_followed_by_constructor(self):
        toks = [t.type for t in lex("//@import block.tlb\nfoo = Foo;") if t.type != TokenType.EOF]
        assert toks == [
            TokenType.IMPORT,
            TokenType.IDENT,
            TokenType.EQUALS,
            TokenType.IDENT,
            TokenType.SEMICOLON,
        ]

    def test_multiple_imports(self):
        toks = [t for t in lex("//@import a.tlb\n//@import b.tlb\n") if t.type != TokenType.EOF]
        assert [t.type for t in toks] == [TokenType.IMPORT, TokenType.IMPORT]
        assert [t.value for t in toks] == ["a.tlb", "b.tlb"]

    def test_plain_comment_unchanged(self):
        # //@ at column-3 of a regular comment should still take effect — but
        # a line that starts with `//` (no `@`) is just a comment.
        toks = [t.type for t in lex("// not a directive\nfoo") if t.type != TokenType.EOF]
        assert toks == [TokenType.IDENT]

    def test_missing_path(self):
        with pytest.raises(LexerError, match="requires a path"):
            _ = lex("//@import\n")

    def test_missing_path_only_whitespace(self):
        with pytest.raises(LexerError, match="requires a path"):
            _ = lex("//@import   \n")

    def test_unknown_directive(self):
        with pytest.raises(LexerError, match="unknown directive"):
            _ = lex("//@frobnicate foo\n")

    def test_empty_directive_name(self):
        with pytest.raises(LexerError, match="expected directive name"):
            _ = lex("//@\n")


class TestBlockTlb:
    def test_lex_full_block_tlb(self):
        with open("crypto/block/block.tlb") as f:
            text = f.read()
        tokens = Lexer(text).tokenize()
        assert tokens[-1].type == TokenType.EOF
        assert len(tokens) > 1000

    def test_eof(self):
        tokens = lex("")
        assert len(tokens) == 1
        assert tokens[0].type == TokenType.EOF


class TestPositionTracking:
    def test_line_col(self):
        tokens = lex("foo\nbar")
        assert tokens[0].line == 1 and tokens[0].col == 1
        assert tokens[1].line == 2 and tokens[1].col == 1

    def test_col_advances(self):
        tokens = lex("a b c")
        assert tokens[0].col == 1
        assert tokens[1].col == 3
        assert tokens[2].col == 5


class TestErrors:
    def test_unexpected_char(self):
        with pytest.raises(LexerError, match="unexpected"):
            _ = lex("foo @ bar")
