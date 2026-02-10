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
#include "abi.h"
#include "ast.h"
#include "compilation-errors.h"
#include "constant-evaluator.h"
#include "json-output.h"
#include "type-system.h"
#include "generics-helpers.h"
#include "pack-unpack-serializers.h"
#include "tolk-version.h"

namespace tolk {

static const char* ABI_SCHEMA_VERSION = "1.0";

static const AbiAnnotationForSymbol* get_abi_annotation_above(TypePtr ty) {
  if (const TypeDataStruct* t_struct = ty->unwrap_alias()->try_as<TypeDataStruct>()) {
    return t_struct->struct_ref->abi_annotation;
  }
  if (const TypeDataEnum* t_enum = ty->unwrap_alias()->try_as<TypeDataEnum>()) {
    return t_enum->enum_ref->abi_annotation;
  }
  return nullptr;
}

static std::string_view get_abi_description(const AbiAnnotationForSymbol* abi_annotation) {
  if (abi_annotation && abi_annotation->description) {
    if (auto v_str = abi_annotation->description->try_as<ast_string_const>()) {
      return v_str->str_val;
    }
  }
  return "";
}

static std::string_view get_abi_description(TypePtr ty) {
  return get_abi_description(get_abi_annotation_above(ty));
}

static TypePtr normalize_createMessageTy(TypePtr bodyTy) {
  if (const TypeDataStruct* t_struct = bodyTy->try_as<TypeDataStruct>()) {
    if (t_struct->struct_ref->is_instantiation_of_CellT() || t_struct->struct_ref->is_instantiation_of_UnsafeBodyNoRef()) {
      bodyTy = t_struct->struct_ref->substitutedTs->typeT_at(0);
    }
  }
  return bodyTy;
}

static bool is_builtin_unexported_struct(StructPtr struct_ref) {
  return struct_ref->name == "Cell" || struct_ref->name == "lisp_list";
}

static bool is_builtin_unexported_alias(AliasDefPtr alias_ref) {
  return alias_ref->name == "RemainingBitsAndRefs";
}

void ContractABI::register_used_type(TypePtr type) {
  if (used_types.contains(type)) {
    return;
  }

  used_types.insert(type);
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

void ContractABI::register_used_symbol(const Symbol* symbol) {
  auto it = std::find(used_symbols.begin(), used_symbols.end(), symbol);
  if (it == used_symbols.end()) {
    used_symbols.push_back(symbol);
  }
}

void ContractABI::register_get_method(FunctionPtr fun_ref) {
  tolk_assert(fun_ref->is_contract_getter());
  register_used_type(fun_ref->inferred_full_type);

  std::vector<ABIFunctionParameter> parameters;
  parameters.reserve(fun_ref->get_num_params());
  for (int i = 0; i < fun_ref->get_num_params(); ++i) {
    const LocalVarData& param_ref = fun_ref->get_param(i);
    parameters.emplace_back(ABIFunctionParameter{
      .name = param_ref.name,
      .ty = param_ref.declared_type, 
    });
    register_used_type(param_ref.declared_type);
  }

  getMethods.emplace_back(ABIGetMethod{
    .tvmMethodId = fun_ref->tvm_method_id,
    .name = fun_ref->name,
    .parameters = std::move(parameters),
    .returnTy = fun_ref->inferred_return_type,
    .description = get_abi_description(fun_ref->abi_annotation), 
  });
  register_used_type(fun_ref->inferred_return_type);
}

void ContractABI::register_incoming_message(TypePtr bodyTy) {
  // todo I don't like that minimalMsgValue is in @abi above a struct, not in `contract` annotation
  const AbiAnnotationForSymbol* abi_annotation = get_abi_annotation_above(bodyTy);
  std::optional<int64_t> minimalMsgValue;
  std::optional<int64_t> preferredSendMode;
  if (abi_annotation && abi_annotation->minimalMsgValue) {
    ConstValExpression val = unwrap_ConstVal_casts(eval_expression_if_const_or_fire(abi_annotation->minimalMsgValue));
    tolk_assert(std::holds_alternative<ConstValInt>(val));
    ConstValInt val_int = std::get<ConstValInt>(val);
    if (val_int.int_val->fits_bits(63)) {
      minimalMsgValue = val_int.int_val->to_long();
    }
  }
  if (abi_annotation && abi_annotation->preferredSendMode) {
    ConstValExpression val = unwrap_ConstVal_casts(eval_expression_if_const_or_fire(abi_annotation->preferredSendMode));
    tolk_assert(std::holds_alternative<ConstValInt>(val));
    ConstValInt val_int = std::get<ConstValInt>(val);
    if (val_int.int_val->fits_bits(63)) {
      preferredSendMode = val_int.int_val->to_long();
    }
  }
  incomingMessages.emplace_back(ABIInternalMessage{
    .bodyTy = bodyTy,
    .description = get_abi_description(bodyTy),
    .minimalMsgValue = minimalMsgValue,
    .preferredSendMode = preferredSendMode,
  });
  register_used_type(bodyTy);
}

void ContractABI::register_external_message(TypePtr bodyTy) {
  incomingExternal.emplace_back(ABIExternalMessage{
    .bodyTy = bodyTy,
    .description = get_abi_description(bodyTy),
  });
  register_used_type(bodyTy);
}

void ContractABI::register_outgoing_message(TypePtr bodyTy) {
  bodyTy = normalize_createMessageTy(bodyTy);

  // check for duplicates, since `bodyTy` is registered for every `createMessage` call
  auto it = std::find_if(outgoingMessages.begin(), outgoingMessages.end(), [bodyTy](const ABIOutgoingMessage& m) {
    return bodyTy->equal_to(m.bodyTy);
  });
  if (it != outgoingMessages.end() || bodyTy == TypeDataVoid::create()) {
    return;
  }
  
  outgoingMessages.emplace_back(ABIOutgoingMessage{
    .bodyTy = bodyTy,
    .description = get_abi_description(bodyTy),
  });
  register_used_type(bodyTy);
}

void ContractABI::register_emitted_event(TypePtr bodyTy) {
  bodyTy = normalize_createMessageTy(bodyTy);

  // check for duplicates, since `bodyTy` is registered for every `createMessage` call
  auto it = std::find_if(emittedEvents.begin(), emittedEvents.end(), [bodyTy](const ABIOutgoingMessage& m) {
    return bodyTy->equal_to(m.bodyTy);
  });
  if (it != emittedEvents.end() || bodyTy == TypeDataVoid::create()) {
    return;
  }
  
  emittedEvents.emplace_back(ABIOutgoingMessage{
    .bodyTy = bodyTy,
    .description = get_abi_description(bodyTy),
  });
  register_used_type(bodyTy);
}

void ContractABI::register_storage(TypePtr storageTy, TypePtr storageAtDeploymentTy) {
  if (storageTy != nullptr && storageTy != TypeDataNullLiteral::create()) {
    storage.storageTy = storageTy;
    register_used_type(storageTy);
  }
  if (storageAtDeploymentTy != nullptr && storageAtDeploymentTy != TypeDataNullLiteral::create()) {
    storage.storageAtDeploymentTy = storageAtDeploymentTy;
    register_used_type(storageAtDeploymentTy);
  }
}

void ContractABI::register_thrown_error(GlobalConstPtr const_ref) {
  ConstValExpression val = unwrap_ConstVal_casts(eval_and_cache_const_init_val(const_ref));
  tolk_assert(std::holds_alternative<ConstValInt>(val));

  register_thrown_error(std::get<ConstValInt>(val).int_val, const_ref->name);
  register_used_type(const_ref->inferred_type);
}

void ContractABI::register_thrown_error(const td::RefInt256& error_code, std::string const_name) {
  if (!error_code->fits_bits(31)) {
    return;
  }
  int errCode = static_cast<int>(error_code->to_long());

  // check for duplicates, since this function is called for every `throw` or `assert`
  // (but not only for errCode: there may be multiple constants with the same errCode, export them all)
  auto it = std::find_if(thrownErrors.begin(), thrownErrors.end(), [errCode, const_name](const ABIThrownError& e) {
    return errCode == e.errCode && const_name == e.constName; 
  });
  if (it != thrownErrors.end()) {
    return;
  }

  thrownErrors.emplace_back(ABIThrownError{
    .constName = std::move(const_name),
    .errCode = errCode,
  });
}

void ContractABI::register_constant(GlobalConstPtr const_ref) {
  constants.emplace_back(ABIConstant{
    .name = const_ref->name,
    .value = eval_and_cache_const_init_val(const_ref),
    .description = get_abi_description(const_ref->abi_annotation),
  });
}


static void to_json(JsonPrettyOutput& out, TypePtr type) {
  std::string type_as_json;
  type->as_abi_json(type_as_json);
  out << type_as_json;
}

static void to_json(JsonPrettyOutput& out, const ConstValExpression& v) {
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
  } else if (const auto* c = std::get_if<ConstValCastToType>(&v)) {
    out.key_value("kind", "castTo");
    out.key_value("inner", c->inner.front());
    out.key_value("castTo", c->cast_to);
  } else if (std::holds_alternative<ConstValNullLiteral>(v)) {
    out.key_value("kind", "null");
  } else {
    tolk_assert(false);
  }
  out.end_object();
}

static void to_json(JsonPrettyOutput& out, const GenericsDeclaration* genericTs) {
  out << '[';
  out << '"' << genericTs->get_nameT(0) << '"';
  for (int i = 1; i < genericTs->size(); ++i) {
    out << ", " << '"' << genericTs->get_nameT(i) << '"';
  }
  out << ']';
}

static void to_json(JsonPrettyOutput& out, const std::vector<ConstValExpression>& v) {
  out.start_array();
  for (const ConstValExpression& v_item : v) {
    out.write_value(v_item);
  }
  out.end_array();
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

ContractABI::ContractABI()
  : compilerName("tolk")
  , compilerVersion(TOLK_VERSION) {
}

void ContractABI::to_pretty_json(std::ostream& os) const {
  // todo camel_case or snakeCase for fields?
  JsonPrettyOutput json(os);
  json.start_object();

  json.key_value("abiSchemaVersion", ABI_SCHEMA_VERSION);
  json.key_value("contractName", this->contractName);
  if (!this->author.empty()) {
    json.key_value("author", this->author);
  }
  if (!this->version.empty()) {
    json.key_value("version", this->version);
  }
  if (!this->description.empty()) {
    json.key_value("description", this->description);
  }

  json.start_array("declarations");
  for (const Symbol* symbol : this->used_symbols) {
    json.start_object();
    if (StructPtr struct_ref = symbol->try_as<StructPtr>()) {
      json.key_value("kind", "Struct");
      json.key_value("name", struct_ref->name);
      if (struct_ref->is_generic_struct()) {
        json.key_value("typeParams", struct_ref->genericTs);
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
        if (field_ref->default_value) {
          json.key_value("defaultValue", eval_field_default_value(field_ref));
        }
        if (field_ref->abi_annotation && field_ref->abi_annotation->description) {
          json.key_value("description", get_abi_description(field_ref->abi_annotation));
        }
        // todo clientTy?
        json.end_object();
      }
      json.end_array();
      if (CustomPackUnpackF f = get_custom_pack_unpack_function(TypeDataStruct::create(struct_ref))) {
        json.key_value("customPackUnpack", f);
      }
    } else if (AliasDefPtr alias_ref = symbol->try_as<AliasDefPtr>()) {
      json.key_value("kind", "Alias");
      json.key_value("name", alias_ref->name);
      json.key_value("targetTy", alias_ref->underlying_type);
      if (alias_ref->is_generic_alias()) {
        json.key_value("typeParams", alias_ref->genericTs);
      }
      if (CustomPackUnpackF f = get_custom_pack_unpack_function(TypeDataAlias::create(alias_ref))) {
        json.key_value("customPackUnpack", f);
      }
    } else if (EnumDefPtr enum_ref = symbol->try_as<EnumDefPtr>()) {
      json.key_value("kind", "Enum");
      json.key_value("name", enum_ref->name);
      json.key_value("encodedAs", calculate_intN_to_serialize_enum(enum_ref));
      json.start_array("members");
      for (EnumMemberPtr member_ref : enum_ref->members) {
        json.start_object();
        json.key_value("name", member_ref->name);
        json.key_value("value", member_ref->computed_value);
        // todo description?
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

  json.start_array("incomingMessages");
  for (const ABIInternalMessage& m : this->incomingMessages) {
    json.start_object();
    json.key_value("bodyTy", m.bodyTy);
    if (!m.description.empty()) {
      json.key_value("description", m.description);
    }
    if (m.minimalMsgValue.has_value()) {
      json.key_value("minimalMsgValue", m.minimalMsgValue.value());
    }
    if (m.preferredSendMode.has_value()) {
      json.key_value("preferredSendMode", m.preferredSendMode.value());
    }
    json.end_object();
  }
  json.end_array();

  json.start_array("incomingExternal");
  for (const ABIExternalMessage& m : this->incomingExternal) {
    json.start_object();
    json.key_value("bodyTy", m.bodyTy);
    if (!m.description.empty()) {
      json.key_value("description", m.description);
    }
    json.end_object();
  }
  json.end_array();

  json.start_array("outgoingMessages");
  for (const ABIOutgoingMessage& m : this->outgoingMessages) {
    json.start_object();
    json.key_value("bodyTy", m.bodyTy);
    if (!m.description.empty()) {
      json.key_value("description", m.description);
    }
    json.end_object();
  }
  json.end_array();

  json.start_array("emittedEvents");
  for (const ABIOutgoingMessage& m : this->emittedEvents) {
    json.start_object();
    json.key_value("bodyTy", m.bodyTy);
    if (!m.description.empty()) {
      json.key_value("description", m.description);
    }
    json.end_object();
  }
  json.end_array();

  json.start_object("storage");
  if (this->storage.storageTy != nullptr) {
    json.key_value("storageTy", this->storage.storageTy);
  }
  if (this->storage.storageAtDeploymentTy != nullptr) {
    json.key_value("storageAtDeploymentTy", this->storage.storageAtDeploymentTy);
  }
  json.end_object();

  json.start_array("getMethods");
  for (const ABIGetMethod& m : this->getMethods) {
    json.start_object();
    json.key_value("tvmMethodId", m.tvmMethodId);
    json.key_value("name", m.name);
    json.start_array("parameters");
    for (const ABIFunctionParameter& p : m.parameters) {
      json.start_object();
      json.key_value("name", p.name);
      json.key_value("ty", p.ty);
      // todo description?
      // todo default value
      json.end_object();
    }
    json.end_array();
    json.key_value("returnTy", m.returnTy);
    if (!m.description.empty()) {
      json.key_value("description", m.description);
    }
    json.end_object();
  }
  json.end_array();

  json.start_array("thrownErrors");
  std::vector<ABIThrownError> sorted_throws = this->thrownErrors;
  std::sort(sorted_throws.begin(), sorted_throws.end(), [](const ABIThrownError& e1, const ABIThrownError& e2) {
    return e1.errCode < e2.errCode;
  });
  for (const ABIThrownError& e : sorted_throws) {
    json.start_object();
    if (!e.constName.empty()) {
      json.key_value("constName", e.constName);
    }
    json.key_value("errCode", e.errCode);
    json.end_object();
  }
  json.end_array();

  json.start_array("constants");
  for (const ABIConstant& c : this->constants) {
    json.start_object();
    json.key_value("name", c.name);
    json.key_value("value", c.value);
    if (!c.description.empty()) {
      json.key_value("description", c.description);
    }
    json.end_object();
  }
  json.end_array();

  json.key_value("compilerName", this->compilerName);
  json.key_value("compilerVersion", this->compilerVersion);

  json.end_object();
}

} // namespace tolk
