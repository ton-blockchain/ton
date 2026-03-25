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
#include <unordered_set>
#include "fwd-declarations.h"
#include "constant-evaluator.h"

namespace tolk {

struct ABIFunctionParameter {
  std::string_view name;
  TypePtr ty;
  std::string description;
  std::optional<ConstValExpression> defaultValue;
};

struct ABIGetMethod {
  int tvmMethodId;
  std::string_view name;
  std::vector<ABIFunctionParameter> parameters;
  TypePtr returnTy;
  std::string description;
};

struct ABIInternalMessage {
  TypePtr bodyTy;
  std::string description;
  std::optional<int64_t> minimalMsgValue;
  std::optional<int64_t> preferredSendMode;
};

struct ABIExternalMessage {
  TypePtr bodyTy;
  std::string description;
};

struct ABIOutgoingMessage {
  TypePtr bodyTy;
  std::string description;
};

struct ABIStorage {
  TypePtr storageTy = nullptr;
  TypePtr storageAtDeploymentTy = nullptr;
};

enum class ABIThrownErrorKind {
  plainInt,
  constant,
  enumMember,
};

struct ABIThrownError {
  ABIThrownErrorKind kind;
  std::string name;           // empty / "CONST_NAME" / "EnumName.MemberName"
  int errCode;
};

struct ABIConstant {
  std::string_view name;
  ConstValExpression value;
  std::string description;
};


struct ContractABI {
  std::string_view contractName;
  std::string_view author;
  std::string_view version;
  std::string_view description;

  std::unordered_set<TypePtr> used_types;     // to collect unique declarations
  std::vector<const Symbol*> used_symbols;    // structs, aliases, enums

  ABIStorage storage;
  std::vector<ABIInternalMessage> incomingMessages;
  std::vector<ABIExternalMessage> incomingExternal;
  std::vector<ABIOutgoingMessage> outgoingMessages;
  std::vector<ABIOutgoingMessage> emittedEvents;
  std::vector<ABIGetMethod> getMethods;
  std::vector<ABIThrownError> thrownErrors;
  std::vector<ABIConstant> constants;

  std::string_view compilerName;
  std::string_view compilerVersion;
  // final ABI also contains `codeBoc64`, but it's filled by tolk-js, after calling Fift

  ContractABI();

  void register_used_symbol(const Symbol* sym);
  void register_used_type(TypePtr type);
  void register_storage(TypePtr storageTy, TypePtr storageAtDeploymentTy);
  void register_get_method(FunctionPtr fun_ref);
  void register_incoming_message(TypePtr bodyTy);
  void register_external_message(TypePtr bodyTy);
  void register_outgoing_message(TypePtr bodyTy);
  void register_emitted_event(TypePtr bodyTy);
  void register_thrown_error(GlobalConstPtr const_ref);
  void register_thrown_error(EnumDefPtr enum_ref, EnumMemberPtr member_ref);
  void register_thrown_error(const td::RefInt256& err_code);
  void register_thrown_error(ABIThrownErrorKind kind, const td::RefInt256& error_code, std::string name);
  void register_constant(GlobalConstPtr const_ref);

  void to_pretty_json(std::ostream& os) const;
};

} // namespace tolk
