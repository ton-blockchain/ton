/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include "platform-utils.h"
#include "src-file.h"
#include <string>

namespace tolk {

enum TokenType {
  tok_empty,

  tok_int_const,
  tok_string_const,
  tok_string_modifier,

  tok_identifier,

  tok_true,
  tok_false,
  tok_nil,      // todo "null" keyword is still absent, "nil" in FunC is an empty tuple

  tok_plus,
  tok_minus,
  tok_mul,
  tok_div,
  tok_mod,
  tok_question,
  tok_colon,
  tok_comma,
  tok_semicolon,
  tok_oppar,
  tok_clpar,
  tok_opbracket,
  tok_clbracket,
  tok_opbrace,
  tok_clbrace,
  tok_assign,
  tok_underscore,
  tok_lt,
  tok_gt,
  tok_bitwise_and,
  tok_bitwise_or,
  tok_bitwise_xor,
  tok_bitwise_not,
  tok_dot,

  tok_eq,
  tok_neq,
  tok_leq,
  tok_geq,
  tok_spaceship,
  tok_lshift,
  tok_rshift,
  tok_rshiftR,
  tok_rshiftC,
  tok_divR,
  tok_divC,
  tok_modR,
  tok_modC,
  tok_divmod,
  tok_set_plus,
  tok_set_minus,
  tok_set_mul,
  tok_set_div,
  tok_set_divR,
  tok_set_divC,
  tok_set_mod,
  tok_set_modR,
  tok_set_modC,
  tok_set_lshift,
  tok_set_rshift,
  tok_set_rshiftR,
  tok_set_rshiftC,
  tok_set_bitwise_and,
  tok_set_bitwise_or,
  tok_set_bitwise_xor,

  tok_return,
  tok_var,
  tok_repeat,
  tok_do,
  tok_while,
  tok_until,
  tok_try,
  tok_catch,
  tok_if,
  tok_ifnot,
  tok_then,
  tok_else,
  tok_elseif,
  tok_elseifnot,

  tok_int,
  tok_cell,
  tok_slice,
  tok_builder,
  tok_cont,
  tok_tuple,
  tok_mapsto,
  tok_forall,

  tok_extern,
  tok_global,
  tok_asm,
  tok_impure,
  tok_pure,
  tok_inline,
  tok_inlineref,
  tok_builtin,
  tok_autoapply,
  tok_method_id,
  tok_get,
  tok_operator,
  tok_infix,
  tok_infixl,
  tok_infixr,
  tok_const,

  tok_pragma,
  tok_pragma_name,
  tok_semver,
  tok_include,

  tok_eof
};

// All tolk language is parsed into tokens.
// Lexer::next() returns a Token.
struct Token {
  TokenType type = tok_empty;
  std::string_view str_val;

  Token() = default;
  Token(TokenType type, std::string_view str_val): type(type), str_val(str_val) {}
};

// Lexer::next() is a method to be used externally (while parsing tolk file to AST).
// It's streaming: `next()` parses a token on demand.
// For comments, see lexer.cpp, a comment above Lexer constructor.
class Lexer {
  Token tokens_circularbuf[8]{};
  int last_token_idx = -1;
  int cur_token_idx = -1;
  Token cur_token;  // = tokens_circularbuf[cur_token_idx & 7]

  const SrcFile* file;
  const char *p_start, *p_end, *p_next;
  SrcLocation location;

  void update_location() {
    location.char_offset = static_cast<int>(p_next - p_start);
  }

  GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
  void on_expect_call_failed(const char* str_expected) const;

public:

  explicit Lexer(const SrcFile* file);
  Lexer(const Lexer&) = delete;
  Lexer &operator=(const Lexer&) = delete;

  void add_token(TokenType type, std::string_view str) {
    tokens_circularbuf[++last_token_idx & 7] = Token(type, str);
  }

  void skip_spaces() {
    while (std::isspace(*p_next)) {
      ++p_next;
    }
  }

  void skip_line() {
    while (p_next < p_end && *p_next != '\n' && *p_next != '\r') {
      ++p_next;
    }
    while (*p_next == '\n' || *p_next == '\r') {
      ++p_next;
    }
  }

  void skip_chars(int n) {
    p_next += n;
  }

  bool is_eof() const {
    return p_next >= p_end;
  }

  char char_at() const { return *p_next; }
  char char_at(int shift) const { return *(p_next + shift); }
  const char* c_str() const { return p_next; }

  TokenType tok() const { return cur_token.type; }
  std::string_view cur_str() const { return cur_token.str_val; }
  SrcLocation cur_location() const { return location; }
  const SrcFile* cur_file() const { return file; }

  void next();
  void next_special(TokenType parse_next_as, const char* str_expected);

  void check(TokenType next_tok, const char* str_expected) const {
    if (cur_token.type != next_tok) {
      on_expect_call_failed(str_expected); // unlikely path, not inlined
    }
  }
  void expect(TokenType next_tok, const char* str_expected) {
    if (cur_token.type != next_tok) {
      on_expect_call_failed(str_expected);
    }
    next();
  }

  GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
  void error(const std::string& err_msg) const;
};

void lexer_init();

// todo #ifdef TOLK_PROFILING
void lexer_measure_performance(const AllSrcFiles& files_to_just_parse);

}  // namespace tolk
