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
#include "compiler-state.h"
#include "json-output.h"

/**
 *   There are two ways to generate a compilation error:
 *  1) err("...").fire() — throw an error immediately and interrupt compilation, it's [[noreturn]]
 *  2) err("...").collect() — store errors in a buffer, all collected will be thrown before AST->IR stage
 *
 *   Be very careful of using collect()! Bulk errors are tricky. Double-check that no forward logic
 * relies on this. For example, collecting errors in a type checker (after type inferring) is safe,
 * but when resolving symbols, if a symbol not found, we can do only fire() — otherwise, nullptr is left.
 *
 *   Optional related locations can be attached with `with_secondary()` before fire()/collect().
 *
 *   Note: the compiler emits only errors, not warnings (warnings are the goal of a linter).
 */

namespace tolk {

static std::string str_in_function(FunctionPtr f) {
  if (f->is_lambda()) {
    return f->base_fun_ref ? "in lambda " + str_in_function(f->base_fun_ref) : "in lambda";
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

std::string ErrorBuilder::build_string() const {
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
  return replaced;
}


Error& Error::with_secondary(AnyV at, std::string note) {
  // `at` may be nullptr for `fun_ref->return_type_node` and other optional;
  // then just don't add a secondary location, only a primary will be shown
  if (at) {
    secondary_locations.push_back(ErrorSecondaryLocation{at->range, std::move(note)});
  }
  return *this;
}

Error& Error::with_secondary(const Symbol* at_sym, std::string note) {
  // ident_anchor may be nullptr in built-in symbols todo rework built-ins
  if (at_sym->ident_anchor) {
    secondary_locations.push_back(ErrorSecondaryLocation{at_sym->ident_anchor->range, std::move(note)});
  }
  return *this;
}

Error& Error::with_secondary(SrcRange range, std::string note) {
  secondary_locations.push_back(ErrorSecondaryLocation{range, std::move(note)});
  return *this;
}

void Error::fire(AnyV at, FunctionPtr in_function) {
  throw ThrownParseError(in_function, at->range, std::move(message), std::move(secondary_locations));
}

void Error::fire(const Symbol* at_sym, FunctionPtr in_function) {
  throw ThrownParseError(in_function, at_sym->ident_anchor->range, std::move(message), std::move(secondary_locations));
}

void Error::fire(SrcRange range, FunctionPtr in_function) {
  throw ThrownParseError(in_function, range, std::move(message), std::move(secondary_locations));
}

void Error::collect(AnyV at, FunctionPtr in_function) {
  tolk_assert(G.error_collector);
  G.error_collector->add(ThrownParseError(in_function, at->range, std::move(message), std::move(secondary_locations)));
}

void Error::collect(const Symbol* at_sym, FunctionPtr in_function) {
  tolk_assert(G.error_collector);
  G.error_collector->add(ThrownParseError(in_function, at_sym->ident_anchor->range, std::move(message), std::move(secondary_locations)));
}

void Error::collect(SrcRange range, FunctionPtr in_function) {
  tolk_assert(G.error_collector);
  G.error_collector->add(ThrownParseError(in_function, range, std::move(message), std::move(secondary_locations)));
}

struct JsonErrorRange {
  SrcRange range;
};

static void to_json(JsonPrettyOutput& json, JsonErrorRange v) {
  SrcRange::DecodedRange decoded = v.range.decode_offsets();
  json.start_object();
  json.key_value("file_name", v.range.get_src_file()->realpath);
  json.key_value("start_line_no", decoded.start_line_no);
  json.key_value("start_char_no", decoded.start_char_no);
  json.key_value("end_line_no", decoded.end_line_no);
  json.key_value("end_char_no", decoded.end_char_no);
  json.key_value("text_inside", decoded.text_inside);
  json.end_object();
}

void ThrownParseError::output_to_console(std::ostream& os) const {
  std::string loc_text = range.stringify_start_location(true);
  os << loc_text << ": error: ";

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
  if (in_function) {
    os << std::endl << "    // " << str_in_function(in_function) << std::endl;
  }
  range.output_underlined(os);

  for (const ErrorSecondaryLocation& loc : secondary_locations) {
    os << std::endl;
    os << loc.range.stringify_start_location(true) << ": note: " << loc.note << std::endl;
    loc.range.output_underlined(os);
  }
}

void ThrownParseError::output_to_json(JsonPrettyOutput& json) const {
  json.start_object();
  json.key_value("message", message);
  if (in_function) {
    json.key_value("in_function", str_in_function(in_function));
  }
  if (range.is_valid()) {
    json.key_value("range", JsonErrorRange{range});
  }
  if (!secondary_locations.empty()) {
    json.start_array("secondary_locations");
    for (const ErrorSecondaryLocation& loc : secondary_locations) {
      json.next_array_item();
      json.start_object();
      json.key_value("note", loc.note);
      json.key_value("range", JsonErrorRange{loc.range});
      json.end_object();
    }
    json.end_array();
  }
  json.end_object();
}

} // namespace tolk
