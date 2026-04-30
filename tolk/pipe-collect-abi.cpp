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
#include "compiler-settings.h"
#include "contract-directive.h"
#include "generics-helpers.h"

namespace tolk {

class CollectAbiFromBodyVisitor final : public ASTVisitorFunctionBody {
  ContractABI* abi;

  void on_assert_throw(AnyExprV v_err_code) const {
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
      } else if (f_name == "createMessage") {
        abi->register_outgoing_message(called_f->substitutedTs->typeT_at(0));
      } else if (f_name == "createExternalLogMessage") {
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
  explicit CollectAbiFromBodyVisitor(ContractABI* cur_abi)
    : abi(cur_abi) {}

  bool should_visit_function(FunctionPtr fun_ref) override {
    // todo only really used functions
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }

  void on_exit_function(V<ast_function_declaration> v_function) override {
    if (cur_f->is_contract_getter()) {
      abi->register_get_method(cur_f);
    }
  }
};

static std::vector<TypePtr> ungroup_union_type(TypePtr specified_type) {
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

  if (d->incomingMessages) {
    for (TypePtr t_incoming : ungroup_union_type(d->incomingMessages->resolved_type)) {
      abi->register_incoming_message(t_incoming);
    }
  }
  if (d->incomingExternal) {
    for (TypePtr t_external : ungroup_union_type(d->incomingExternal->resolved_type)) {
      abi->register_external_message(t_external);
    }
  }
  if (d->storage) {
    abi->register_storage(d->storage->resolved_type, d->storageAtDeployment ? d->storageAtDeployment->resolved_type : nullptr);
  }
  if (d->forceAbiExport) {
    for (TypePtr t_export : ungroup_union_type(d->forceAbiExport->resolved_type)) {
      abi->json_types.register_used_type(t_export);
    }
  }

  if (!d->incomingExternal && lookup_global_symbol("onExternalMessage")) {
    abi->register_external_message(TypeDataSlice::create());
  }
}

void pipeline_collect_abi_output(std::ostream& os) {
  ContractABI abi;

  SrcFilePtr entrypoint_file = G.all_src_files.get_entrypoint_file();
  if (entrypoint_file->has_contract_directive()) {
    populate_abi_from_contract_directive(&abi, entrypoint_file->contract_directive);
  }
  if (const Symbol* f_main = lookup_global_symbol("main"); f_main && f_main->try_as<FunctionPtr>()) {
    abi.register_get_method(f_main->try_as<FunctionPtr>());
  }

  CollectAbiFromBodyVisitor visitor(&abi);
  visit_ast_of_all_functions(visitor);

  abi.to_pretty_json(os);
}

} // namespace tolk
