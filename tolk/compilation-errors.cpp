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
#include "compilation-errors.h"
#include "ast.h"

namespace tolk {

void on_assertion_failed(const char *description, const char *file_name, int line_number) {
  std::string message = static_cast<std::string>("Assertion failed at ") + file_name + ":" + std::to_string(line_number) + ": " + description;
#ifdef TOLK_DEBUG
#ifdef __arm64__
  // when developing, it's handy when the debugger stops on assertion failure (stacktraces and watches are available)
  std::cerr << message << std::endl;
  __builtin_debugtrap();
#endif
#endif
  throw Fatal(std::move(message));
}

GNU_ATTRIBUTE_NOINLINE
static void output_compiler_message(
  std::ostream& os,
  bool is_warning,
  std::string_view in_function,
  SrcRange range,
  std::string_view message
  ) {
  std::string loc_text = range.stringify_start_location(true);
  os << loc_text << ": " << (is_warning ? "warning: " : "error: ");

  if (message.find('\n') == std::string::npos) {
    // just print a single-line message after "error:"
    os << message << std::endl;
  } else {
    // print "location: line1 \n (spaces) line2 \n ..."
    std::string loc_spaces(std::min(static_cast<int>(loc_text.size()), 9), ' ');
    size_t start = 0, end;
    while ((end = message.find('\n', start)) != std::string::npos) {
      if (start > 0) {
        os << loc_spaces << "  ";
      }
      os << message.substr(start, end - start) << std::endl;
      start = end + 1;
    }
    if (start < message.size()) {
      os << loc_spaces << "  " << message.substr(start) << std::endl;
    }
  }
  if (!in_function.empty()) {
    os << std::endl << "    // in function `" << in_function << "`" << std::endl;
  }
  range.output_underlined(os);
}


void fire(AnyV at, std::string message) {
  throw ParseError(at->range, std::move(message));
}

void fire(SrcRange range, std::string message) {
  throw ParseError(range, std::move(message));
}

void fire(FunctionPtr in_function, AnyV at, std::string message) {
  std::string f = in_function ? in_function->as_human_readable() : "";
  throw ParseError(f, at->range, std::move(message));
}

void fire(FunctionPtr in_function, SrcRange range, std::string message) {
  std::string f = in_function ? in_function->as_human_readable() : "";
  throw ParseError(f, range, std::move(message));
}

void ParseError::output_compilation_error(std::ostream& os) const {
  output_compiler_message(os, false, in_function, range, message);
}


void compilation_warning(AnyV at, const std::string& message) {
  output_compiler_message(std::cerr, true, "", at->range, message);
}

void compilation_warning(SrcRange range, const std::string& message) {
  output_compiler_message(std::cerr, true, "", range, message);
}

void compilation_warning(FunctionPtr in_function, AnyV at, const std::string& message) {
  std::string f = in_function ? in_function->as_human_readable() : "";
  output_compiler_message(std::cerr, true, f, at->range, message);
}

void compilation_warning(FunctionPtr in_function, SrcRange range, const std::string& message) {
  std::string f = in_function ? in_function->as_human_readable() : "";
  output_compiler_message(std::cerr, true, f, range, message);
}


} // namespace tolk
