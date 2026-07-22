# Extended TL-B Grammar

## Lexical Grammar

```
whitespace     = ' ' | '\t' | '\n' | '\r'
line_comment   = '//' <any char except newline>* newline
block_comment  = '/*' <any chars> '*/'

IDENT          = [a-zA-Z] [a-zA-Z0-9_]*
NAT_CONST      = [0-9]+
HEX_TAG        = '#' [0-9a-fA-F]+ '_'?  |  '#_'
BIN_TAG        = '$' [01]+ '_'?  |  '$_'
```

Single-character tokens: `( ) { } [ ] : ; = _ ? . ~ ^ + * ! < >`

Multi-character tokens: `<= >=`

Special compound tokens: `#` (bare, the nat type), `##`, `#<`, `#<=`

Keywords: `Type`

Directives: a comment starting with `//@` is parsed as a directive, not a comment.
Currently only `//@import <path>` is recognized; the path runs to end of line and
is taken verbatim (after trimming surrounding whitespace).

## Tag Encoding

Hex tags: each hex digit = 4 bits. Trailing `_` strips trailing zeros then the
final `1`. `#_` = empty tag.

Binary tags: literal bits. Trailing `_` works the same. `$_` = empty tag.

Named constructors without explicit tags get a CRC32-based 32-bit auto-tag (computed in sema).
Anonymous constructors without tags have zero-bit tags.

## Syntactic Grammar

```ebnf
schema              = { import_directive } { constructor_def }

import_directive    = '//@import' <path-to-end-of-line>

constructor_def     = [ '!' ] ( IDENT | '_' ) [ HEX_TAG | BIN_TAG ]
                      field_list '=' IDENT { result_param } ';'

field_list          = { field_or_implicit }

field_or_implicit   = '{' implicit_or_constraint '}'
                    | ( IDENT | '_' ) ':' field_type
                    | field_type

implicit_or_constraint
                    = IDENT ':' ( '#' | 'Type' )       (* implicit param *)
                    | expr                              (* constraint *)

field_type          = conditional                       (* parsed at expr95 level *)

result_param        = [ '~' ] result_atom
result_atom         = NAT_CONST | IDENT | '(' expr ')'
```

Disambiguation: inside `{ }`, if the first two tokens are `IDENT ':'`, parse as
implicit param; otherwise parse as constraint. In `field_list`, if the first two
tokens are `(IDENT | '_') ':'`, parse as named field; otherwise parse as unnamed
field.

Constructor names must start lowercase (checked in parser). Result type names must
start uppercase (checked in parser).

### Expressions (lowest to highest precedence)

```ebnf
expr                = addition [ ( '=' | '<' | '<=' | '>' | '>=' ) addition ]
addition            = multiplication { '+' multiplication }
multiplication      = application { '*' application }
application         = conditional { conditional }
conditional         = getbit [ '?' term ]
getbit              = term [ '.' term ]

term                = '(' expr ')'
                    | '[' field_list ']'
                    | '^' term
                    | '~' IDENT
                    | IDENT
                    | NAT_CONST
                    | '#' | '##' | '#<' | '#<='
```

Application continues while the next token can start a `conditional`, i.e. is one
of: `(  IDENT  NAT_CONST  ~  ^  [  #  ##  #<  #<=`.

Field types are parsed at the `conditional` level, not `expr`. This means bare
`Maybe X` in a field position is two separate fields; use `(Maybe X)` for
application.
