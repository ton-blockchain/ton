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
#include "compiler-settings.h"
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

static constexpr char ANSI_RESET[] = "\x1b[0m";
static constexpr char ANSI_BOLD[] = "\x1b[1m";
static constexpr char ANSI_DIM[] = "\x1b[2m";
static constexpr char ANSI_BOLD_RED[] = "\x1b[1;31m";
static constexpr char ANSI_BOLD_BLUE[] = "\x1b[1;34m";
static constexpr char ANSI_BOLD_MAGENTA[] = "\x1b[1;35m";
static constexpr char ANSI_BOLD_CYAN[] = "\x1b[1;36m";

struct AnsiStyled {
  std::string_view text;
  const char* ansi_style;
};

static std::ostream& operator<<(std::ostream& os, const AnsiStyled& value) {
  if (G_settings.colorize_errors) {
    os << value.ansi_style;
  }
  os << value.text;
  if (G_settings.colorize_errors) {
    os << ANSI_RESET;
  }
  return os;
}

// generate prefix "in function" or "in lambda in function", returning topmost non-lambda
static std::string str_in_function(FunctionPtr& f) {
  if (f->is_lambda()) {
    if (!f->base_fun_ref) {
      return "in lambda ";
    }
    f = f->base_fun_ref;
    return "in lambda " + str_in_function(f);
  }
  return "in function ";
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
  // compiler-only built-ins like `__throw` have no ident_anchor
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
  os << AnsiStyled{"error", ANSI_BOLD_RED}
     << AnsiStyled{": ", ANSI_BOLD};

  // message is:
  // - a single string "can not parse"
  // - a multi-line string "can not parse\n""because it's bad"
  // - a multi-line string with hints: "can not parse\n""hint: first\n""hint: second"
  std::string_view msg_text = message;
  std::string_view hint_text;
  if (size_t hint_pos = message.find("\nhint:"); hint_pos != std::string_view::npos) {
    hint_text = msg_text.substr(hint_pos + 1);
    msg_text = msg_text.substr(0, hint_pos);
  }

  size_t first_eol = msg_text.find('\n');
  // a bold message after "error:"
  os << AnsiStyled{msg_text.substr(0, first_eol), ANSI_BOLD} << "\n";
  // multi-line message: print "(spaces) line2 \n (spaces) line3", non-bold
  size_t start = first_eol == std::string_view::npos ? msg_text.size() : first_eol + 1;
  while (start < msg_text.size()) {
    size_t eol = msg_text.find('\n', start);
    size_t end = eol == std::string_view::npos ? msg_text.size() : eol;
    os << "    " << msg_text.substr(start, end - start) << "\n";
    start = end + 1;
  }

  os << AnsiStyled{"    --> ", ANSI_BOLD_BLUE}
     << range.stringify_start_location(true) << "\n";

  if (in_function) {
    FunctionPtr f_non_lambda = in_function;
    os << AnsiStyled{"     |  ", ANSI_BOLD_BLUE}
       << AnsiStyled{str_in_function(f_non_lambda), ANSI_DIM}
       << AnsiStyled{f_non_lambda->as_human_readable(), ANSI_BOLD_MAGENTA} << "\n";
  }

  range.output_underlined(os,
    G_settings.colorize_errors ? ANSI_BOLD_BLUE : "",
    G_settings.colorize_errors ? ANSI_BOLD_RED : "",
    G_settings.colorize_errors ? ANSI_RESET : "");

  // print hints, probably multi-line (one hint per line), non-bold, don't highlight "hint"
  if (!hint_text.empty()) {
    os << hint_text << "\n";
  }

  for (const ErrorSecondaryLocation& loc : secondary_locations) {
    os << "\n" << AnsiStyled{"note", ANSI_BOLD_CYAN} << ": " << loc.note << "\n"
       << AnsiStyled{"    --> ", ANSI_BOLD_BLUE}
       << loc.range.stringify_start_location(true) << "\n";
    loc.range.output_underlined(os,
      G_settings.colorize_errors ? ANSI_BOLD_BLUE : "",
      G_settings.colorize_errors ? ANSI_BOLD_CYAN : "",
      G_settings.colorize_errors ? ANSI_RESET : "");
  }
}

void ThrownParseError::output_to_json(JsonPrettyOutput& json) const {
  json.start_object();
  json.key_value("message", message);
  if (in_function) {
    FunctionPtr f_non_lambda = in_function;
    std::string in_prefix = str_in_function(f_non_lambda);
    json.key_value("in_function", in_prefix + f_non_lambda->as_human_readable());
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
