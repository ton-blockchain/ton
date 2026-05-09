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
#include "contract-directive.h"
#include "ast.h"

namespace tolk {

// `incomingMessages` and some other must be parsed as types, not as expressions
bool is_contract_property_type_node(std::string_view name) {
  return name == "incomingMessages"
      || name == "incomingExternal"
      || name == "outgoingMessages"
      || name == "emittedEvents"
      || name == "thrownErrors"
      || name == "storage"
      || name == "storageAtDeployment"
      || name == "forceAbiExport"
  ;
}


static std::string expect_string(V<ast_contract_directive_item> v) {
  auto v_str = v->is_value_expr() ? v->v_as_expr->try_as<ast_string_const>() : nullptr;
  if (!v_str) {
    err("property `{}` must be a string", v->name).fire(v->name_range());
  }
  return v_str->str_val;
}

static AnyTypeV expect_type(V<ast_contract_directive_item> v) {
  if (!v->is_value_type()) {
    err("property `{}` must be a type", v->name).fire(v->name_range());
  }
  return v->v_as_type;
}

ContractDirective* parse_contract_directive(AnyV v) {
  auto v_contract = v->try_as<ast_contract_directive>();

  // an object is allocated on a heap (and is present only for files having `contract` within)
  ContractDirective* d = new ContractDirective;
  d->contractName = static_cast<std::string>(v_contract->get_identifier()->name);

  std::vector<std::string_view> occurred;
  occurred.reserve(v_contract->size_items());
  for (int i = 0; i < v_contract->size_items(); ++i) {
    V<ast_contract_directive_item> ith = v_contract->get_ith_item(i);
    std::string_view prop = ith->name;
    if (std::find(occurred.begin(), occurred.end(), prop) != occurred.end()) {
      err("duplicated contract property `{}`", prop).fire(ith->name_range());
    }
    occurred.push_back(prop);

    if      (prop == "author")                d->author               = expect_string(ith);
    else if (prop == "version")               d->version              = expect_string(ith);
    else if (prop == "description")           d->description          = expect_string(ith);
    else if (prop == "incomingMessages")      d->incomingMessages     = expect_type(ith);
    else if (prop == "incomingExternal")      d->incomingExternal     = expect_type(ith);
    else if (prop == "outgoingMessages")      d->outgoingMessages     = expect_type(ith);
    else if (prop == "emittedEvents")         d->emittedEvents        = expect_type(ith);
    else if (prop == "thrownErrors")          d->thrownErrors         = expect_type(ith);
    else if (prop == "storage")               d->storage              = expect_type(ith);
    else if (prop == "storageAtDeployment")   d->storageAtDeployment  = expect_type(ith);
    else if (prop == "forceAbiExport")        d->forceAbiExport       = expect_type(ith);
    else if (prop != "custom")
      err("unknown contract property `{}`", prop).fire(ith->name_range());
  }

  return d;
}

} // namespace tolk
