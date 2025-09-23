#include "tolk.h"
#include "pipeline.h"
#include "compiler-state.h"
#include "tolk-version.h"
#include "type-system.h"
#include "td/utils/JsonBuilder.h"
#include <fstream>

namespace tolk {

void pipeline_generate_source_map(std::ostream& debug_out) {
  if (!G.settings.collect_source_map) {
    return;
  }

  td::JsonBuilder root_builder;
  auto root_builder_obj = root_builder.enter_object();

  root_builder_obj("version", "1");
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
        loc_builder("line", static_cast<td::int64>(entry.loc.line));
        loc_builder("col", static_cast<td::int64>(entry.loc.col));
        loc_builder("line_offset", static_cast<td::int64>(entry.loc.line_offset));
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

      ob("vars", td::JsonRaw(var_builder.string_builder().as_cslice()));

      {
        td::JsonBuilder ctxb;
        auto ctx_builder = ctxb.enter_object();

        if (entry.descr.size() != 0) {
          ctx_builder("descr", entry.descr);  // Human-readable description
        }

        if (entry.is_entry) {
          ctx_builder("is_entry", td::JsonBool(entry.is_entry));  // Marks function entry points
        }

        ctx_builder("ast_kind", entry.ast_kind);  // AST node type

        ctx_builder("func_name", entry.func_name);
        if (entry.inlined_to_func_name != "") {
          ctx_builder("inlined_to_func", entry.inlined_to_func_name);
        }

        ctx_builder("func_inline_mode", static_cast<td::int64>(entry.func_inline_mode));

        if (entry.before_inlined_function_call) {
          ctx_builder("before_inlined_function_call", td::JsonBool(entry.before_inlined_function_call));
        }

        if (entry.after_inlined_function_call) {
          ctx_builder("after_inlined_function_call", td::JsonBool(entry.after_inlined_function_call));
        }
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

}  // namespace tolk
