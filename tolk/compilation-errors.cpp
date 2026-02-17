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
#include "type-system.h"

namespace tolk {

static std::string str_in_function(FunctionPtr f) {
  if (f == nullptr) {
    return "";
  }
  if (f->is_lambda()) {
    return "in lambda " + str_in_function(f->base_fun_ref);
  }
  return "in function `" + f->as_human_readable() + "`"; 
}

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
    os << std::endl << "    // " << in_function << std::endl;
  }
  range.output_underlined(os);
}

void ErrorBuilder::push(const char* v) {
  add_arg(v);
}

void ErrorBuilder::push(const std::string& v) {
  add_arg(v);
}

void ErrorBuilder::push(std::string_view v) {
  add_arg(static_cast<std::string>(v));
}

void ErrorBuilder::push(TypePtr v) {
  add_arg(v->as_human_readable());
}

void ErrorBuilder::push(FunctionPtr v) {
  add_arg(v->as_human_readable());
}

void ErrorBuilder::push(StructPtr v) {
  add_arg(v->as_human_readable());
}

void ErrorBuilder::push(StructFieldPtr v) {
  add_arg(v->name);
}

void ErrorBuilder::push(AliasDefPtr v) {
  add_arg(v->as_human_readable());
}

void ErrorBuilder::push(EnumDefPtr v) {
  add_arg(v->as_human_readable());
}

void ErrorBuilder::push(EnumMemberPtr v) {
  add_arg(v->name);
}

void ErrorBuilder::push(GlobalConstPtr v) {
  add_arg(v->name);
}

void ErrorBuilder::push(LocalVarPtr v) {
  add_arg(v->name);
}

void ErrorBuilder::push(int v) {
  add_arg(std::to_string(v));
}

void ErrorBuilder::push(size_t v) {
  add_arg(std::to_string(v));
}

void ErrorBuilder::push(bool v) {
  add_arg(v ? "true" : "false");
}

Error ErrorBuilder::build() const {
  std::string replaced = tpl;
  size_t arg_i = 0, pos;
  while ((pos = replaced.find("{}")) != std::string::npos) {
    if (arg_i >= args.size()) {
#ifdef TOLK_DEBUG
      throw Fatal(std::string("mismatch err() tpl: ") + tpl);
#endif
      break;
    }
    replaced.replace(pos, 2, args[arg_i++]);
  }
#ifdef TOLK_DEBUG
  if (arg_i != args.size()) {
    throw Fatal(std::string("mismatch err() tpl: ") + tpl);
  }
#endif
  return Error(std::move(replaced));
}

void Error::fire(AnyV at, FunctionPtr in_function) const {
  throw ThrownParseError(str_in_function(in_function), at->range, message);  
}

void Error::fire(SrcRange range, FunctionPtr in_function) const {
  throw ThrownParseError(str_in_function(in_function), range, message);  
}

void Error::warning(AnyV at, FunctionPtr in_function) const {
  output_compiler_message(std::cerr, true, str_in_function(in_function), at->range, message);
}

void Error::warning(SrcRange range, FunctionPtr in_function) const {
  output_compiler_message(std::cerr, true, str_in_function(in_function), range, message);
}

void ThrownParseError::output_compilation_error(std::ostream& os) const {
  output_compiler_message(os, false, in_function, range, message);
}

} // namespace tolk
