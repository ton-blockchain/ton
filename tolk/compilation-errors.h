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

#include "fwd-declarations.h"
#include "src-file.h"
#include "platform-utils.h"
#include <string>
#include <vector>

namespace tolk {

#define tolk_assert(expr) if(UNLIKELY(!(expr))) on_assertion_failed(#expr, __FILE__, __LINE__);

GNU_ATTRIBUTE_COLD GNU_ATTRIBUTE_NORETURN
void on_assertion_failed(const char *description, const char *file_name, int line_number);

class JsonPrettyOutput;   // forward declaration

// A compilation error may optionally have several related locations.
// For example, can't `break` from a loop if it has `return` (break is secondary, also highlighted).
struct ErrorSecondaryLocation {
  SrcRange range;
  std::string note;
};

// Error is a compilation error created with `err("...")`:
// 1) err("...").fire() — throw an error immediately and interrupt compilation, it's [[noreturn]]
// 2) err("...").collect() — store errors in a buffer, all collected will be thrown before AST->IR stage
class [[nodiscard]] Error {
  std::string message;
  std::vector<ErrorSecondaryLocation> secondary_locations;

  Error& with_secondary(AnyV at, std::string note);
  Error& with_secondary(const Symbol* at_sym, std::string note);
  Error& with_secondary(SrcRange range, std::string note);

public:
  explicit Error(std::string message)
    : message(std::move(message)) {}

  template<class Loc, class... Args>
  Error& with_secondary(Loc at, const char* note_tpl, Args&&... args);

  GNU_ATTRIBUTE_NORETURN
  void fire(AnyV at, FunctionPtr in_function = nullptr);
  GNU_ATTRIBUTE_NORETURN
  void fire(const Symbol* at_sym, FunctionPtr in_function = nullptr);
  GNU_ATTRIBUTE_NORETURN
  void fire(SrcRange range, FunctionPtr in_function = nullptr);

  void collect(AnyV at, FunctionPtr in_function = nullptr);
  void collect(const Symbol* at_sym, FunctionPtr in_function = nullptr);
  void collect(SrcRange range, FunctionPtr in_function = nullptr);
};

// `err("format {} string", arg)` and `with_secondary(..., "note {}", arg)` share this formatter.
class ErrorBuilder {
  const char* tpl;
  std::vector<std::string> args;

  void add_arg(std::string v) {
    args.push_back(std::move(v));
  }

public:
  explicit ErrorBuilder(const char* tpl)
    : tpl(tpl) {}

  void push(const char* v);
  void push(const std::string& v);
  void push(std::string_view v);
  void push(TypePtr v);
  void push(FunctionPtr v);
  void push(StructPtr v);
  void push(StructFieldPtr v);
  void push(AliasDefPtr v);
  void push(EnumDefPtr v);
  void push(EnumMemberPtr v);
  void push(GlobalConstPtr v);
  void push(LocalVarPtr v);
  void push(int v);
  void push(size_t v);
  void push(bool v);
  void push(void*) = delete;
  void push(const void*) = delete;
  void push(std::nullptr_t) = delete;

  [[nodiscard]] std::string build_string() const;
};

template<class... Args>
inline std::string format_err_message(const char* tpl, Args&&... args) {
  ErrorBuilder b(tpl);
  (b.push(std::forward<Args>(args)), ...);
  return b.build_string();
}

template<class Loc, class... Args>
inline Error& Error::with_secondary(Loc at, const char* note_tpl, Args&&... args) {
  return with_secondary(at, format_err_message(note_tpl, std::forward<Args>(args)...));
}

template<class... Args>
GNU_ATTRIBUTE_COLD
inline Error err(const char* tpl, Args&&... args) {
  return Error(format_err_message(tpl, std::forward<Args>(args)...));
}


struct Fatal final : std::exception {
  std::string message;

  explicit Fatal(std::string message)
    : message(std::move(message)) {}

  const char* what() const noexcept override {
    return message.c_str();
  }
};

struct TooDeepStackFatal final : std::exception {
  const char* what() const noexcept override {
    return "too deep stack";
  }
};

struct ThrownParseError final : std::exception {
  FunctionPtr in_function;
  SrcRange range;
  std::string message;
  std::vector<ErrorSecondaryLocation> secondary_locations;

  ThrownParseError(FunctionPtr in_function, SrcRange range, std::string message,
                   std::vector<ErrorSecondaryLocation> secondary_locations)
    : in_function(in_function), range(range), message(std::move(message))
    , secondary_locations(std::move(secondary_locations)) {}

  const char* what() const noexcept override {
    return message.c_str();
  }
  void output_to_console(std::ostream& os) const;
  void output_to_json(JsonPrettyOutput& json) const;
};

struct UnexpectedASTNodeKind final : std::exception {
  AnyV v_unexpected;
  std::string message;

  explicit UnexpectedASTNodeKind(AnyV v_unexpected, const char* place_where);

  const char* what() const noexcept override {
    return message.c_str();
  }
};

// `err("...").collect()` (not `fire`) are stored in a global buffer.
class ErrorCollector {
  std::vector<ThrownParseError> errors;

public:
  void add(ThrownParseError err) {
    errors.push_back(std::move(err));
  }

  bool empty() const {
    return errors.empty();
  }

  std::vector<ThrownParseError>&& flush() {
    return std::move(errors);
  }
};

} // namespace tolk
