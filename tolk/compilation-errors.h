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

namespace tolk {

#define tolk_assert(expr) if(UNLIKELY(!(expr))) on_assertion_failed(#expr, __FILE__, __LINE__);

GNU_ATTRIBUTE_COLD GNU_ATTRIBUTE_NORETURN
void on_assertion_failed(const char *description, const char *file_name, int line_number);

// fire a general non-recoverable error, just a wrapper over `throw`
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
void fire(AnyV at, std::string message);
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
void fire(SrcRange range, std::string message);
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
void fire(FunctionPtr in_function, AnyV at, std::string message);
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
void fire(FunctionPtr in_function, SrcRange range, std::string message);

void compilation_warning(AnyV at, const std::string& message);
void compilation_warning(SrcRange range, const std::string& message);
void compilation_warning(FunctionPtr function, AnyV at, const std::string& message);
void compilation_warning(FunctionPtr function, SrcRange range, const std::string& message);

struct Fatal final : std::exception {
  std::string message;

  explicit Fatal(std::string message)
    : message(std::move(message)) {}

  const char* what() const noexcept override {
    return message.c_str();
  }
};

struct ParseError final : std::exception {
  std::string in_function;
  SrcRange range;
  std::string message;

  ParseError(SrcRange range, std::string message)
    : range(range), message(std::move(message)) {}
  ParseError(std::string in_function, SrcRange range, std::string message)
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
