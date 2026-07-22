"""TL-B Lexer: Tokenizes TL-B schema files."""

from dataclasses import dataclass
from enum import Enum, auto
from typing import final, override


# fmt: off
class TokenType(Enum):
    IDENT = auto()         # any identifier ([a-zA-Z][a-zA-Z0-9_]*)
    NAT_CONST = auto()     # natural number constant
    HEX_TAG = auto()       # #hex or #hex_ or #_
    BIN_TAG = auto()       # $bin or $bin_ or $_
    UNDERSCORE = auto()    # _ (standalone)
    TYPE_KW = auto()       # Type keyword

    HASH = auto()          # # (bare — the nat type)
    HASH_HASH = auto()     # ##
    HASH_LT = auto()       # #<
    HASH_LEQ = auto()      # #<=

    LBRACE = auto()        # {
    RBRACE = auto()        # }
    LPAREN = auto()        # (
    RPAREN = auto()        # )
    LBRACKET = auto()      # [
    RBRACKET = auto()      # ]
    COLON = auto()         # :
    SEMICOLON = auto()     # ;
    EQUALS = auto()        # =
    QUESTION = auto()      # ?
    DOT = auto()           # .
    TILDE = auto()         # ~
    CARET = auto()         # ^
    PLUS = auto()          # +
    STAR = auto()          # *
    BANG = auto()          # !
    LEQ = auto()           # <=
    GEQ = auto()           # >=
    LT = auto()            # <
    GT = auto()            # >

    IMPORT = auto()        # //@import <path>

    EOF = auto()
# fmt: on


@dataclass(frozen=True)
class Token:
    type: TokenType
    value: str
    line: int
    col: int

    @override
    def __repr__(self) -> str:
        return f"Token({self.type.name}, {self.value!r}, L{self.line}:{self.col})"


class LexerError(Exception):
    line: int
    col: int

    def __init__(self, msg: str, line: int, col: int) -> None:
        super().__init__(f"Lexer error at L{line}:{col}: {msg}")
        self.line = line
        self.col = col


_HEX_CHARS = frozenset("0123456789abcdefABCDEF")


@final
class Lexer:
    """Tokenizes TL-B schema text into a list of tokens."""

    _text: str
    _pos: int
    _line: int
    _col: int

    def __init__(self, text: str) -> None:
        self._text = text
        self._pos = 0
        self._line = 1
        self._col = 1

    def _at_end(self) -> bool:
        return self._pos >= len(self._text)

    def _peek(self) -> str:
        if self._at_end():
            return ""
        return self._text[self._pos]

    def _peek_at(self, offset: int) -> str:
        i = self._pos + offset
        if i >= len(self._text):
            return ""
        return self._text[i]

    def _advance(self) -> str:
        ch = self._text[self._pos]
        self._pos += 1
        if ch == "\n":
            self._line += 1
            self._col = 1
        else:
            self._col += 1
        return ch

    def _skip_whitespace_and_comments(self) -> None:
        while not self._at_end():
            ch = self._peek()
            if ch in " \t\n\r":
                _ = self._advance()
                continue
            if ch == "/" and self._peek_at(1) == "/":
                # //@... is a directive — leave it for tokenize() to handle.
                if self._peek_at(2) == "@":
                    break
                while not self._at_end() and self._peek() != "\n":
                    _ = self._advance()
                continue
            if ch == "/" and self._peek_at(1) == "*":
                start_line, start_col = self._line, self._col
                _ = self._advance()  # /
                _ = self._advance()  # *
                while not self._at_end():
                    if self._peek() == "*" and self._peek_at(1) == "/":
                        _ = self._advance()  # *
                        _ = self._advance()  # /
                        break
                    _ = self._advance()
                else:
                    raise LexerError("unterminated block comment", start_line, start_col)
                continue
            break

    def _read_while(self, predicate: str) -> str:
        chars = set(predicate) if len(predicate) > 10 else predicate
        start = self._pos
        while not self._at_end() and self._text[self._pos] in chars:
            self._pos += 1
            self._col += 1
        return self._text[start : self._pos]

    def _read_ident_tail(self) -> str:
        start = self._pos
        while not self._at_end():
            ch = self._text[self._pos]
            if ch.isalnum() or ch == "_":
                self._pos += 1
                self._col += 1
            else:
                break
        return self._text[start : self._pos]

    def _read_digits(self) -> str:
        start = self._pos
        while not self._at_end() and self._text[self._pos].isdigit():
            self._pos += 1
            self._col += 1
        return self._text[start : self._pos]

    def _make(self, tt: TokenType, value: str, line: int, col: int) -> Token:
        return Token(tt, value, line, col)

    def _lex_hash(self, line: int, col: int) -> Token:
        """Lex a token starting with #."""
        _ = self._advance()  # consume #
        nxt = self._peek()

        if nxt == "#":
            _ = self._advance()
            return self._make(TokenType.HASH_HASH, "##", line, col)

        if nxt == "<":
            _ = self._advance()
            if self._peek() == "=":
                _ = self._advance()
                return self._make(TokenType.HASH_LEQ, "#<=", line, col)
            return self._make(TokenType.HASH_LT, "#<", line, col)

        if nxt == "_":
            _ = self._advance()
            return self._make(TokenType.HEX_TAG, "#_", line, col)

        if nxt in _HEX_CHARS:
            hex_digits = self._read_while("0123456789abcdefABCDEF")
            if self._peek() == "_":
                _ = self._advance()
                return self._make(TokenType.HEX_TAG, "#" + hex_digits + "_", line, col)
            return self._make(TokenType.HEX_TAG, "#" + hex_digits, line, col)

        # Bare # (the nat type)
        return self._make(TokenType.HASH, "#", line, col)

    def _lex_directive(self, line: int, col: int) -> Token:
        """Lex a //@<name> ... directive. Currently only //@import <path> is recognized."""
        _ = self._advance()  # /
        _ = self._advance()  # /
        _ = self._advance()  # @
        name = self._read_ident_tail()
        if not name:
            raise LexerError("expected directive name after '//@'", line, col)
        if name == "import":
            while not self._at_end() and self._peek() in " \t":
                _ = self._advance()
            start = self._pos
            while not self._at_end() and self._peek() != "\n":
                self._pos += 1
                self._col += 1
            path = self._text[start : self._pos].rstrip()
            if not path:
                raise LexerError("'//@import' requires a path", line, col)
            return self._make(TokenType.IMPORT, path, line, col)
        raise LexerError(f"unknown directive '//@{name}'", line, col)

    def _lex_dollar(self, line: int, col: int) -> Token:
        """Lex a token starting with $."""
        _ = self._advance()  # consume $
        nxt = self._peek()

        if nxt == "_":
            _ = self._advance()
            return self._make(TokenType.BIN_TAG, "$_", line, col)

        if nxt in "01":
            bin_digits = self._read_while("01")
            if self._peek() == "_":
                _ = self._advance()
                return self._make(TokenType.BIN_TAG, "$" + bin_digits + "_", line, col)
            return self._make(TokenType.BIN_TAG, "$" + bin_digits, line, col)

        # Bare $ (treated as empty binary tag)
        return self._make(TokenType.BIN_TAG, "$_", line, col)

    _SIMPLE_TOKENS: dict[str, TokenType] = {
        "{": TokenType.LBRACE,
        "}": TokenType.RBRACE,
        "(": TokenType.LPAREN,
        ")": TokenType.RPAREN,
        "[": TokenType.LBRACKET,
        "]": TokenType.RBRACKET,
        ":": TokenType.COLON,
        ";": TokenType.SEMICOLON,
        "?": TokenType.QUESTION,
        "~": TokenType.TILDE,
        "^": TokenType.CARET,
        "+": TokenType.PLUS,
        "*": TokenType.STAR,
        "!": TokenType.BANG,
        ".": TokenType.DOT,
    }

    def tokenize(self) -> list[Token]:
        tokens: list[Token] = []
        while True:
            self._skip_whitespace_and_comments()
            if self._at_end():
                tokens.append(self._make(TokenType.EOF, "", self._line, self._col))
                break

            line, col = self._line, self._col
            ch = self._peek()

            if ch == "/" and self._peek_at(1) == "/" and self._peek_at(2) == "@":
                tokens.append(self._lex_directive(line, col))
            elif ch == "#":
                tokens.append(self._lex_hash(line, col))
            elif ch == "$":
                tokens.append(self._lex_dollar(line, col))
            elif ch == "=":
                _ = self._advance()
                tokens.append(self._make(TokenType.EQUALS, "=", line, col))
            elif ch == "<":
                _ = self._advance()
                if self._peek() == "=":
                    _ = self._advance()
                    tokens.append(self._make(TokenType.LEQ, "<=", line, col))
                else:
                    tokens.append(self._make(TokenType.LT, "<", line, col))
            elif ch == ">":
                _ = self._advance()
                if self._peek() == "=":
                    _ = self._advance()
                    tokens.append(self._make(TokenType.GEQ, ">=", line, col))
                else:
                    tokens.append(self._make(TokenType.GT, ">", line, col))
            elif ch == "_":
                _ = self._advance()
                tokens.append(self._make(TokenType.UNDERSCORE, "_", line, col))
            elif ch in self._SIMPLE_TOKENS:
                _ = self._advance()
                tokens.append(self._make(self._SIMPLE_TOKENS[ch], ch, line, col))
            elif ch.isdigit():
                digits = self._read_digits()
                tokens.append(self._make(TokenType.NAT_CONST, digits, line, col))
            elif ch.isalpha():
                first = self._advance()
                rest = self._read_ident_tail()
                ident = first + rest
                if ident == "Type":
                    tokens.append(self._make(TokenType.TYPE_KW, "Type", line, col))
                else:
                    tokens.append(self._make(TokenType.IDENT, ident, line, col))
            else:
                raise LexerError(f"unexpected character: {ch!r}", line, col)

        return tokens
