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

// above get methods, we allow `@param` in comments,
// so a result of parsing doc_lines is ParsedDocComment
struct ParsedDocComment {
  std::string description;
  std::vector<std::pair<std::string_view, std::string>> param_descriptions;

  std::string find_param_description(std::string_view param_name) const {
    for (const auto& [name, desc] : param_descriptions) {
      if (name == param_name) {
        return desc;
      }
    }
    return {};
  }
};

static void append_line(std::string& s, std::string_view line) {
  if (!s.empty()) {
    s += '\n';
  }
  s += line;
}

static ParsedDocComment parse_doc_comment(const DocCommentLines& doc_lines) {
  ParsedDocComment result;
  for (std::string_view line : doc_lines) {
    if (line.starts_with("@param ") && line.size() > 7) {
      std::string_view after = line.substr(7);
      if (size_t sp = after.find(' '); sp != std::string::npos) {
        std::string_view param_name = after.substr(0, sp);
        std::string_view param_desc = after.substr(sp + 1);
        result.param_descriptions.emplace_back(param_name, std::string(param_desc));
      }
    } else if (!result.param_descriptions.empty() && !line.empty() && !line.starts_with("@")) {
      append_line(result.param_descriptions.back().second, line);
    } else {
      append_line(result.description, line);
    }
  }
  return result;
}

static TypePtr normalize_createMessage_ty(TypePtr body_ty) {
  if (const TypeDataStruct* t_struct = body_ty->unwrap_alias()->try_as<TypeDataStruct>()) {
    if (t_struct->struct_ref->is_instantiation_of_CellT() || t_struct->struct_ref->is_instantiation_of_UnsafeBodyNoRef()) {
      body_ty = t_struct->struct_ref->substitutedTs->typeT_at(0);
    }
  }
  return body_ty;
}

void ContractABI::register_storage(TypePtr storage_ty, TypePtr storage_at_deployment_ty) {
  if (storage_ty != nullptr && storage_ty != TypeDataNullLiteral::create()) {
    storage.storage_ty = storage_ty;
    json_types.register_used_type(storage_ty);
  }
  if (storage_at_deployment_ty != nullptr && storage_at_deployment_ty != TypeDataNullLiteral::create()) {
    storage.storage_at_deployment_ty = storage_at_deployment_ty;
    json_types.register_used_type(storage_at_deployment_ty);
  }
}

void ContractABI::register_get_method(FunctionPtr fun_ref) {
  tolk_assert(fun_ref->is_contract_getter() || fun_ref->name == "main");
  json_types.register_used_type(fun_ref->inferred_return_type);
  for (int i = 0; i < fun_ref->get_num_params(); ++i) {
    json_types.register_used_type(fun_ref->get_param(i).declared_type);
  }

  ParsedDocComment doc = parse_doc_comment(fun_ref->doc_lines);

  std::vector<ABIFunctionParameter> parameters;
  parameters.reserve(fun_ref->get_num_params());
  for (int i = 0; i < fun_ref->get_num_params(); ++i) {
    const LocalVarData& param_ref = fun_ref->get_param(i);
    std::optional<ConstValExpression> default_value;
    if (param_ref.default_value) {
      default_value = eval_expression_if_const_or_fire(param_ref.default_value);
      json_types.register_used_const_val(default_value.value());
    }
    parameters.emplace_back(ABIFunctionParameter{
      .name = param_ref.name,
      .ty = param_ref.declared_type,
      .description = doc.find_param_description(param_ref.name),
      .default_value = std::move(default_value),
    });
    json_types.register_used_type(param_ref.declared_type);
  }

  get_methods.emplace_back(ABIGetMethod{
    .tvm_method_id = fun_ref->tvm_method_id,
    .name = fun_ref->name,
    .parameters = std::move(parameters),
    .return_ty = fun_ref->inferred_return_type,
    .description = std::move(doc.description),
  });
  json_types.register_used_type(fun_ref->inferred_return_type);
}

void ContractABI::register_incoming_message(TypePtr body_ty) {
  incoming_messages.emplace_back(ABIInternalMessage{
    .body_ty = body_ty,
  });
  json_types.register_used_type(body_ty);
}

void ContractABI::register_external_message(TypePtr body_ty) {
  incoming_external.emplace_back(ABIExternalMessage{
    .body_ty = body_ty,
  });
  json_types.register_used_type(body_ty);
}

void ContractABI::register_outgoing_message(TypePtr body_ty) {
  body_ty = normalize_createMessage_ty(body_ty);

  auto it = std::find_if(outgoing_messages.begin(), outgoing_messages.end(), [body_ty](const ABIOutgoingMessage& m) {
    return body_ty->equal_to(m.body_ty);
  });
  if (it != outgoing_messages.end() || body_ty == TypeDataVoid::create()) {
    return;
  }
  
  outgoing_messages.emplace_back(ABIOutgoingMessage{
    .body_ty = body_ty,
  });
  json_types.register_used_type(body_ty);
}

void ContractABI::register_emitted_event(TypePtr body_ty) {
  body_ty = normalize_createMessage_ty(body_ty);

  auto it = std::find_if(emitted_events.begin(), emitted_events.end(), [body_ty](const ABIOutgoingMessage& m) {
    return body_ty->equal_to(m.body_ty);
  });
  if (it != emitted_events.end() || body_ty == TypeDataVoid::create()) {
    return;
  }
  
  emitted_events.emplace_back(ABIOutgoingMessage{
    .body_ty = body_ty,
  });
  json_types.register_used_type(body_ty);
}

void ContractABI::register_thrown_error(GlobalConstPtr const_ref) {
  ConstValExpression val = eval_and_cache_const_init_val(const_ref);
  while (std::holds_alternative<ConstValCastToType>(val)) {     // unwrap `const A: int32 = 5`
    val = std::get<ConstValCastToType>(val).inner.front();   // (it's "cast 5 to int32" in a const-expr tree)
  }
  if (!std::holds_alternative<ConstValInt>(val)) {
    return;
  }

  register_thrown_error(ABIThrownErrorKind::constant, std::get<ConstValInt>(val).int_val, const_ref->name, get_abi_description(const_ref->doc_lines));
  json_types.register_used_type(const_ref->inferred_type);
}

void ContractABI::register_thrown_error(EnumDefPtr enum_ref, EnumMemberPtr member_ref) {
  std::string name = enum_ref->name + "." + member_ref->name;
  register_thrown_error(ABIThrownErrorKind::enum_member, member_ref->computed_value, std::move(name), get_abi_description(member_ref->doc_lines));
}

void ContractABI::register_thrown_error(const td::RefInt256& err_code) {
  register_thrown_error(ABIThrownErrorKind::plain_int, err_code, "", "");
}

void ContractABI::register_thrown_error(ABIThrownErrorKind kind, const td::RefInt256& error_code, std::string name, std::string description) {
  if (!error_code->fits_bits(31)) {
    return;
  }
  int err_code = static_cast<int>(error_code->to_long());

  auto it = std::find_if(thrown_errors.begin(), thrown_errors.end(), [kind, err_code, name](const ABIThrownError& e) {
    return kind == e.kind && err_code == e.err_code && name == e.name;
  });
  if (it != thrown_errors.end()) {
    return;
  }

  thrown_errors.emplace_back(ABIThrownError{
    .kind = kind,
    .name = std::move(name),
    .description = std::move(description),
    .err_code = err_code,
  });
}


// --------------------------------------------
//    output ABI to JSON
//

static void to_json(JsonPrettyOutput& json, ABIThrownErrorKind kind) {
  switch (kind) {
    case ABIThrownErrorKind::plain_int:     json.write_value("plain_int");     break;
    case ABIThrownErrorKind::constant:      json.write_value("constant");      break;
    case ABIThrownErrorKind::enum_member:   json.write_value("enum_member");   break;
  }
}

ContractABI::ContractABI()
  : compiler_name("tolk")
  , compiler_version(TOLK_VERSION) {
  json_types.seed_primitive_types();
}

void ContractABI::to_pretty_json(std::ostream& os) const {
  JsonPrettyOutput json(os);
  json.start_object();

  json.key_value("abi_schema_version", ABI_SCHEMA_VERSION);
  json.key_value("contract_name", this->contract_name);
  if (!this->author.empty()) {
    json.key_value("author", this->author);
  }
  if (!this->version.empty()) {
    json.key_value("version", this->version);
  }
  if (!this->description.empty()) {
    json.key_value("description", this->description);
  }

  this->json_types.emit_unique_ty_and_declarations_json(json, {
    .emit_default_values = true,
    .emit_descriptions = true,
    .emit_abi_client_types = true,
  });

  json.start_object("storage");
  if (this->storage.storage_ty != nullptr) {
    json.key_value("storage_ty_idx", this->json_types.get_type_idx(this->storage.storage_ty));
  }
  if (this->storage.storage_at_deployment_ty != nullptr) {
    json.key_value("storage_at_deployment_ty_idx", this->json_types.get_type_idx(this->storage.storage_at_deployment_ty));
  }
  json.end_object();

  json.start_array("incoming_messages");
  for (const ABIInternalMessage& m : this->incoming_messages) {
    json.next_array_item();
    json.start_object();
    json.key_value("body_ty_idx", this->json_types.get_type_idx(m.body_ty));
    json.end_object();
  }
  json.end_array();

  json.start_array("incoming_external");
  for (const ABIExternalMessage& m : this->incoming_external) {
    json.next_array_item();
    json.start_object();
    json.key_value("body_ty_idx", this->json_types.get_type_idx(m.body_ty));
    json.end_object();
  }
  json.end_array();

  json.start_array("outgoing_messages");
  for (const ABIOutgoingMessage& m : this->outgoing_messages) {
    json.next_array_item();
    json.start_object();
    json.key_value("body_ty_idx", this->json_types.get_type_idx(m.body_ty));
    json.end_object();
  }
  json.end_array();

  json.start_array("emitted_events");
  for (const ABIOutgoingMessage& m : this->emitted_events) {
    json.next_array_item();
    json.start_object();
    json.key_value("body_ty_idx", this->json_types.get_type_idx(m.body_ty));
    json.end_object();
  }
  json.end_array();

  json.start_array("get_methods");
  for (const ABIGetMethod& m : this->get_methods) {
    json.next_array_item();
    json.start_object();
    json.key_value("tvm_method_id", m.tvm_method_id);
    json.key_value("name", m.name);
    json.start_array("parameters");
    for (const ABIFunctionParameter& p : m.parameters) {
      json.next_array_item();
      json.start_object();
      json.key_value("name", p.name);
      json.key_value("ty_idx", this->json_types.get_type_idx(p.ty));
      if (!p.description.empty()) {
        json.key_value("description", p.description);
      }
      if (p.default_value.has_value()) {
        json.key_value("default_value", this->json_types.const_val_json(p.default_value.value()));
      }
      json.end_object();
    }
    json.end_array();
    json.key_value("return_ty_idx", this->json_types.get_type_idx(m.return_ty));
    if (!m.description.empty()) {
      json.key_value("description", m.description);
    }
    json.end_object();
  }
  json.end_array();

  std::vector<ABIThrownError> sorted_throws = this->thrown_errors;
  std::sort(sorted_throws.begin(), sorted_throws.end(), [](const ABIThrownError& e1, const ABIThrownError& e2) {
    return e1.err_code < e2.err_code;
  });
  json.start_array("thrown_errors");
  for (const ABIThrownError& e : sorted_throws) {
    json.next_array_item();
    json.start_object();
    json.key_value("kind", e.kind);
    if (!e.name.empty()) {
      json.key_value("name", e.name);
    }
    if (!e.description.empty()) {
      json.key_value("description", e.description);
    }
    json.key_value("err_code", e.err_code);
    json.end_object();
  }
  json.end_array();

  json.key_value("compiler_name", this->compiler_name);
  json.key_value("compiler_version", this->compiler_version);

  json.end_object();
}

} // namespace tolk
