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
#include <algorithm>

namespace tolk {

static bool is_builtin_unexported_struct(StructPtr struct_ref) {
  return (struct_ref->is_generic_struct() && (struct_ref->name == "Cell" || struct_ref->name == "lisp_list"))
       || struct_ref->is_instantiation_of_CellT() || struct_ref->is_instantiation_of_LispListT();
}

static bool is_builtin_unexported_alias(AliasDefPtr alias_ref) {
  return alias_ref->name == "RemainingBitsAndRefs";
}

// --------------------------------------------
//    as_abi_json()
//
// Serializes TypeData subtypes into a compact JSON string.
// All nested types are referenced by `ty_idx` (registered via the registry on the fly).
// Used both for JSON export and as a key for type deduplication.

struct Escaped {
  std::string_view str;

  explicit Escaped(std::string_view str): str(str) {}
};

static void operator+=(std::string& out, Escaped to_be_escaped) {
  for (char c : to_be_escaped.str) {
    if (c == '"')       { out += '\\'; out += c; }
    else if (c == '\\') { out += "\\\\"; }
    else if (c == '\n') { out += "\\\\n"; }
    else out += c;
  }
}

static void append_type_idx(std::string& out, TypePtr nested, JsonTypeExporter& registry) {
  out += std::to_string(registry.register_used_type(nested));
}

static void append_type_idx_array(std::string& out, const std::vector<TypePtr>& arr, JsonTypeExporter& registry) {
  bool first = true;
  for (TypePtr item : arr) {
    if (!first) out += ",";
    first = false;
    append_type_idx(out, item, registry);
  }
}

static void append_type_idx_array(std::string& out, const GenericsSubstitutions* substitutedTs, JsonTypeExporter& registry) {
  for (int i = 0; i < substitutedTs->size(); ++i) {
    if (i) out += ",";
    append_type_idx(out, substitutedTs->typeT_at(i), registry);
  }
}

std::vector<StructData::PackOpcode> auto_generate_opcodes_for_union(TypePtr union_type, std::string& because_msg, bool& tree_auto_generated);

void TypeDataAlias::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  if (alias_ref->name == "RemainingBitsAndRefs") {
    out += R"({"kind":"remaining"})";
    return;
  }
  out += R"({"kind":"AliasRef","alias_name":")";
  if (!alias_ref->is_instantiation_of_generic_alias()) {
    out += Escaped(alias_ref->name);
    out += "\"}";
  } else {
    out += Escaped(alias_ref->base_alias_ref->name);
    out += R"(","type_args_ty_idx":[)";
    append_type_idx_array(out, alias_ref->substitutedTs, registry);
    out += "]}";
    // monomorphic_target_ty_idx is recorded into the registry's
    // alias_instantiations table by register_used_type after the head is pushed
  }
}

void TypeDataInt::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"int"})";
}

void TypeDataBool::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"bool"})";
}

void TypeDataCell::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"cell"})";
}

void TypeDataSlice::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"slice"})";
}

void TypeDataBuilder::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"builder"})";
}

void TypeDataContinuation::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"callable"})";
}

void TypeDataString::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"string"})";
}

void TypeDataAddress::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += is_internal() ? R"({"kind":"address"})" : R"({"kind":"addressAny"})";
}

void TypeDataArray::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"arrayOf","inner_ty_idx":)";
  append_type_idx(out, innerT, registry);
  out += '}';
}

void TypeDataShapedTuple::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"shapedTuple","items_ty_idx":[)";
  append_type_idx_array(out, items, registry);
  out += "]}";
}

void TypeDataNullLiteral::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"nullLiteral"})";
}

void TypeDataFunCallable::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"callable"})";
}

void TypeDataGenericT::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"genericT","name_t":")";
  out += nameT;
  out += "\"}";
}

void TypeDataGenericTypeWithTs::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  if (struct_ref && struct_ref->name == "Cell") {
    out += R"({"kind":"cellOf","inner_ty_idx":)";
    append_type_idx(out, type_arguments[0], registry);
    out += '}';
    return;
  }
  if (struct_ref && struct_ref->name == "lisp_list") {
    out += R"({"kind":"lispListOf","inner_ty_idx":)";
    append_type_idx(out, type_arguments[0], registry);
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
  out += R"(","type_args_ty_idx":[)";
  append_type_idx_array(out, type_arguments, registry);
  out += "]}";
}

void TypeDataStruct::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  if (struct_ref->is_instantiation_of_CellT()) {
    out += R"({"kind":"cellOf","inner_ty_idx":)";
    append_type_idx(out, struct_ref->substitutedTs->typeT_at(0), registry);
    out += '}';
    return;
  }
  if (struct_ref->is_instantiation_of_LispListT()) {
    out += R"({"kind":"lispListOf","inner_ty_idx":)";
    append_type_idx(out, struct_ref->substitutedTs->typeT_at(0), registry);
    out += '}';
    return;
  }

  out += R"({"kind":"StructRef","struct_name":")";
  if (!struct_ref->is_instantiation_of_generic_struct()) {
    out += Escaped(struct_ref->name);
    out += "\"}";
  } else {
    out += Escaped(struct_ref->base_struct_ref->name);
    out += R"(","type_args_ty_idx":[)";
    append_type_idx_array(out, struct_ref->substitutedTs, registry);
    out += "]}";
    // monomorphic_fields are recorded into the registry's
    // struct_instantiations table by register_used_type after the head is pushed
  }
}

void TypeDataEnum::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"EnumRef","enum_name":")";
  out += Escaped(enum_ref->name);
  out += "\"}";
}

void TypeDataTensor::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"tensor","items_ty_idx":[)";
  append_type_idx_array(out, items, registry);
  out += "]}";
}

void TypeDataIntN::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += is_variadic
    ? is_unsigned ? R"({"kind":"varuintN","n":)" : R"({"kind":"varintN","n":)"
    : is_unsigned ? R"({"kind":"uintN","n":)" : R"({"kind":"intN","n":)";
  out += std::to_string(n_bits);
  out += '}';
}

void TypeDataCoins::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"coins"})";
}

void TypeDataBitsN::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"bitsN","n":)";
  out += std::to_string(is_bits ? n_width : n_width * 8);
  out += '}';
}

void TypeDataUnion::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  if (or_null && or_null->unwrap_alias()->try_as<TypeDataAddress>() && or_null->unwrap_alias()->try_as<TypeDataAddress>()->is_internal()) {
    // for `AddressAlias?` we also emit `address?`, unless it has custom serializers
    if (!get_custom_pack_unpack_function(or_null)) {
      out += R"({"kind":"addressOpt"})";
      return;
    }
  }
  if (or_null) {
    out += R"({"kind":"nullable","inner_ty_idx":)";
    append_type_idx(out, or_null, registry);
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
    out += R"({"variant_ty_idx":)";
    append_type_idx(out, variants[i], registry);
    out += R"(,"prefix_num":)";
    out += std::to_string(opcodes[i].pack_prefix);
    out += R"(,"prefix_len":)";
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

void TypeDataMapKV::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"mapKV","key_ty_idx":)";
  append_type_idx(out, TKey, registry);
  out += R"(,"value_ty_idx":)";
  append_type_idx(out, TValue, registry);
  out += '}';
}

void TypeDataUnknown::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"unknown"})";
}

void TypeDataNotInferred::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  tolk_assert(false);
}

void TypeDataNever::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"void"})";
}

void TypeDataVoid::as_abi_json(std::string& out, JsonTypeExporter& registry) const {
  out += R"({"kind":"void"})";
}


// --------------------------------------------
//    JsonTypeExporter
//


// given struct `Wrapper<T>`, construct TypePtr `Wrapper<T>`
static TypePtr construct_generic_struct_type(StructPtr struct_decl) {
  std::vector<TypePtr> type_arguments;
  type_arguments.reserve(struct_decl->genericTs->size());
  for (int i = 0; i < struct_decl->genericTs->size(); ++i) {
    type_arguments.push_back(TypeDataGenericT::create(std::string(struct_decl->genericTs->get_nameT(i))));
  }
  return TypeDataGenericTypeWithTs::create(struct_decl, nullptr, std::move(type_arguments));
}

// given alias `Either<L, R>`, construct TypePtr `Either<L, R>`
static TypePtr construct_generic_alias_type(AliasDefPtr alias_decl) {
  std::vector<TypePtr> type_arguments;
  type_arguments.reserve(alias_decl->genericTs->size());
  for (int i = 0; i < alias_decl->genericTs->size(); ++i) {
    type_arguments.push_back(TypeDataGenericT::create(std::string(alias_decl->genericTs->get_nameT(i))));
  }
  return TypeDataGenericTypeWithTs::create(nullptr, alias_decl, std::move(type_arguments));
}

// Walks the declaration's fields/target/members and registers them. Cannot be
// done inside as_abi_json: that runs BEFORE the type is in used_types, so for
// self-referential types like `struct Node { next: Node? }` walking the fields
// would loop back through register_used_type → as_abi_json → fields → ...
// Calling this AFTER the push allows recursive register_used_type to work.
static void register_referenced_declarations(JsonTypeExporter& reg, TypePtr type) {
  StructPtr struct_decl = nullptr;
  AliasDefPtr alias_decl = nullptr;
  EnumDefPtr enum_decl = nullptr;

  if (const auto* t_struct = type->try_as<TypeDataStruct>()) {
    struct_decl = t_struct->struct_ref->is_instantiation_of_generic_struct()
                  ? t_struct->struct_ref->base_struct_ref
                  : t_struct->struct_ref;
  } else if (const auto* t_alias = type->try_as<TypeDataAlias>()) {
    alias_decl = t_alias->alias_ref->is_instantiation_of_generic_alias()
                 ? t_alias->alias_ref->base_alias_ref
                 : t_alias->alias_ref;
  } else if (const auto* t_enum = type->try_as<TypeDataEnum>()) {
    enum_decl = t_enum->enum_ref;
  } else if (const auto* t_generic = type->try_as<TypeDataGenericTypeWithTs>()) {
    struct_decl = t_generic->struct_ref;
    alias_decl = t_generic->alias_ref;
  }

  if (struct_decl && !is_builtin_unexported_struct(struct_decl) && reg.register_used_symbol(struct_decl)) {
    if (struct_decl->is_generic_struct()) {
      reg.register_used_type(construct_generic_struct_type(struct_decl));
    }
    for (StructFieldPtr field_ref : struct_decl->fields) {
      reg.register_used_type(field_ref->declared_type);
      if (field_ref->abi_client_type) {
        reg.register_used_type(field_ref->abi_client_type);
      }
      if (field_ref->default_value && !field_ref->abi_client_type && !struct_decl->is_generic_struct()) {
        reg.register_used_const_val(eval_field_default_value(field_ref));
      }
    }
  }
  if (alias_decl && !is_builtin_unexported_alias(alias_decl) && reg.register_used_symbol(alias_decl)) {
    if (alias_decl->is_generic_alias()) {
      reg.register_used_type(construct_generic_alias_type(alias_decl));
    }
    reg.register_used_type(alias_decl->underlying_type);
  }
  if (enum_decl && reg.register_used_symbol(enum_decl)) {
    reg.register_used_type(calculate_intN_to_serialize_enum(enum_decl));
  }
}

int JsonTypeExporter::register_used_type(TypePtr type) {
  int ty_idx = find_unique_type(type);
  if (ty_idx != -1) {
    return ty_idx;
  }

  std::string abi_json;
  type->as_abi_json(abi_json, *this);

  // reserve the slot; subsequent recursive register_used_type calls will hit it
  ty_idx = static_cast<int>(used_types.size());
  used_types.push_back(UniqueType{.t_ptr = type, .abi_json = std::move(abi_json)});

  // for generic instantiations: walk the INSTANTIATED struct/alias (NOT the base) and
  // record resolved field/target type indices into the instantiation tables
  if (const auto* t_struct = type->try_as<TypeDataStruct>(); t_struct &&
      t_struct->struct_ref->is_instantiation_of_generic_struct() &&
      !is_builtin_unexported_struct(t_struct->struct_ref->base_struct_ref)) {
    std::vector<int> mono_fields_ty_idx;
    mono_fields_ty_idx.reserve(t_struct->struct_ref->fields.size());
    for (StructFieldPtr field_ref : t_struct->struct_ref->fields) {
      mono_fields_ty_idx.push_back(register_used_type(field_ref->declared_type));
    }
    struct_instantiations.push_back(StructInstantiation{
      .ty_idx = ty_idx,
      .struct_ref = t_struct->struct_ref,
      .monomorphic_fields_ty_idx = std::move(mono_fields_ty_idx),
    });
  } else if (const auto* t_alias = type->try_as<TypeDataAlias>(); t_alias &&
             t_alias->alias_ref->is_instantiation_of_generic_alias() &&
             !is_builtin_unexported_alias(t_alias->alias_ref->base_alias_ref)) {
    int target_ty_idx = register_used_type(t_alias->alias_ref->underlying_type);
    alias_instantiations.push_back(AliasInstantiation{
      .ty_idx = ty_idx,
      .alias_ref = t_alias->alias_ref,
      .monomorphic_target_ty_idx = target_ty_idx,
    });
  }

  register_referenced_declarations(*this, type);
  return ty_idx;
}

bool JsonTypeExporter::register_used_symbol(const Symbol* symbol) {
  if (std::find(used_symbols.begin(), used_symbols.end(), symbol) != used_symbols.end()) {
    return false;
  }
  used_symbols.push_back(symbol);
  return true;
}

// ensure that `cast_to_ty_idx` will exist in a registry
void JsonTypeExporter::register_used_const_val(const ConstValExpression& v) {
  if (const auto* t = std::get_if<ConstValTensor>(&v)) {
    for (const ConstValExpression& item : t->items) {
      register_used_const_val(item);
    }
  } else if (const auto* h = std::get_if<ConstValShapedTuple>(&v)) {
    for (const ConstValExpression& item : h->items) {
      register_used_const_val(item);
    }
  } else if (const auto* o = std::get_if<ConstValObject>(&v)) {
    register_used_type(TypeDataStruct::create(o->struct_ref));
    for (const ConstValExpression& field : o->fields) {
      register_used_const_val(field);
    }
  } else if (const auto* c = std::get_if<ConstValCastToType>(&v)) {
    register_used_type(c->cast_to);
    for (const ConstValExpression& inner : c->inner) {
      register_used_const_val(inner);
    }
  }
}

int JsonTypeExporter::find_unique_type(TypePtr t) const {
  // two-level dedup: at first, fast path for primitives (singletons like int/bool/void)
  for (int ty_idx = 0; ty_idx < static_cast<int>(used_types.size()); ++ty_idx) {
    if (used_types[ty_idx].t_ptr == t) {
      return ty_idx;
    }
  }

  // level 2: canonical equality by JSON content; the serialized JSON is the canonical
  // identity of a type for ABI purposes (unique_types), so it's the real dedup key
  std::string abi_json;
  t->as_abi_json(abi_json, *const_cast<JsonTypeExporter*>(this));

  for (int ty_idx = 0; ty_idx < static_cast<int>(used_types.size()); ++ty_idx) {
    if (used_types[ty_idx].abi_json == abi_json) {
      return ty_idx;
    }
  }
  return -1;
}

int JsonTypeExporter::get_type_idx(TypePtr t) const {
  int ty_idx = find_unique_type(t);
  tolk_assert(ty_idx != -1);
  return ty_idx;
}

// Seeds primitives so that universally-used types (int, void, intN, ...) get small
// stable ty_idx values 0..N-1 in every output, regardless of registration order.
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

static void to_json(JsonPrettyOutput& json, const GenericsDeclaration* genericTs) {
  json.start_array();
  for (int i = 0; i < genericTs->size(); ++i) {
    json.next_array_item();
    json.write_value(genericTs->get_nameT(i));
  }
  json.end_array();
}

static void to_json(JsonPrettyOutput& json, const CustomPackUnpackF& f) {
  json.start_object();
  if (f.f_pack) {
    json.key_value("pack_to_builder", true);
  }
  if (f.f_unpack) {
    json.key_value("unpack_from_slice", true);
  }
  json.end_object();
}

void to_json(JsonPrettyOutput& json, const JsonTypeExporter::ConstValJson& v) {
  const ConstValExpression& expr = v.value;
  json.start_object();
  if (const auto* i = std::get_if<ConstValInt>(&expr)) {
    json.key_value("kind", "int");
    json.key_value("v", i->int_val);
  } else if (const auto* b = std::get_if<ConstValBool>(&expr)) {
    json.key_value("kind", "bool");
    json.key_value("v", b->bool_val);
  } else if (const auto* s = std::get_if<ConstValSlice>(&expr)) {
    json.key_value("kind", "slice");
    json.key_value("hex", s->str_hex);
  } else if (const auto* q = std::get_if<ConstValString>(&expr)) {
    json.key_value("kind", "string");
    json.key_value("str", q->str_val);
  } else if (const auto* a = std::get_if<ConstValAddress>(&expr)) {
    json.key_value("kind", "address");
    json.key_value("addr", a->orig_str);
  } else if (const auto* t = std::get_if<ConstValTensor>(&expr)) {
    json.key_value("kind", "tensor");
    json.start_array("items");
    for (const ConstValExpression& item : t->items) {
      json.next_array_item();
      json.write_value(v.registry.const_val_json(item));
    }
    json.end_array();
  } else if (const auto* h = std::get_if<ConstValShapedTuple>(&expr)) {
    json.key_value("kind", "shapedTuple");
    json.start_array("items");
    for (const ConstValExpression& item : h->items) {
      json.next_array_item();
      json.write_value(v.registry.const_val_json(item));
    }
    json.end_array();
  } else if (const auto* o = std::get_if<ConstValObject>(&expr)) {
    json.key_value("kind", "object");
    json.key_value("struct_name", o->struct_ref->name);
    json.start_array("fields");
    for (const ConstValExpression& field : o->fields) {
      json.next_array_item();
      json.write_value(v.registry.const_val_json(field));
    }
    json.end_array();
  } else if (const auto* c = std::get_if<ConstValCastToType>(&expr)) {
    json.key_value("kind", "castTo");
    json.key_value("inner", v.registry.const_val_json(c->inner.front()));
    json.key_value("cast_to_ty_idx", v.registry.get_type_idx(c->cast_to));
  } else if (std::holds_alternative<ConstValNullLiteral>(expr)) {
    json.key_value("kind", "null");
  } else {
    tolk_assert(false);
  }
  json.end_object();
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

static std::string get_type_description(TypePtr ty) {
  if (const TypeDataAlias* t_alias = ty->try_as<TypeDataAlias>()) {
    if (!t_alias->alias_ref->doc_lines.empty() && !is_builtin_unexported_alias(t_alias->alias_ref)) {
      return get_abi_description(t_alias->alias_ref->doc_lines);
    }
    return get_type_description(t_alias->underlying_type);
  }
  if (const TypeDataStruct* t_struct = ty->try_as<TypeDataStruct>(); t_struct && !is_builtin_unexported_struct(t_struct->struct_ref)) {
    return get_abi_description(t_struct->struct_ref->doc_lines);
  }
  if (const TypeDataEnum* t_enum = ty->try_as<TypeDataEnum>()) {
    return get_abi_description(t_enum->enum_ref->doc_lines);
  }
  return {};
}

static void to_json(JsonPrettyOutput& json, SrcRange range) {
  SrcRange::DecodedRange r = range.decode_offsets();
  json << '['
       << r.file_id << ',' << ' '
       << r.start_line_no << ',' << r.start_char_no << ',' << ' '
       << r.end_line_no << ',' << r.end_char_no
       << ']';
}

// Writes all type-related top-level arrays in canonical order: unique_types, generic instantiations, and declarations.
// Both out.abi.json and out.symbolTypes.json need exactly the same set of arrays (differ only in EmitOptions).
void JsonTypeExporter::emit_unique_ty_and_declarations_json(JsonPrettyOutput& json, const EmitOptions& opts) const {
  json.start_array("unique_types");
  for (const UniqueType& ty : used_types) {
    json.next_array_item();
    json.write_value(JsonPrettyOutput::Unquoted{ty.abi_json});
  }
  json.end_array();

  json.start_array("struct_instantiations");
  for (const StructInstantiation& s : struct_instantiations) {
    json.next_array_item();
    json.start_object();
    json.key_value("ty_idx", s.ty_idx);
    json.key_value("struct_name", s.struct_ref->name);
    json.start_array("monomorphic_fields_ty_idx");
    for (int idx : s.monomorphic_fields_ty_idx) {
      json.next_array_item();
      json.write_value(idx);
    }
    json.end_array();
    if (CustomPackUnpackF f = get_custom_pack_unpack_function(TypeDataStruct::create(s.struct_ref))) {
      json.key_value("custom_pack_unpack", f);
    }
    json.end_object();
  }
  json.end_array();

  json.start_array("alias_instantiations");
  for (const AliasInstantiation& a : alias_instantiations) {
    json.next_array_item();
    json.start_object();
    json.key_value("ty_idx", a.ty_idx);
    json.key_value("alias_name", a.alias_ref->name);
    json.key_value("monomorphic_target_ty_idx", a.monomorphic_target_ty_idx);
    if (CustomPackUnpackF f = get_custom_pack_unpack_function(TypeDataAlias::create(a.alias_ref))) {
      json.key_value("custom_pack_unpack", f);
    }
    json.end_object();
  }
  json.end_array();

  // emit declarations in source order so the JSON mirrors how a developer reads the file(s)
  std::vector<const Symbol*> sorted_symbols = used_symbols;
  std::sort(sorted_symbols.begin(), sorted_symbols.end(), [](const Symbol* a, const Symbol* b) {
    return a->ident_anchor->range < b->ident_anchor->range;
  });

  json.start_array("declarations");
  for (const Symbol* symbol : sorted_symbols) {
    json.next_array_item();
    json.start_object();
    if (StructPtr struct_ref = symbol->try_as<StructPtr>()) {
      TypePtr ty = struct_ref->is_generic_struct() ? construct_generic_struct_type(struct_ref) : TypeDataStruct::create(struct_ref);
      json.key_value("kind", "struct");
      json.key_value("name", struct_ref->name);
      json.key_value("ty_idx", get_type_idx(ty));
      if (opts.emit_ident_loc) {
        json.key_value("ident_loc", struct_ref->ident_anchor->range);
      }
      if (struct_ref->is_generic_struct()) {
        json.key_value("type_params", struct_ref->genericTs);
      }
      if (struct_ref->opcode.exists()) {
        json.start_object("prefix");
        json.key_value("prefix_num", struct_ref->opcode.pack_prefix);
        json.key_value("prefix_len", struct_ref->opcode.prefix_len);
        json.end_object();
      }
      json.start_array("fields");
      for (StructFieldPtr field_ref : struct_ref->fields) {
        json.next_array_item();
        json.start_object();
        json.key_value("name", field_ref->name);
        json.key_value("ty_idx", get_type_idx(field_ref->declared_type));
        if (opts.emit_abi_client_types && field_ref->abi_client_type) {
          json.key_value("client_ty_idx", get_type_idx(field_ref->abi_client_type));
        }
        if (opts.emit_default_values && field_ref->default_value && !field_ref->abi_client_type && !struct_ref->is_generic_struct()) {
          ConstValExpression default_value = eval_field_default_value(field_ref);
          json.key_value("default_value", const_val_json(default_value));
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
      if (opts.emit_descriptions && !struct_ref->doc_lines.empty()) {
        json.key_value("description", get_abi_description(struct_ref->doc_lines));
      }
    } else if (AliasDefPtr alias_ref = symbol->try_as<AliasDefPtr>()) {
      TypePtr ty = alias_ref->is_generic_alias() ? construct_generic_alias_type(alias_ref) : TypeDataAlias::create(alias_ref);
      std::string description = get_type_description(TypeDataAlias::create(alias_ref));   // if doc_comment is absent, take from underlying_type
      json.key_value("kind", "alias");
      json.key_value("name", alias_ref->name);
      json.key_value("ty_idx", get_type_idx(ty));
      if (opts.emit_ident_loc) {
        json.key_value("ident_loc", alias_ref->ident_anchor->range);
      }
      json.key_value("target_ty_idx", get_type_idx(alias_ref->underlying_type));
      if (alias_ref->is_generic_alias()) {
        json.key_value("type_params", alias_ref->genericTs);
      }
      if (CustomPackUnpackF f = get_custom_pack_unpack_function(TypeDataAlias::create(alias_ref))) {
        json.key_value("custom_pack_unpack", f);
      }
      if (opts.emit_descriptions && !description.empty()) {
        json.key_value("description", description);
      }
    } else if (EnumDefPtr enum_ref = symbol->try_as<EnumDefPtr>()) {
      TypePtr ty = TypeDataEnum::create(enum_ref);
      json.key_value("kind", "enum");
      json.key_value("name", enum_ref->name);
      json.key_value("ty_idx", get_type_idx(ty));
      if (opts.emit_ident_loc) {
        json.key_value("ident_loc", enum_ref->ident_anchor->range);
      }
      json.key_value("encoded_as_ty_idx", get_type_idx(calculate_intN_to_serialize_enum(enum_ref)));
      json.start_array("members");
      for (EnumMemberPtr member_ref : enum_ref->members) {
        json.next_array_item();
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
      if (opts.emit_descriptions && !enum_ref->doc_lines.empty()) {
        json.key_value("description", get_abi_description(enum_ref->doc_lines));
      }
    } else {
      tolk_assert(false);
    }
    json.end_object();
  }
  json.end_array();
}

} // namespace tolk
