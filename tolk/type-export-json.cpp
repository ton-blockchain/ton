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
#include "type-export-json.h"
#include "ast.h"
#include "generics-helpers.h"
#include "pack-unpack-serializers.h"
#include "constant-evaluator.h"

namespace tolk {

static bool is_builtin_unexported_struct(StructPtr struct_ref) {
  return struct_ref->name == "Cell" || struct_ref->name == "lisp_list";
}

static bool is_builtin_unexported_alias(AliasDefPtr alias_ref) {
  return alias_ref->name == "RemainingBitsAndRefs";
}

// --------------------------------------------
//    as_abi_json()
//
// Serializes TypeData subtypes into a compact JSON string.
// Used both for JSON export and as a key for type deduplication.

struct Escaped {
  std::string_view str;

  explicit Escaped(std::string_view str): str(str) {}
};

static void operator+=(std::string& out, Escaped to_be_escaped) {
  for (char c : to_be_escaped.str) {
    if (c == '"')       { out += '\\'; out += c; }
    else if (c == '\n') { out += "\\\\n"; }
    else out += c;
  }
}

static void operator+=(std::string& out, TypePtr nested) {
  nested->as_abi_json(out);
}

static void operator+=(std::string& out, const std::vector<TypePtr>& arr) {
  bool first = true;
  for (TypePtr item : arr) {
    if (!first) out += ",";
    first = false;
    item->as_abi_json(out);
  }
}

static void operator+=(std::string& out, const GenericsSubstitutions* substitutedTs) {
  for (int i = 0; i < substitutedTs->size(); ++i) {
    if (i) out += ",";
    substitutedTs->typeT_at(i)->as_abi_json(out);
  }
}

std::vector<StructData::PackOpcode> auto_generate_opcodes_for_union(TypePtr union_type, std::string& because_msg, bool& tree_auto_generated);

void TypeDataAlias::as_abi_json(std::string& out) const {
  if (alias_ref->name == "RemainingBitsAndRefs") {
    out += R"({"kind":"remaining"})";
    return;
  }
  out += R"({"kind":"AliasRef","alias_name":")";
  if (!alias_ref->is_instantiation_of_generic_alias()) {
    out += Escaped(alias_ref->name);
    out += '"';
  } else {
    out += Escaped(alias_ref->base_alias_ref->name);
    out += R"(","type_args":[)";
    out += alias_ref->substitutedTs;
    out += ']';
  }
  out += '}';
}

void TypeDataInt::as_abi_json(std::string& out) const {
  out += R"({"kind":"int"})";
}

void TypeDataBool::as_abi_json(std::string& out) const {
  out += R"({"kind":"bool"})";
}

void TypeDataCell::as_abi_json(std::string& out) const {
  out += R"({"kind":"cell"})";
}

void TypeDataSlice::as_abi_json(std::string& out) const {
  out += R"({"kind":"slice"})";
}

void TypeDataBuilder::as_abi_json(std::string& out) const {
  out += R"({"kind":"builder"})";
}

void TypeDataContinuation::as_abi_json(std::string& out) const {
  out += R"({"kind":"callable"})";
}

void TypeDataString::as_abi_json(std::string& out) const {
  out += R"({"kind":"string"})";
}

void TypeDataAddress::as_abi_json(std::string& out) const {
  out += is_internal() ? R"({"kind":"address"})" : R"({"kind":"addressAny"})";
}

void TypeDataArray::as_abi_json(std::string& out) const {
  out += R"({"kind":"arrayOf","inner":)";
  out += innerT;
  out += '}';
}

void TypeDataShapedTuple::as_abi_json(std::string& out) const {
  out += R"({"kind":"shapedTuple","items":[)";
  out += items;
  out += "]}";
}

void TypeDataNullLiteral::as_abi_json(std::string& out) const {
  out += R"({"kind":"nullLiteral"})";
}

void TypeDataFunCallable::as_abi_json(std::string& out) const {
  out += R"({"kind":"callable"})";
}

void TypeDataGenericT::as_abi_json(std::string& out) const {
  out += R"({"kind":"genericT","name_t":")";
  out += nameT;
  out += "\"}";
}

void TypeDataGenericTypeWithTs::as_abi_json(std::string& out) const {
  if (struct_ref && struct_ref->name == "Cell") {
    out += R"({"kind":"cellOf","inner":)";
    out += type_arguments;
    out += '}';
    return;
  }
  if (struct_ref && struct_ref->name == "lisp_list") {
    out += R"({"kind":"lispListOf","inner":)";
    out += type_arguments;
    out += '}';
    return;
  }

  if (alias_ref) {
    out += R"({"kind":"AliasRef","alias_name":")";
    out += Escaped(alias_ref->name);
  } else {
    out += R"({"kind":"StructRef","struct_name":")";
    out += Escaped(struct_ref->name);
  }
  out += R"(","type_args":[)";
  out += type_arguments;
  out += "]}";
}

void TypeDataStruct::as_abi_json(std::string& out) const {
  if (struct_ref->is_instantiation_of_CellT()) {
    out += R"({"kind":"cellOf","inner":)";
    out += struct_ref->substitutedTs->typeT_at(0);
    out += '}';
    return;
  }
  if (struct_ref->is_instantiation_of_LispListT()) {
    out += R"({"kind":"lispListOf","inner":)";
    out += struct_ref->substitutedTs->typeT_at(0);
    out += '}';
    return;
  }

  out += R"({"kind":"StructRef","struct_name":")";
  if (!struct_ref->is_instantiation_of_generic_struct()) {
    out += Escaped(struct_ref->name);
    out += '"';
  } else {
    out += Escaped(struct_ref->base_struct_ref->name);
    out += R"(","type_args":[)";
    out += struct_ref->substitutedTs;
    out += ']';
  }
  out += '}';
}

void TypeDataEnum::as_abi_json(std::string& out) const {
  out += R"({"kind":"EnumRef","enum_name":")";
  out += Escaped(enum_ref->name);
  out += "\"}";
}

void TypeDataTensor::as_abi_json(std::string& out) const {
  out += R"({"kind":"tensor","items":[)";
  out += items;
  out += "]}";
}

void TypeDataIntN::as_abi_json(std::string& out) const {
  out += is_variadic
    ? is_unsigned ? R"({"kind":"varuintN","n":)" : R"({"kind":"varintN","n":)"
    : is_unsigned ? R"({"kind":"uintN","n":)" : R"({"kind":"intN","n":)";
  out += std::to_string(n_bits);
  out += '}';
}

void TypeDataCoins::as_abi_json(std::string& out) const {
  out += R"({"kind":"coins"})";
}

void TypeDataBitsN::as_abi_json(std::string& out) const {
  out += R"({"kind":"bitsN","n":)";
  out += std::to_string(is_bits ? n_width : n_width * 8);
  out += '}';
}

void TypeDataUnion::as_abi_json(std::string& out) const {
  if (or_null && or_null->unwrap_alias()->try_as<TypeDataAddress>() && or_null->unwrap_alias()->try_as<TypeDataAddress>()->is_internal()) {
    out += R"({"kind":"addressOpt"})";  // for `AddressAlias?` we also emit `address?`, not a nullable alias
    return;
  }
  if (or_null) {
    out += R"({"kind":"nullable","inner":)";
    out += or_null;
    if (!has_genericT_inside() && !is_primitive_nullable()) {
      out += R"(,"stack_type_id":)";
      out += std::to_string(or_null->get_type_id());
      out += R"(,"stack_width":)";
      out += std::to_string(get_width_on_stack());
    }
    out += '}';
    return;
  }

  std::string err_msg;
  bool tree_auto_generated;
  std::vector<StructData::PackOpcode> opcodes = auto_generate_opcodes_for_union(this, err_msg, tree_auto_generated);
  tolk_assert(size() == static_cast<int>(opcodes.size()));

  out += R"({"kind":"union","variants":[)";
  for (int i = 0; i < size(); ++i) {
    if (i != 0) out += ',';
    out += R"({"variant_ty":)";
    out += variants[i];
    out += R"(,"prefix_str":")";
    out += opcodes[i].format_as_string(false);
    out += R"(","prefix_len":)";
    out += std::to_string(opcodes[i].prefix_len);
    if (tree_auto_generated) {
      out += R"(,"is_prefix_implicit":true)";
    }
    if (!has_genericT_inside()) {
      out += R"(,"stack_type_id":)";
      out += std::to_string(variants[i]->get_type_id());
      out += R"(,"stack_width":)";
      out += std::to_string(variants[i]->get_width_on_stack());
    }
    out += '}';
  }
  out += ']';
  if (!has_genericT_inside()) {
    out += R"(,"stack_width":)";
    out += std::to_string(get_width_on_stack());
  }
  out += '}';
}

void TypeDataMapKV::as_abi_json(std::string& out) const {
  out += R"({"kind":"mapKV","k":)";
  out += TKey;
  out += R"(,"v":)";
  out += TValue;
  out += '}';
}

void TypeDataUnknown::as_abi_json(std::string& out) const {
  out += R"({"kind":"unknown"})";
}

void TypeDataNotInferred::as_abi_json(std::string& out) const {
  tolk_assert(false);
}

void TypeDataNever::as_abi_json(std::string& out) const {
  out += R"({"kind":"void"})";
}

void TypeDataVoid::as_abi_json(std::string& out) const {
  out += R"({"kind":"void"})";
}


// --------------------------------------------
//    JsonTypeExporter
//

void JsonTypeExporter::register_used_type(TypePtr type) {
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

void JsonTypeExporter::register_used_symbol(const Symbol* symbol) {
  auto it = std::find(used_symbols.begin(), used_symbols.end(), symbol);
  if (it == used_symbols.end()) {
    used_symbols.push_back(symbol);
  }
}

int JsonTypeExporter::find_unique_type(TypePtr t) const {
  for (int i = 0; i < static_cast<int>(used_types.size()); ++i) {
    if (used_types[i].t_ptr == t) {
      return i;
    }
  }
  std::string t_abi_json;
  t->as_abi_json(t_abi_json);
  for (int i = 0; i < static_cast<int>(used_types.size()); ++i) {
    if (used_types[i].abi_json == t_abi_json) {
      return i;
    }
  }
  return -1;
}

int JsonTypeExporter::get_type_idx(TypePtr t) const {
  int idx = find_unique_type(t);
  tolk_assert(idx != -1);
  return idx;
}

void JsonTypeExporter::seed_primitive_types() {
  register_used_type(TypeDataVoid::create());
  register_used_type(TypeDataInt::create());
  register_used_type(TypeDataSlice::create());
  register_used_type(TypeDataCell::create());
  register_used_type(TypeDataBuilder::create());
  register_used_type(TypeDataBool::create());
  register_used_type(TypeDataCoins::create());
  register_used_type(TypeDataAddress::internal());
  register_used_type(TypeDataIntN::create(32, false, false));
  register_used_type(TypeDataIntN::create(32, true, false));
  register_used_type(TypeDataIntN::create(64, false, false));
  register_used_type(TypeDataIntN::create(64, true, false));
}

// ---- shared to_json overloads ----

void to_json(JsonPrettyOutput& out, TypePtr type) {
  std::string type_as_json;
  type->as_abi_json(type_as_json);
  out << type_as_json;
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
    out.key_value("pack_to_builder", true);
  }
  if (f.f_unpack) {
    out.key_value("unpack_from_slice", true);
  }
  out.end_object();
}

void to_json(JsonPrettyOutput& out, const ConstValExpression& v) {
  out.start_object();
  if (const auto* i = std::get_if<ConstValInt>(&v)) {
    out.key_value("kind", "int");
    out.key_value("v", i->int_val);
  } else if (const auto* b = std::get_if<ConstValBool>(&v)) {
    out.key_value("kind", "bool");
    out.key_value("v", b->bool_val);
  } else if (const auto* s = std::get_if<ConstValSlice>(&v)) {
    out.key_value("kind", "slice");
    out.key_value("hex", s->str_hex);
  } else if (const auto* q = std::get_if<ConstValString>(&v)) {
    out.key_value("kind", "string");
    out.key_value("str", q->str_val);
  } else if (const auto* a = std::get_if<ConstValAddress>(&v)) {
    out.key_value("kind", "address");
    out.key_value("addr", a->orig_str);
  } else if (const auto* t = std::get_if<ConstValTensor>(&v)) {
    out.key_value("kind", "tensor");
    out.key_value("items", t->items);
  } else if (const auto* h = std::get_if<ConstValShapedTuple>(&v)) {
    out.key_value("kind", "shapedTuple");
    out.key_value("items", h->items);
  } else if (const auto* o = std::get_if<ConstValObject>(&v)) {
    out.key_value("kind", "object");
    out.key_value("struct_name", o->struct_ref->name);
    out.key_value("fields", o->fields);
  } else if (const auto* c = std::get_if<ConstValCastToType>(&v)) {
    out.key_value("kind", "castTo");
    out.key_value("inner", c->inner.front());
    out.key_value("cast_to", c->cast_to);
  } else if (std::holds_alternative<ConstValNullLiteral>(v)) {
    out.key_value("kind", "null");
  } else {
    tolk_assert(false);
  }
  out.end_object();
}

static void to_json(JsonPrettyOutput& out, const std::vector<ConstValExpression>& v) {
  out.start_array();
  for (const ConstValExpression& v_item : v) {
    out.write_value(v_item);
  }
  out.end_array();
}

// ---- shared declarations serialization ----

std::string get_abi_description(const DocCommentLines& doc_lines) {
  std::string result;
  for (std::string_view line : doc_lines) {
    if (line.starts_with("@")) {    // description is before the first @tag in a doc comment
      break;
    }
    if (!result.empty()) {
      result += '\n';
    }
    result += line;
  }
  return result;
}

static void to_json(JsonPrettyOutput& out, SrcRange range) {
  SrcRange::DecodedRange r = range.decode_offsets();
  out << '['
      << r.file_id << ',' << ' '
      << r.start_line_no << ',' << r.start_char_no << ',' << ' '
      << r.end_line_no << ',' << r.end_char_no
      << ']';
}

void JsonTypeExporter::emit_declarations_json(JsonPrettyOutput& json, const EmitOptions& opts) const {
  json.start_array("declarations");
  for (const Symbol* symbol : used_symbols) {
    json.start_object();
    if (StructPtr struct_ref = symbol->try_as<StructPtr>()) {
      json.key_value("kind", "struct");
      json.key_value("name", struct_ref->name);
      if (opts.emit_ident_loc) {
        json.key_value("ident_loc", struct_ref->ident_anchor->range);
      }
      if (struct_ref->is_generic_struct()) {
        json.key_value("type_params", struct_ref->genericTs);
      }
      if (struct_ref->opcode.exists()) {
        json.start_object("prefix");
        json.key_value("prefix_str", struct_ref->opcode.format_as_string(false));
        json.key_value("prefix_len", struct_ref->opcode.prefix_len);
        json.end_object();
      }
      json.start_array("fields");
      for (StructFieldPtr field_ref : struct_ref->fields) {
        json.start_object();
        json.key_value("name", field_ref->name);
        json.key_value("ty", field_ref->declared_type);
        if (opts.emit_default_values && field_ref->default_value) {
          json.key_value("default_value", eval_field_default_value(field_ref));
        }
        if (opts.emit_descriptions && !field_ref->doc_lines.empty()) {
          json.key_value("description", get_abi_description(field_ref->doc_lines));
        }
        json.end_object();
      }
      json.end_array();
      if (CustomPackUnpackF f = get_custom_pack_unpack_function(TypeDataStruct::create(struct_ref))) {
        json.key_value("custom_pack_unpack", f);
      }
    } else if (AliasDefPtr alias_ref = symbol->try_as<AliasDefPtr>()) {
      json.key_value("kind", "alias");
      json.key_value("name", alias_ref->name);
      if (opts.emit_ident_loc) {
        json.key_value("ident_loc", alias_ref->ident_anchor->range);
      }
      json.key_value("target_ty", alias_ref->underlying_type);
      if (alias_ref->is_generic_alias()) {
        json.key_value("type_params", alias_ref->genericTs);
      }
      if (CustomPackUnpackF f = get_custom_pack_unpack_function(TypeDataAlias::create(alias_ref))) {
        json.key_value("custom_pack_unpack", f);
      }
    } else if (EnumDefPtr enum_ref = symbol->try_as<EnumDefPtr>()) {
      json.key_value("kind", "enum");
      json.key_value("name", enum_ref->name);
      if (opts.emit_ident_loc) {
        json.key_value("ident_loc", enum_ref->ident_anchor->range);
      }
      json.key_value("encoded_as", calculate_intN_to_serialize_enum(enum_ref));
      json.start_array("members");
      for (EnumMemberPtr member_ref : enum_ref->members) {
        json.start_object();
        json.key_value("name", member_ref->name);
        json.key_value("value", member_ref->computed_value);
        if (opts.emit_descriptions && !member_ref->doc_lines.empty()) {
          json.key_value("description", get_abi_description(member_ref->doc_lines));
        }
        json.end_object();
      }
      json.end_array();
      if (CustomPackUnpackF f = get_custom_pack_unpack_function(TypeDataEnum::create(enum_ref))) {
        json.key_value("custom_pack_unpack", f);
      }
    } else {
      tolk_assert(false);
    }
    json.end_object();
  }
  json.end_array();
}

void JsonTypeExporter::emit_unique_ty_json(JsonPrettyOutput& json) const {
  int idx = 0;
  json.start_array("unique_ty");
  for (const UniqueType& t : used_types) {
    json.start_object();
    json.key_value("ty_idx", idx++);
    json.key_value("ty", JsonPrettyOutput::Unquoted{t.abi_json});
    json.end_object();
  }
  json.end_array();
}

} // namespace tolk
