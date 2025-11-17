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

class [[nodiscard]] Error {
  std::string message;

public:
  explicit Error(std::string message)
    : message(std::move(message)) {}

  GNU_ATTRIBUTE_NORETURN
  void fire(AnyV at, FunctionPtr in_function = nullptr) const;
  GNU_ATTRIBUTE_NORETURN
  void fire(SrcRange range, FunctionPtr in_function = nullptr) const;

  void warning(AnyV at, FunctionPtr in_function = nullptr) const;
  void warning(SrcRange range, FunctionPtr in_function = nullptr) const;
};

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

  [[nodiscard]] Error build() const;
};


template<class... Args>
GNU_ATTRIBUTE_COLD
inline Error err(const char* tpl, Args&&... args) {
  ErrorBuilder b(tpl);
  (b.push(std::forward<Args>(args)), ...);
  return b.build();
}


struct Fatal final : std::exception {
  std::string message;

  explicit Fatal(std::string message)
    : message(std::move(message)) {}

  const char* what() const noexcept override {
    return message.c_str();
  }
};

struct ThrownParseError final : std::exception {
  std::string in_function;
  SrcRange range;
  std::string message;

  ThrownParseError(std::string in_function, SrcRange range, std::string message)
    : in_function(std::move(in_function)), range(range), message(std::move(message)) {}

  const char* what() const noexcept override {
    return message.c_str();
  }
  void output_compilation_error(std::ostream& os) const;
};

struct UnexpectedASTNodeKind final : std::exception {
  AnyV v_unexpected;
  std::string message;

  explicit UnexpectedASTNodeKind(AnyV v_unexpected, const char* place_where);

  const char* what() const noexcept override {
    return message.c_str();
  }
};

} // namespace tolk
