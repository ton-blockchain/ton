"""TL-B Parser: Recursive descent parser producing a typed AST.

Field types are parsed at the 'conditional' precedence level (expr95 in the
reference C++ parser). Application and arithmetic require parentheses in field
type position. Inside parentheses, the full expression grammar is available.

See grammar.md for the complete grammar specification.
"""

from .ast_nodes import (
    Add,
    Apply,
    CellRef,
    Compare,
    CompareOp,
    Conditional,
    Constraint,
    Constructor,
    ExplicitField,
    FieldDef,
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
    TypeExpr,
)
from .lexer import Token, TokenType


class ParseError(Exception):
    def __init__(self, msg: str, token: Token | None = None) -> None:
        if token is not None:
            super().__init__(f"Parse error at L{token.line}:{token.col}: {msg} (got {token})")
        else:
            super().__init__(f"Parse error: {msg}")
        self.token: Token | None = token


# ── Tag decoding helpers ──────────────────────────────────────────────


def _decode_hex_tag(raw: str) -> str:
    """Decode a hex tag token value like '#4', '#abc_', '#_' into a binary string."""
    body = raw[1:]  # strip leading #
    if body == "_" or body == "":
        return ""

    has_trailing = body.endswith("_")
    if has_trailing:
        body = body[:-1]
    if not body:
        return ""

    bits = "".join(f"{int(c, 16):04b}" for c in body)

    if has_trailing:
        bits = bits.rstrip("0")
        if bits.endswith("1"):
            bits = bits[:-1]

    return bits


def _decode_bin_tag(raw: str) -> str:
    """Decode a binary tag token value like '$10', '$_' into a binary string."""
    body = raw[1:]  # strip leading $
    if body == "_" or body == "":
        return ""

    has_trailing = body.endswith("_")
    if has_trailing:
        body = body[:-1]
        body = body.rstrip("0")
        if body.endswith("1"):
            body = body[:-1]

    return body


# ── Token set that can start a term ──────────────────────────────────

_TERM_STARTERS = frozenset(
    {
        TokenType.LPAREN,
        TokenType.LBRACKET,
        TokenType.CARET,
        TokenType.TILDE,
        TokenType.IDENT,
        TokenType.NAT_CONST,
        TokenType.HASH,
        TokenType.HASH_HASH,
        TokenType.HASH_LT,
        TokenType.HASH_LEQ,
    }
)

_COMPARE_OPS: dict[TokenType, CompareOp] = {
    TokenType.EQUALS: CompareOp.EQ,
    TokenType.LT: CompareOp.LT,
    TokenType.LEQ: CompareOp.LE,
    TokenType.GT: CompareOp.GT,
    TokenType.GEQ: CompareOp.GE,
}


class Parser:
    """Recursive descent TL-B parser."""

    _tokens: list[Token]
    _pos: int

    def __init__(self, tokens: list[Token]) -> None:
        self._tokens = tokens
        self._pos = 0

    # ── Cursor helpers ────────────────────────────────────────────────

    def _peek(self) -> Token:
        return self._tokens[self._pos]

    def _peek_at(self, offset: int) -> Token:
        i = self._pos + offset
        if i < len(self._tokens):
            return self._tokens[i]
        return self._tokens[-1]  # EOF

    def _advance(self) -> Token:
        tok = self._tokens[self._pos]
        self._pos += 1
        return tok

    def _at(self, *types: TokenType) -> bool:
        return self._tokens[self._pos].type in types

    def _expect(self, tt: TokenType, what: str | None = None) -> Token:
        tok = self._advance()
        if tok.type != tt:
            expected = what or tt.name
            raise ParseError(f"expected {expected}", tok)
        return tok

    def _at_end(self) -> bool:
        return self._tokens[self._pos].type == TokenType.EOF

    def _can_start_term(self) -> bool:
        return self._tokens[self._pos].type in _TERM_STARTERS

    # ── Top level ─────────────────────────────────────────────────────

    def parse(self) -> Schema:
        schema = Schema()
        while self._at(TokenType.IMPORT):
            tok = self._advance()
            schema.imports.append(Import(path=tok.value))
        while not self._at_end():
            if self._at(TokenType.IMPORT):
                raise ParseError(
                    "//@import directives must appear before any constructor",
                    self._peek(),
                )
            schema.constructors.append(self._parse_constructor_def())
        return schema

    # ── Constructor definition ────────────────────────────────────────

    def _parse_constructor_def(self) -> Constructor:
        is_special = False
        if self._at(TokenType.BANG):
            _ = self._advance()
            is_special = True

        name, tag = self._parse_constructor_head()
        fields = self._parse_field_list(TokenType.EQUALS)
        _ = self._expect(TokenType.EQUALS, "'='")

        result_type_tok = self._expect(TokenType.IDENT, "result type name")
        if not result_type_tok.value[0].isupper():
            raise ParseError(
                "result type name must start with an uppercase letter",
                result_type_tok,
            )
        result_type = result_type_tok.value

        result_params: list[ResultParam] = []
        while not self._at(TokenType.SEMICOLON) and not self._at_end():
            result_params.append(self._parse_result_param())

        _ = self._expect(TokenType.SEMICOLON, "';'")

        return Constructor(
            name=name,
            tag=tag,
            fields=fields,
            result_type=result_type,
            result_params=result_params,
            is_special=is_special,
        )

    def _parse_constructor_head(self) -> tuple[str | None, Tag]:
        tok = self._peek()

        if tok.type == TokenType.UNDERSCORE:
            _ = self._advance()
            name = None
        elif tok.type == TokenType.IDENT:
            if tok.value[0].isupper():
                raise ParseError("constructor name must start with a lowercase letter", tok)
            _ = self._advance()
            name = tok.value
        else:
            raise ParseError("expected constructor name or '_'", tok)

        # Parse optional tag
        if self._at(TokenType.HEX_TAG):
            bits = _decode_hex_tag(self._advance().value)
            tag = Tag(bits=bits)
        elif self._at(TokenType.BIN_TAG):
            bits = _decode_bin_tag(self._advance().value)
            tag = Tag(bits=bits)
        elif name is not None:
            tag = Tag(is_auto=True)
        else:
            tag = Tag()

        return name, tag

    # ── Field list ────────────────────────────────────────────────────

    def _parse_field_list(self, stop: TokenType) -> list[FieldDef]:
        """Parse fields until we hit `stop` token or ']'."""
        fields: list[FieldDef] = []
        while not self._at(stop, TokenType.RBRACKET) and not self._at_end():
            if self._at(TokenType.LBRACE):
                fields.append(self._parse_braced())
            elif self._is_named_field():
                fields.append(self._parse_named_field())
            else:
                fields.append(self._parse_unnamed_field())
        return fields

    def _is_named_field(self) -> bool:
        """Check if the next tokens are `(IDENT | '_') ':'`."""
        if self._at(TokenType.IDENT, TokenType.UNDERSCORE):
            return self._peek_at(1).type == TokenType.COLON
        return False

    def _parse_named_field(self) -> ExplicitField:
        tok = self._advance()  # IDENT or UNDERSCORE
        name = tok.value if tok.type == TokenType.IDENT else None
        _ = self._expect(TokenType.COLON, "':'")
        type_expr = self._parse_conditional()
        return ExplicitField(name=name, type_expr=type_expr)

    def _parse_unnamed_field(self) -> ExplicitField:
        type_expr = self._parse_conditional()
        return ExplicitField(name=None, type_expr=type_expr)

    def _parse_braced(self) -> ImplicitParam | Constraint:
        """Parse { implicit_param | constraint }."""
        _ = self._expect(TokenType.LBRACE, "'{'")

        # Disambiguate: IDENT ':' → implicit param, otherwise constraint
        if self._at(TokenType.IDENT) and self._peek_at(1).type == TokenType.COLON:
            result = self._parse_implicit_param()
        else:
            result = Constraint(expr=self._parse_expr())

        _ = self._expect(TokenType.RBRACE, "'}'")
        return result

    def _parse_implicit_param(self) -> ImplicitParam:
        name_tok = self._expect(TokenType.IDENT, "parameter name")
        _ = self._expect(TokenType.COLON, "':'")

        if self._at(TokenType.HASH):
            _ = self._advance()
            return ImplicitParam(name=name_tok.value, is_type=False)
        elif self._at(TokenType.TYPE_KW):
            _ = self._advance()
            return ImplicitParam(name=name_tok.value, is_type=True)
        else:
            raise ParseError("expected '#' or 'Type' after implicit parameter name", self._peek())

    # ── Result parameters ─────────────────────────────────────────────

    def _parse_result_param(self) -> ResultParam:
        negated = False
        if self._at(TokenType.TILDE):
            _ = self._advance()
            negated = True

        expr = self._parse_result_atom()
        return ResultParam(expr=expr, negated=negated)

    def _parse_result_atom(self) -> TypeExpr:
        if self._at(TokenType.NAT_CONST):
            return IntConst(value=int(self._advance().value))

        if self._at(TokenType.IDENT):
            return Identifier(name=self._advance().value)

        if self._at(TokenType.LPAREN):
            _ = self._advance()
            expr = self._parse_expr()
            _ = self._expect(TokenType.RPAREN, "')'")
            return expr

        raise ParseError("expected result parameter", self._peek())

    # ── Expressions ───────────────────────────────────────────────────

    def _parse_expr(self) -> TypeExpr:
        """Full expression including comparison (lowest precedence)."""
        return self._parse_comparison()

    def _parse_comparison(self) -> TypeExpr:
        left = self._parse_addition()
        tok = self._peek()
        if tok.type in _COMPARE_OPS:
            op = _COMPARE_OPS[tok.type]
            _ = self._advance()
            right = self._parse_addition()
            return Compare(op=op, left=left, right=right)
        return left

    def _parse_addition(self) -> TypeExpr:
        left = self._parse_multiplication()
        while self._at(TokenType.PLUS):
            _ = self._advance()
            right = self._parse_multiplication()
            left = Add(left=left, right=right)
        return left

    def _parse_multiplication(self) -> TypeExpr:
        left = self._parse_application()
        while self._at(TokenType.STAR):
            _ = self._advance()
            right = self._parse_application()
            left = Multiply(left=left, right=right)
        return left

    def _parse_application(self) -> TypeExpr:
        head = self._parse_conditional()
        if not self._can_start_term():
            return head
        args: list[TypeExpr] = []
        while self._can_start_term():
            args.append(self._parse_conditional())
        return Apply(function=head, arguments=args)

    def _parse_conditional(self) -> TypeExpr:
        """expr95: getbit [ '?' term ]."""
        expr = self._parse_getbit()
        if self._at(TokenType.QUESTION):
            _ = self._advance()
            type_expr = self._parse_term()
            return Conditional(selector=expr, type_expr=type_expr)
        return expr

    def _parse_getbit(self) -> TypeExpr:
        """expr97: term [ '.' term ]."""
        expr = self._parse_term()
        if self._at(TokenType.DOT):
            _ = self._advance()
            bit = self._parse_term()
            return GetBit(value=expr, bit=bit)
        return expr

    def _parse_term(self) -> TypeExpr:
        """Atomic expression."""
        tok = self._peek()

        if tok.type == TokenType.LPAREN:
            _ = self._advance()
            expr = self._parse_expr()
            _ = self._expect(TokenType.RPAREN, "')'")
            return expr

        if tok.type == TokenType.LBRACKET:
            _ = self._advance()
            fields = self._parse_field_list(TokenType.RBRACKET)
            _ = self._expect(TokenType.RBRACKET, "']'")
            return InlineRecord(fields=fields)

        if tok.type == TokenType.CARET:
            _ = self._advance()
            inner = self._parse_term()
            return CellRef(inner=inner)

        if tok.type == TokenType.TILDE:
            _ = self._advance()
            name_tok = self._expect(TokenType.IDENT, "identifier after '~'")
            return NegatedIdentifier(name=name_tok.value)

        if tok.type == TokenType.IDENT:
            _ = self._advance()
            return Identifier(name=tok.value)

        if tok.type == TokenType.NAT_CONST:
            _ = self._advance()
            return IntConst(value=int(tok.value))

        if tok.type == TokenType.HASH:
            _ = self._advance()
            return Identifier(name="#")

        if tok.type == TokenType.HASH_HASH:
            _ = self._advance()
            return Identifier(name="##")

        if tok.type == TokenType.HASH_LT:
            _ = self._advance()
            return Identifier(name="#<")

        if tok.type == TokenType.HASH_LEQ:
            _ = self._advance()
            return Identifier(name="#<=")

        raise ParseError("expected expression", tok)
