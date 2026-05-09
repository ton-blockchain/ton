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
#include "ast.h"
#include "ast-visitor.h"
#include "abi.h"
#include "type-system.h"
#include "compiler-state.h"
#include "contract-directive.h"
#include "generics-helpers.h"

namespace tolk {

class CollectAbiFromBodyVisitor final : public ASTVisitorFunctionBody {
  ContractABI* abi;
  bool collect_outgoing_messages;
  bool collect_emitted_events;
  bool collect_thrown_errors;

  void on_assert_throw(AnyExprV v_err_code) const {
    if (!collect_thrown_errors) {
      return;
    }

    // on `throw ERR`, `assert (cond, ERR)` and similar we register that constant;
    // note, that even if it's a local exception caught by `catch`, it's also registered, for simplicity
    while (auto v_cast = v_err_code->try_as<ast_cast_as_operator>()) {
      v_err_code = v_cast->get_expr();
    }

    if (auto v_ref = v_err_code->try_as<ast_reference>()) {
      if (GlobalConstPtr const_ref = v_ref->sym->try_as<GlobalConstPtr>()) {
        abi->register_thrown_error(const_ref);
      }
    } else if (auto v_dot = v_err_code->try_as<ast_dot_access>()) {
      if (v_dot->is_target_enum_member()) {
        EnumDefPtr enum_ref = v_dot->inferred_type->try_as<TypeDataEnum>()->enum_ref;
        EnumMemberPtr member_ref = std::get<EnumMemberPtr>(v_dot->target);
        abi->register_thrown_error(enum_ref, member_ref);
      }
    } else {
      ConstValExpression expr;
      try { expr = eval_expression_if_const_or_fire(v_err_code); }
      catch (...) { expr = ConstValNullLiteral{}; }
      if (const ConstValInt* thrown_int = std::get_if<ConstValInt>(&expr)) {
        abi->register_thrown_error(thrown_int->int_val);
      }
    }
  }

  void visit(V<ast_object_literal> v) override {
    if (v->struct_ref->name == "UnpackOptions") {
      for (int i = 0; i < v->get_body()->get_num_fields(); ++i) {
        if (v->get_body()->get_field(i)->get_field_name() == "throwIfOpcodeDoesNotMatch") {
          on_assert_throw(v->get_body()->get_field(i)->get_init_val());
        }
      }
    }

    parent::visit(v);
  }
  
  void visit(V<ast_function_call> v) override {
    FunctionPtr called_f = v->fun_maybe;

    if (called_f && called_f->is_builtin() && called_f->is_instantiation_of_generic_function()) {
      std::string_view f_name = called_f->base_fun_ref->name;
      if (f_name == "map<K,V>.mustGet" && v->get_num_args() > 1) {
        on_assert_throw(v->get_arg(1)->get_expr());
      } else if (f_name == "createMessage" && collect_outgoing_messages) {
        abi->register_outgoing_message(called_f->substitutedTs->typeT_at(0));
      } else if (f_name == "createExternalLogMessage" && collect_emitted_events) {
        abi->register_emitted_event(called_f->substitutedTs->typeT_at(0));
      }
    }

    parent::visit(v);
  }

  void visit(V<ast_throw_statement> v) override {
    on_assert_throw(v->get_thrown_code());
    parent::visit(v);
  }

  void visit(V<ast_assert_statement> v) override {
    on_assert_throw(v->get_thrown_code());
    parent::visit(v);
  }

public:
  CollectAbiFromBodyVisitor(ContractABI* cur_abi, bool collect_outgoing_messages, bool collect_emitted_events, bool collect_thrown_errors)
    : abi(cur_abi)
    , collect_outgoing_messages(collect_outgoing_messages)
    , collect_emitted_events(collect_emitted_events)
    , collect_thrown_errors(collect_thrown_errors) {}

  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && fun_ref->is_really_used();
  }

  void on_exit_function(V<ast_function_declaration> v_function) override {
    if (cur_f->is_contract_getter()) {
      abi->register_get_method(cur_f);
    }
  }
};

// For `incomingMessages`, `forceAbiExport`, etc. the user may specify:
// - a union, like `AllowedMessages` or directly `Msg1 | Msg2 | Msg3`
// - a tensor, like `(Msg1, Msg2, Msg3)`
// If so, this type is ungrouped to distinct types.
static std::vector<TypePtr> ungroup_union_type(AnyTypeV specified_type_node) {
  if (specified_type_node == nullptr) {
    return {};
  }
  TypePtr specified_type = specified_type_node->resolved_type;
  tolk_assert(specified_type != nullptr);

  if (const TypeDataUnion* i_union = specified_type->unwrap_alias()->try_as<TypeDataUnion>()) {
    return i_union->variants;
  }
  if (const TypeDataTensor* i_tensor = specified_type->unwrap_alias()->try_as<TypeDataTensor>()) {
    return i_tensor->items;
  }
  if (specified_type != TypeDataNullLiteral::create()) {
    return {specified_type};
  }
  return {};
}

static void populate_abi_from_contract_directive(ContractABI* abi, const ContractDirective* d) {
  abi->contract_name  = d->contractName;
  abi->author         = d->author;
  abi->version        = d->version;
  abi->description    = d->description;

  for (TypePtr t_incoming : ungroup_union_type(d->incomingMessages)) {
    abi->register_incoming_message(t_incoming);
  }
  for (TypePtr t_external : ungroup_union_type(d->incomingExternal)) {
    abi->register_external_message(t_external);
  }
  for (TypePtr t_outgoing : ungroup_union_type(d->outgoingMessages)) {
    abi->register_outgoing_message(t_outgoing);
  }
  for (TypePtr t_event : ungroup_union_type(d->emittedEvents)) {
    abi->register_emitted_event(t_event);
  }
  for (TypePtr t_errors : ungroup_union_type(d->thrownErrors)) {
    if (const TypeDataEnum* t_enum = t_errors->unwrap_alias()->try_as<TypeDataEnum>()) {
      for (EnumMemberPtr member_ref : t_enum->enum_ref->members) {
        abi->register_thrown_error(t_enum->enum_ref, member_ref);
      }
      abi->json_types.register_used_type(t_enum);
    } else {
      err("`thrownErrors` must be an enum type, got `{}`", t_errors).fire(d->thrownErrors);
    }
  }
  if (d->storage) {
    abi->register_storage(d->storage->resolved_type, d->storageAtDeployment ? d->storageAtDeployment->resolved_type : nullptr);
  }
  for (TypePtr t_export : ungroup_union_type(d->forceAbiExport)) {
    abi->json_types.register_used_type(t_export);
  }

  if (!d->incomingExternal && lookup_global_symbol("onExternalMessage")) {
    abi->register_external_message(TypeDataSlice::create());
  }
}

void pipeline_collect_abi_output(std::ostream& os) {
  ContractABI abi;
  bool collect_outgoing_messages = true;
  bool collect_emitted_events = true;
  bool collect_thrown_errors = true;

  SrcFilePtr entrypoint_file = G.all_src_files.get_entrypoint_file();
  if (entrypoint_file->has_contract_directive()) {
    const ContractDirective* directive = entrypoint_file->contract_directive;
    populate_abi_from_contract_directive(&abi, directive);
    collect_outgoing_messages = !directive->outgoingMessages;
    collect_emitted_events = !directive->emittedEvents;
    collect_thrown_errors = !directive->thrownErrors;
  }
  if (const Symbol* f_main = lookup_global_symbol("main"); f_main && f_main->try_as<FunctionPtr>()) {
    abi.register_get_method(f_main->try_as<FunctionPtr>());
  }

  CollectAbiFromBodyVisitor visitor(&abi, collect_outgoing_messages, collect_emitted_events, collect_thrown_errors);
  visit_ast_of_all_functions(visitor);

  abi.to_pretty_json(os);
}

} // namespace tolk
