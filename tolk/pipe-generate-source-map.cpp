#include "tolk.h"
#include "pipeline.h"
#include "compiler-state.h"
#include "tolk-version.h"
#include "type-system.h"
#include "td/utils/JsonBuilder.h"
#include <fstream>

namespace tolk {

static std::string extract_assert_condition(const std::string& assert_str);

void pipeline_generate_source_map(std::ostream& debug_out) {
  if (!G.settings.collect_source_map) {
    return;
  }

  td::JsonBuilder root_builder;
  auto root_builder_obj = root_builder.enter_object();

  root_builder_obj("version", "1.0.0");
  root_builder_obj("language", "tolk");
  root_builder_obj("compiler_version", TOLK_VERSION);

  {
    td::JsonBuilder jsonb;
    auto array_builder = jsonb.enter_array();
    for (const auto& file : G.all_src_files) {
      auto value_builder = array_builder.enter_value();
      auto ob = value_builder.enter_object();

      ob("path", file->realpath);
      ob("is_stdlib", td::JsonBool(file->is_stdlib_file));
      ob("content", file->text);
    }
    array_builder.leave();

    root_builder_obj("files", td::JsonRaw(jsonb.string_builder().as_cslice()));
  }

  {
    td::JsonBuilder jsonb;
    auto array_builder = jsonb.enter_array();
    for (const auto& glob_var : G.all_global_vars) {
      auto value_builder = array_builder.enter_value();
      auto ob = value_builder.enter_object();

      ob("name", glob_var->name);
      ob("type", glob_var->declared_type->as_human_readable());

      const auto global_loc = glob_var->loc;
      if (const SrcFile* src_file = global_loc.get_src_file(); src_file != nullptr) {
        const auto pos = src_file->convert_offset(global_loc.get_char_offset());

        td::JsonBuilder locb;
        auto loc_builder = locb.enter_object();
        loc_builder("file", src_file->realpath);
        loc_builder("line", static_cast<td::int64>(pos.line_no - 1));
        loc_builder("column", static_cast<td::int64>(pos.char_no - 1));
        loc_builder("end_line", static_cast<td::int64>(0));
        loc_builder("end_column", static_cast<td::int64>(0));
        loc_builder("length", static_cast<td::int64>(1));
        loc_builder.leave();

        ob("loc", td::JsonRaw(locb.string_builder().as_cslice()));
      }
    }
    array_builder.leave();

    root_builder_obj("globals", td::JsonRaw(jsonb.string_builder().as_cslice()));
  }

  {
    td::JsonBuilder jsonb;
    auto array_builder = jsonb.enter_array();

    for (size_t i = 0; i < G.source_map.size(); ++i) {
      const auto& entry = G.source_map[i];
      auto value_builder = array_builder.enter_value();
      auto ob = value_builder.enter_object();

      ob("idx", td::JsonRaw(std::to_string(entry.idx)));

#ifdef TOLK_DEBUG
      {
        td::JsonBuilder debugb;
        auto debug_builder = debugb.enter_object();
        if (i + 1 < G.source_map.size()) {
          debug_builder("opcode", G.source_map[i + 1].opcode);
        }

        if (const auto file = G.all_src_files.find_file(entry.loc.file)) {
          const auto& pos = file->convert_offset(entry.loc.offset);
          const auto line = std::string(pos.line_str);
          debug_builder("line_str", line);

          std::string underline = "";
          for (int j = 0; j < entry.loc.col; ++j) {
            underline += " ";
          }
          underline += "^";

          debug_builder("line_off", underline);
        }
        debug_builder.leave();

        ob("debug", td::JsonRaw(debugb.string_builder().as_cslice()));
      }
#endif

      {
        td::JsonBuilder locb;
        auto loc_builder = locb.enter_object();
        loc_builder("file", entry.loc.file);
        loc_builder("line", static_cast<td::int64>(entry.loc.line - 1));
        loc_builder("column", static_cast<td::int64>(entry.loc.col - 1));
        loc_builder("end_line", static_cast<td::int64>(0));
        loc_builder("end_column", static_cast<td::int64>(0));
        loc_builder("length", static_cast<td::int64>(entry.loc.length));
        loc_builder.leave();

        ob("loc", td::JsonRaw(locb.string_builder().as_cslice()));
      }

      td::JsonBuilder var_builder;
      auto var_array_builder = var_builder.enter_array();
      for (const auto& [var, value] : entry.vars) {
        auto var_array_builder_value = var_array_builder.enter_value();
        auto var_array_value_object = var_array_builder_value.enter_object();

        var_array_value_object("name", var.name.empty() ? "'" + std::to_string(var.ir_idx) : var.name);
        var_array_value_object("type", var.v_type == nullptr ? "" : var.v_type->as_human_readable());

        if (!var.name.empty() && (var.name[0] == '\'' || var.name == "lazyS")) {
          // '1 or lazyS
          var_array_value_object("is_temporary", td::JsonBool(true));
        }

        if (var.parent_type != nullptr) {
          const auto union_parent = var.parent_type->try_as<TypeDataUnion>();
          if (union_parent != nullptr) {
            td::JsonBuilder parent_type_builder;
            auto parent_type_array_builder = parent_type_builder.enter_array();

            for (auto variant : union_parent->variants) {
              auto array_value = parent_type_array_builder.enter_value();
              array_value << variant->as_human_readable();
            }

            parent_type_array_builder.leave();
            var_array_value_object("possible_qualifier_types",
                                   td::JsonRaw(parent_type_builder.string_builder().as_cslice()));
          }
        }

        if (!value.empty()) {
          var_array_value_object("constant_value", value);
        }
      }
      var_array_builder.leave();

      ob("variables", td::JsonRaw(var_builder.string_builder().as_cslice()));

      {
        td::JsonBuilder ctxb;
        auto ctx_builder = ctxb.enter_object();

        {
          td::JsonBuilder descb;
          auto desc_builder = descb.enter_object();
          desc_builder("ast_kind", entry.ast_kind);

          if (entry.ast_kind == "ast_function_call" && entry.is_assert_throw && !entry.descr.empty()) {
            std::string condition = extract_assert_condition(entry.descr);
            desc_builder("condition", condition);
            desc_builder("is_assert_throw", td::JsonBool(true));
          } else if (entry.ast_kind == "ast_binary_operator" && !entry.descr.empty()) {
            desc_builder("description", entry.descr);
          }

          desc_builder.leave();
          ctx_builder("description", td::JsonRaw(descb.string_builder().as_cslice()));
        }

        {
          td::JsonBuilder inlb;
          auto inl_builder = inlb.enter_object();
          if (entry.inlined_to_func_name != "") {
            inl_builder("inlined_to_func", entry.inlined_to_func_name);
          }
          inl_builder("containing_func_inline_mode", static_cast<td::int64>(entry.func_inline_mode));
          inl_builder.leave();
          ctx_builder("inlining", td::JsonRaw(inlb.string_builder().as_cslice()));
        }

        std::string event_type;
        if (entry.is_entry) {
          event_type = "EnterFunction";
        } else if (entry.is_leave) {
          event_type = "LeaveFunction";
        } else if (entry.before_inlined_function_call) {
          event_type = "EnterInlinedFunction";
        } else if (entry.after_inlined_function_call) {
          event_type = "LeaveInlinedFunction";
        }

        if (event_type != "") {
          ctx_builder("event", event_type);
        }

        ctx_builder("containing_function", entry.func_name);

        ctx_builder.leave();

        ob("context", td::JsonRaw(ctxb.string_builder().as_cslice()));
      }
    }
    array_builder.leave();

    root_builder_obj("locations", td::JsonRaw(jsonb.string_builder().as_cslice()));
  }

  root_builder_obj.leave();

  debug_out << root_builder.string_builder().as_cslice().str();
}

/// Extracts condition from assert statement string
/// Examples:
/// "assert (a > 10) throw 5" -> "a > 10"
/// "assert(a > 10, 5)" -> "a > 10"
static std::string extract_assert_condition(const std::string& assert_str) {
  if (assert_str.empty()) {
    return "";
  }

  std::string s = assert_str;

  size_t start = s.find_first_not_of(" \t");
  if (start != std::string::npos) {
    s = s.substr(start);
  } else {
    return "";
  }

  if (s.substr(0, 6) == "assert") {
    s = s.substr(6);
  }

  start = s.find_first_not_of(" \t");
  if (start != std::string::npos) {
    s = s.substr(start);
  } else {
    return "";
  }

  if (!s.empty() && s[0] == '(') {
    // Format: assert (condition) throw/error
    if (size_t paren_end = s.rfind(')'); paren_end != std::string::npos) {
      std::string condition = s.substr(1, paren_end - 1);
      const size_t cond_start = condition.find_first_not_of(" \t");
      size_t cond_end = condition.find_last_not_of(" \t");
      if (cond_start != std::string::npos && cond_end != std::string::npos) {
        return condition.substr(cond_start, cond_end - cond_start + 1);
      }
      return condition;
    }
  } else if (!s.empty()) {
    // Format: assert(condition, error) or assert condition throw/error
    size_t comma_pos = s.rfind(',');
    size_t throw_pos = s.find(" throw");
    size_t error_pos = s.find(" error");

    size_t end_pos = std::string::npos;
    if (comma_pos != std::string::npos) {
      end_pos = comma_pos;
    } else if (throw_pos != std::string::npos) {
      end_pos = throw_pos;
    } else if (error_pos != std::string::npos) {
      end_pos = error_pos;
    }

    if (end_pos != std::string::npos) {
      std::string condition = s.substr(0, end_pos);
      size_t cond_end = condition.find_last_not_of(" \t");
      if (cond_end != std::string::npos) {
        return condition.substr(0, cond_end + 1);
      }
      return condition;
    }
  }

  return s;
}

}  // namespace tolk
