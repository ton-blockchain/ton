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

#include <string>
#include <ostream>
#include "compilation-errors.h"

namespace tolk {

class JsonPrettyOutput {
  static constexpr int MAX_DEPTH = 30;

  std::ostream& os;
  std::string cur_indent_spaces;
  int depth = 0;
  int elems_count[MAX_DEPTH] = {};
  bool cursor_after_key = false;

  void on_new_key(std::string_view key) {
    if (elems_count[depth]++) {
      os << ',' << '\n';
    }
    os << cur_indent_spaces << '"' << key << '"' << ':' << ' ';
    cursor_after_key = true;
  }

  void start_object_or_array(char brace) {
    if (!cursor_after_key) {
      if (elems_count[depth]++) {
        os << ',' << '\n';
      }
      os << cur_indent_spaces;
    }
    os << brace << '\n';    // `{` or `[`

    depth = std::min(depth + 1, MAX_DEPTH - 1);
    cur_indent_spaces += "  ";
    elems_count[depth] = 0;
    cursor_after_key = false;
  }

  void end_object_or_array(char brace) {
    if (elems_count[depth]) {
      os << '\n';
    }

    depth--;
    cur_indent_spaces = cur_indent_spaces.substr(0, cur_indent_spaces.size() - 2);
    os << cur_indent_spaces << brace;   // `}` or `]`
  }

public:
  struct Unescaped {
    std::string_view str_no_escape;
  };
  struct Unquoted {
    std::string_view str_no_enquote;
  };

  explicit JsonPrettyOutput(std::ostream& os) : os(os) {}

  void start_object() {
    start_object_or_array('{');
  }

  void start_object(std::string_view key) {
    on_new_key(key);
    start_object_or_array('{');
  }

  void end_object() {
    end_object_or_array('}');
  }

  void start_array() {
    start_object_or_array('[');
  }

  void start_array(std::string_view key) {
    on_new_key(key);
    start_object_or_array('[');
  }

  void end_array() {
    end_object_or_array(']');
  }

  template<class T>
  JsonPrettyOutput& operator<<(const T& append) {
    os << append;
    return *this;
  }

  template<class T>
  void key_value(std::string_view key, const T& value) {
    on_new_key(key);
    write_value(value);
    cursor_after_key = false;
  }

  void write_value(const char* value) {
    write_value(static_cast<std::string_view>(value));
  }

  void write_value(const std::string& value) {
    write_value(static_cast<std::string_view>(value));
  }

  void write_value(std::string_view value) {
    os << '"';
    for (char c: value) {
      if (c == '"')       os << '\\' << '"';
      else if (c == '\n') os << '\\' << 'n';
      else if (c == '\r') os << '\\' << 'r';
      else if (c == '\t') os << '\\' << 't';
      else if (c == '\b') os << '\\' << 'b';
      else if (c == '\\') os << '\\' << '\\';
      else os << c;
    }
    os << '"';
  }

  void write_value(Unescaped value) {
    os << '"' << value.str_no_escape << '"';
  }

  void write_value(Unquoted value) {
    os << value.str_no_enquote;
  }

  void write_value(int value) {
    os << value;
  }

  void write_value(size_t value) {
    os << value;
  }

  void write_value(int64_t value) {
    os << value;
  }

  void write_value(const td::RefInt256& value) {
    os << '"' << value << '"';
  }

  void write_value(bool value) {
    os << (value ? "true" : "false");
  }

  template<class T>
  void write_value(const T& v) {
    to_json(*this, v);
  }

  static void write_value(const void*) {
    tolk_assert(false && "void* in json output");
  }
};

} // namespace tolk
