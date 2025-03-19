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

  tok_fun,
  tok_type,
  tok_enum,
  tok_struct,
  tok_operator,
  tok_infix,

  tok_global,
  tok_const,
  tok_var,
  tok_val,
  tok_redef,
  tok_mutate,
  tok_self,

  tok_annotation_at,
  tok_colon,
  tok_asm,
  tok_builtin,

  tok_int_const,
  tok_string_const,
  tok_true,
  tok_false,
  tok_null,

  tok_identifier,
  tok_dot,

  tok_plus,
  tok_set_plus,
  tok_minus,
  tok_set_minus,
  tok_mul,
  tok_set_mul,
  tok_div,
  tok_set_div,
  tok_mod,
  tok_set_mod,
  tok_lshift,
  tok_set_lshift,
  tok_rshift,
  tok_set_rshift,
  tok_rshiftR,
  tok_rshiftC,
  tok_bitwise_and,
  tok_set_bitwise_and,
  tok_bitwise_or,
  tok_set_bitwise_or,
  tok_bitwise_xor,
  tok_set_bitwise_xor,
  tok_bitwise_not,

  tok_question,
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
  tok_logical_not,
  tok_logical_and,
  tok_logical_or,

  tok_eq,
  tok_neq,
  tok_leq,
  tok_geq,
  tok_spaceship,
  tok_divR,
  tok_divC,

  tok_return,
  tok_repeat,
  tok_do,
  tok_while,
  tok_break,
  tok_continue,
  tok_try,
  tok_catch,
  tok_throw,
  tok_assert,
  tok_if,
  tok_else,
  tok_match,

  tok_arrow,
  tok_double_arrow,
  tok_as,
  tok_is,

  tok_tolk,
  tok_semver,
  tok_import,
  tok_export,

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

public:

  struct SavedPositionForLookahead {
    const char* p_next = nullptr;
    int cur_token_idx = 0;
    Token cur_token;
    SrcLocation loc;
  };

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

  SavedPositionForLookahead save_parsing_position() const;
  void restore_position(SavedPositionForLookahead saved);
  void hack_replace_rshift_with_one_triangle();

  void check(TokenType next_tok, const char* str_expected) const {
    if (cur_token.type != next_tok) {
      unexpected(str_expected); // unlikely path, not inlined
    }
  }
  void expect(TokenType next_tok, const char* str_expected) {
    if (cur_token.type != next_tok) {
      unexpected(str_expected);
    }
    next();
  }

  GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
  void unexpected(const char* str_expected) const;
  GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
  void error(const std::string& err_msg) const;
};

void lexer_init();

// todo #ifdef TOLK_PROFILING
void lexer_measure_performance(const AllRegisteredSrcFiles& files_to_just_parse);

}  // namespace tolk
