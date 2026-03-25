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
#include "source-maps.h"
#include "ast.h"
#include "compiler-state.h"
#include "generics-helpers.h"
#include "type-system.h"
#include "json-output.h"
#include "pack-unpack-serializers.h"

namespace tolk {

// todo a lot of copy-paste from abi.cpp
static bool is_builtin_unexported_struct(StructPtr struct_ref) {
  return struct_ref->name == "Cell" || struct_ref->name == "lisp_list";
}

static bool is_builtin_unexported_alias(AliasDefPtr alias_ref) {
  return alias_ref->name == "RemainingBitsAndRefs";
}

struct SourceMapForOutput {
  struct UniqueType {
    TypePtr t_ptr;
    std::string abi_json;
  };

  std::vector<const Symbol*> used_symbols;    // structs, aliases, enums
  std::vector<UniqueType> used_types;         // not unordered_set, because we need order (get_type_idx)
  std::vector<SrcFilePtr> used_files;
  std::vector<FunctionPtr> used_functions;

  void register_used_type(TypePtr type) {
    if (find_unique_type(type) != -1) {
      return;
    }

    std::string abi_json;
    type->as_abi_json(abi_json);
    used_types.push_back(UniqueType{.t_ptr = type, .abi_json = std::move(abi_json)});

    type->replace_children_custom([this](TypePtr child) {
      if (const TypeDataStruct* t_struct = child->try_as<TypeDataStruct>()) {
        StructPtr symbol = t_struct->struct_ref;
        if (symbol->is_instantiation_of_generic_struct()) {
          symbol = symbol->base_struct_ref;
          for (int i = 0; i < t_struct->struct_ref->substitutedTs->size(); ++i) {
            register_used_type(t_struct->struct_ref->substitutedTs->typeT_at(i));
          }
        }
        if (!is_builtin_unexported_struct(symbol)) {
          for (StructFieldPtr field_ref : symbol->fields) {
            register_used_type(field_ref->declared_type);
          }
          register_used_symbol(symbol);
        }
      } else if (const TypeDataAlias* t_alias = child->try_as<TypeDataAlias>()) {
        AliasDefPtr symbol = t_alias->alias_ref;
        if (symbol->is_instantiation_of_generic_alias()) {
          symbol = symbol->base_alias_ref;
          for (int i = 0; i < t_alias->alias_ref->substitutedTs->size(); ++i) {
            register_used_type(t_alias->alias_ref->substitutedTs->typeT_at(i));
          }
        }
        if (!is_builtin_unexported_alias(symbol)) {
          register_used_type(t_alias->underlying_type);
          register_used_symbol(symbol);
        }
      } else if (const TypeDataEnum* t_enum = child->try_as<TypeDataEnum>()) {
        EnumDefPtr symbol = t_enum->enum_ref;
        register_used_symbol(symbol);
      } else if (const TypeDataGenericTypeWithTs* t_generic = child->try_as<TypeDataGenericTypeWithTs>()) {
        if (t_generic->struct_ref && !is_builtin_unexported_struct(t_generic->struct_ref)) {
          register_used_symbol(t_generic->struct_ref);
        }
        if (t_generic->alias_ref && !is_builtin_unexported_alias(t_generic->alias_ref)) {
          register_used_symbol(t_generic->alias_ref);
        }
      }
      return child;
    });
  }

  void register_used_symbol(const Symbol* symbol) {
    auto it = std::find(used_symbols.begin(), used_symbols.end(), symbol);
    if (it == used_symbols.end()) {
      used_symbols.push_back(symbol);
    }
  }

  void register_used_file(SrcRange range) {
    tolk_assert(range.is_valid());
    SrcFilePtr file_ref = range.get_src_file();
    auto it = std::find(used_files.begin(), used_files.end(), file_ref);
    if (it == used_files.end()) {
      used_files.push_back(file_ref);
    }
  }

  void register_used_function(FunctionPtr fun_ref) {
    tolk_assert(!fun_ref->is_generic_function());
    auto it = std::find(used_functions.begin(), used_functions.end(), fun_ref);
    if (it == used_functions.end()) {
      used_functions.push_back(fun_ref);
      if (fun_ref->is_code_function()) {    // some built-in functions are also registered (if execution fails inside fromCell, we'll see it in call stack)
        register_used_file(fun_ref->ident_anchor->range);
      }
      register_used_type(fun_ref->inferred_return_type);
    }
  }

  int get_fun_idx(FunctionPtr fun_ref) const {
    for (int i = 0; i < static_cast<int>(used_functions.size()); ++i) {
      if (used_functions[i] == fun_ref) {
        return i;
      }
    }
    tolk_assert(false);
  }

  int find_unique_type(TypePtr t) const {
    // step 1: compare pointers, will work for built-in primitives very fast
    for (int i = 0; i < static_cast<int>(used_types.size()); ++i) {
      if (used_types[i].t_ptr == t) {
        return i;
      }
    }
    // step 2: compare abi_json
    std::string t_abi_json;
    t->as_abi_json(t_abi_json);
    for (int i = 0; i < static_cast<int>(used_types.size()); ++i) {
      if (used_types[i].abi_json == t_abi_json) {
        return i;
      }
    }
    // note, that we don't compare via `t->equal_to`, because "equal" is "via abi_json" actually;
    // for example, `type U = A | B`, then `U` and `A | B` are equal_to, but should both be exported
    return -1;
  }

  int get_type_idx(TypePtr t) const {
    int idx = find_unique_type(t);
    tolk_assert(idx != -1);
    return idx;
  }
};

static SrcRange get_function_body_end(FunctionPtr fun_ref) {
  return SrcRange::span_at_end(fun_ref->ast_root->range, 1);      // it's the closing `}`
}

static void to_json(JsonPrettyOutput& out, TypePtr type) {
  std::string type_as_json;
  type->as_abi_json(type_as_json);
  out << type_as_json;
}

static void to_json(JsonPrettyOutput& out, SrcRange range) {
  SrcRange::DecodedRange r = range.decode_offsets();
  out << '['
      << r.file_id << ',' << ' '
      << r.start_line_no << ',' << r.start_char_no << ',' << ' '
      << r.end_line_no << ',' << r.end_char_no
      << ']';
}

static void to_json(JsonPrettyOutput& out, const GenericsDeclaration* genericTs) {
  out << '[';
  out << '"' << genericTs->get_nameT(0) << '"';
  for (int i = 1; i < genericTs->size(); ++i) {
    out << ", " << '"' << genericTs->get_nameT(i) << '"';
  }
  out << ']';
}

static void to_json(JsonPrettyOutput& out, const CustomPackUnpackF& f) {
  out.start_object();
  if (f.f_pack) {
    out.key_value("packToBuilder", true);
  }
  if (f.f_unpack) {
    out.key_value("unpackFromSlice", true);
  }
  out.end_object();
}

static void to_json(JsonPrettyOutput& out, const std::vector<var_idx_t>& ir_idx_arr) {
  out << '[';
  bool first = true;
  for (var_idx_t ir_idx : ir_idx_arr) {
    if (!first) out << ',' << ' ';
    first = false;
    out << ir_idx;
  }
  out << ']';
}

static void to_json(JsonPrettyOutput& out, const DebugMarkCurrentStack& stack) {
  out << '[';
  bool first = true;
  for (DebugMarkCurrentStack::StackSlot slot : stack.stack_slots) {
    if (!first) out << ',' << ' ';
    first = false;
    out << slot.ir_var->ir_idx;
  }
  out << ']';
}


void SourceMapCollecting::to_pretty_json(std::ostream& os) const {
  JsonPrettyOutput json(os);
  json.start_object();

  SourceMapForOutput total;

  total.register_used_type(TypeDataVoid::create());
  total.register_used_type(TypeDataInt::create());
  total.register_used_type(TypeDataSlice::create());
  total.register_used_type(TypeDataCell::create());
  total.register_used_type(TypeDataBuilder::create());
  total.register_used_type(TypeDataBool::create());
  total.register_used_type(TypeDataCoins::create());
  total.register_used_type(TypeDataAddress::internal());
  total.register_used_type(TypeDataIntN::create(32, false, false));
  total.register_used_type(TypeDataIntN::create(32, true, false));
  total.register_used_type(TypeDataIntN::create(64, false, false));
  total.register_used_type(TypeDataIntN::create(64, true, false));
  
  for (const DebugMarkInfo& mark : debug_marks) {
    if (const DebugMarkEnterFunction* m_enter = std::get_if<DebugMarkEnterFunction>(&mark)) {
      total.register_used_function(m_enter->fun_ref);
    } else if (const DebugMarkLocalVar* m_local = std::get_if<DebugMarkLocalVar>(&mark)) {
      total.register_used_type(m_local->local_ref->declared_type);
    } else if (const DebugMarkSmartCast* m_sc = std::get_if<DebugMarkSmartCast>(&mark)) {
      total.register_used_type(m_sc->smart_cast_type);
    } else if (const DebugMarkSetGlob* m_sg = std::get_if<DebugMarkSetGlob>(&mark)) {
      total.register_used_type(m_sg->glob_ref->declared_type);
    }
  }

  int idx = 0;
  
  // todo very a lot of duplicates with abi.cpp
  json.start_array("files");
  for (SrcFilePtr src_file : total.used_files) {
    json.start_object();
    json.key_value("file_id", src_file->file_id);
    json.key_value("file_name", src_file->realpath);
    json.key_value("size_chars", src_file->text.size());
    json.end_object();
  }
  json.end_array();

  json.start_array("declarations");
  for (const Symbol* symbol : total.used_symbols) {
    json.start_object();
    if (StructPtr struct_ref = symbol->try_as<StructPtr>()) {
      json.key_value("kind", "struct");
      json.key_value("name", struct_ref->name);
      json.key_value("ident_loc", struct_ref->ident_anchor->range);
      if (struct_ref->is_generic_struct()) {
        json.key_value("type_params", struct_ref->genericTs);
      }
      if (struct_ref->opcode.exists()) {
        json.start_object("prefix");
        json.key_value("prefixStr", struct_ref->opcode.format_as_string(false));
        json.key_value("prefixLen", struct_ref->opcode.prefix_len);
        json.end_object();
      }
      json.start_array("fields");
      for (StructFieldPtr field_ref : struct_ref->fields) {
        json.start_object();
        json.key_value("name", field_ref->name);
        json.key_value("ty", field_ref->declared_type);
        json.end_object();
      }
      json.end_array();
      if (CustomPackUnpackF f = get_custom_pack_unpack_function(TypeDataStruct::create(struct_ref))) {
        json.key_value("customPackUnpack", f);
      }
    } else if (AliasDefPtr alias_ref = symbol->try_as<AliasDefPtr>()) {
      json.key_value("kind", "alias");
      json.key_value("name", alias_ref->name);
      json.key_value("ident_loc", alias_ref->ident_anchor->range);
      json.key_value("target_ty", alias_ref->underlying_type);
      if (alias_ref->is_generic_alias()) {
        json.key_value("type_params", alias_ref->genericTs);
      }
      if (CustomPackUnpackF f = get_custom_pack_unpack_function(TypeDataAlias::create(alias_ref))) {
        json.key_value("customPackUnpack", f);
      }
    } else if (EnumDefPtr enum_ref = symbol->try_as<EnumDefPtr>()) {
      json.key_value("kind", "enum");
      json.key_value("name", enum_ref->name);
      json.key_value("ident_loc", enum_ref->ident_anchor->range);
      json.key_value("encoded_as", calculate_intN_to_serialize_enum(enum_ref));
      json.start_array("members");
      for (EnumMemberPtr member_ref : enum_ref->members) {
        json.start_object();
        json.key_value("name", member_ref->name);
        json.key_value("value", member_ref->computed_value);
        json.end_object();
      }
      json.end_array();
      if (CustomPackUnpackF f = get_custom_pack_unpack_function(TypeDataEnum::create(enum_ref))) {
        json.key_value("customPackUnpack", f);
      }
    } else {
      tolk_assert(false);   // only top-level declarations were added to used symbols
    }
    json.end_object();
  }
  json.end_array();

  idx = 0;
  json.start_array("unique_ty");
  for (const SourceMapForOutput::UniqueType& t : total.used_types) {
    json.start_object();
    json.key_value("ty_idx", idx++);
    json.key_value("ty", JsonPrettyOutput::Unquoted{t.abi_json});
    json.end_object();
  }
  json.end_array();
  
  idx = 0;
  json.start_array("functions");
  for (FunctionPtr fun_ref : total.used_functions) {
    json.start_object();
    json.key_value("f_idx", idx++);
    json.key_value("name", fun_ref->name);
    json.key_value("return_ty_idx", total.get_type_idx(fun_ref->inferred_return_type));
    json.key_value("num_params", fun_ref->get_num_params());
    if (fun_ref->is_code_function()) {
      json.key_value("ident_loc", fun_ref->ident_anchor->range);
      json.key_value("end_loc", get_function_body_end(fun_ref));
    } else {
      json.key_value("ident_loc", JsonPrettyOutput::Unquoted{"[0,0,0,0,0]"});
      json.key_value("end_loc", JsonPrettyOutput::Unquoted{"[0,0,0,0,0]"});
    }
    json.end_object();
  }
  json.end_array();

  idx = 0;
  json.start_array("debug_marks");
  for (const DebugMarkInfo& mark : debug_marks) {
    json.start_object();
    json.key_value("mark_id", idx++);   // both in Fift and in JSON they start from 0
    if (const DebugMarkLocation* m_loc = std::get_if<DebugMarkLocation>(&mark)) {
      json.key_value("kind", "loc");
      json.key_value("range", m_loc->range);
    } else if (const DebugMarkCurrentStack* m_stack = std::get_if<DebugMarkCurrentStack>(&mark)) {
      json.key_value("kind", "stack");
      json.key_value("stack", *m_stack);
    } else if (const DebugMarkEnterFunction* m_enter = std::get_if<DebugMarkEnterFunction>(&mark)) {
      json.key_value("kind", "enter_fun");
      json.key_value("f_idx", total.get_fun_idx(m_enter->fun_ref));
      json.key_value("f_name", m_enter->fun_ref->name);
      json.key_value("is_inlined", m_enter->is_inlined);
      json.key_value("is_builtin", m_enter->is_builtin);
      json.key_value("range", m_enter->range);
      json.key_value("ir_import", m_enter->ir_import);
    } else if (const DebugMarkLeaveFunction* m_leave = std::get_if<DebugMarkLeaveFunction>(&mark)) {
      json.key_value("kind", "leave_fun");
      json.key_value("f_idx", total.get_fun_idx(m_leave->fun_ref));
      json.key_value("f_name", m_leave->fun_ref->name);
      json.key_value("ir_return", m_leave->ir_return);
      json.key_value("range", m_leave->range);
    } else if (const DebugMarkLocalVar* m_local = std::get_if<DebugMarkLocalVar>(&mark)) {
      json.key_value("kind", "var");
      json.key_value("var_name", m_local->local_ref->name);
      json.key_value("is_parameter", m_local->local_ref->is_parameter());
      json.key_value("ty_idx", total.get_type_idx(m_local->local_ref->declared_type));
      json.key_value("ir_slots", m_local->ir_slots);
      if (m_local->is_lazy) {
        json.key_value("is_lazy", true);
      }
    } else if (const DebugMarkScopeStart* m_scope = std::get_if<DebugMarkScopeStart>(&mark)) {
      json.key_value("kind", "scope_start");
      json.key_value("range", m_scope->range);
    } else if (std::get_if<DebugMarkScopeEnd>(&mark)) {
      json.key_value("kind", "scope_end");
    } else if (const DebugMarkSmartCast* m_sc = std::get_if<DebugMarkSmartCast>(&mark)) {
      json.key_value("kind", "smart_cast");
      json.key_value("var_name", m_sc->local_ref->name);
      json.key_value("ty_idx", total.get_type_idx(m_sc->smart_cast_type));
      json.key_value("ir_slots", m_sc->ir_slots);
    } else if (const DebugMarkSetGlob* m_sg = std::get_if<DebugMarkSetGlob>(&mark)) {
      json.key_value("kind", "set_glob");
      json.key_value("glob_name", m_sg->glob_ref->name);
      json.key_value("ty_idx", total.get_type_idx(m_sg->glob_ref->declared_type));
      json.key_value("ir_slots", m_sg->ir_slots);
    } else {
      tolk_assert(false);
    }
    json.end_object();
  }
  json.end_array();

  json.end_object();
}

void pipeline_collect_source_maps_output(std::ostream& os) {
  G.source_map.to_pretty_json(os);
}

} // namespace tolk
