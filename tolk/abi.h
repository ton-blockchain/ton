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
#pragma once
#include <string>
#include <vector>
#include <optional>
#include "fwd-declarations.h"
#include "type-export-json.h"

namespace tolk {

struct ABIFunctionParameter {
  std::string_view name;
  TypePtr ty;
  std::string description;
  std::optional<ConstValExpression> default_value;
};

struct ABIGetMethod {
  int tvm_method_id;
  std::string_view name;
  std::vector<ABIFunctionParameter> parameters;
  TypePtr return_ty;
  std::string description;
};

struct ABIInternalMessage {
  TypePtr body_ty;    // typically, it's StructPtr; its fields and description are available in declarations
};

struct ABIExternalMessage {
  TypePtr body_ty;
};

struct ABIOutgoingMessage {
  TypePtr body_ty;
};

struct ABIStorage {
  TypePtr storage_ty = nullptr;
  TypePtr storage_at_deployment_ty = nullptr;
};

enum class ABIThrownErrorKind {
  plain_int,
  constant,
  enum_member,
};

struct ABIThrownError {
  ABIThrownErrorKind kind;
  std::string name;           // empty / "CONST_NAME" / "EnumName.MemberName"
  std::string description;    // from doc comment over a constant or enum member
  int err_code;
};


struct ContractABI {
  std::string_view contract_name;
  std::string_view author;
  std::string_view version;
  std::string_view description;

  JsonTypeExporter json_types;

  ABIStorage storage;
  std::vector<ABIInternalMessage> incoming_messages;
  std::vector<ABIExternalMessage> incoming_external;
  std::vector<ABIOutgoingMessage> outgoing_messages;
  std::vector<ABIOutgoingMessage> emitted_events;
  std::vector<ABIGetMethod> get_methods;
  std::vector<ABIThrownError> thrown_errors;

  std::string_view compiler_name;
  std::string_view compiler_version;

  ContractABI();

  void register_storage(TypePtr storage_ty, TypePtr storage_at_deployment_ty);
  void register_get_method(FunctionPtr fun_ref);
  void register_incoming_message(TypePtr body_ty);
  void register_external_message(TypePtr body_ty);
  void register_outgoing_message(TypePtr body_ty);
  void register_emitted_event(TypePtr body_ty);
  void register_thrown_error(GlobalConstPtr const_ref);
  void register_thrown_error(EnumDefPtr enum_ref, EnumMemberPtr member_ref);
  void register_thrown_error(const td::RefInt256& err_code);
  void register_thrown_error(ABIThrownErrorKind kind, const td::RefInt256& error_code, std::string name, std::string description);

  void to_pretty_json(std::ostream& os) const;
};

} // namespace tolk
