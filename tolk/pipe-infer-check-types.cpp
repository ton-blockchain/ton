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
#include "tolk.h"
#include "src-file.h"
#include "ast.h"
#include "ast-visitor.h"

/*
 *   This pipe does type inferring.
 *   It will be fully rewritten, because current type system is based on Hindley-Milner (unifying usages),
 * and I am going to introduce a static type system, drop TypeExpr completely, etc.
 *   Currently, after this inferring, lots of `te_Indirect` and partially complete types still exist,
 * whey are partially refined during converting AST to legacy.
 */

namespace tolk {

class InferAndCheckTypesInsideFunctionVisitor final : public ASTVisitorFunctionBody {
  const FunctionData* current_function = nullptr;

  static bool expect_integer(TypeExpr* inferred) {
    try {
      TypeExpr* t_int = TypeExpr::new_atomic(TypeExpr::_Int);
      unify(inferred, t_int);
      return true;
    } catch (UnifyError&) {
      return false;
    }
  }

  static bool expect_integer(AnyExprV v_inferred) {
    return expect_integer(v_inferred->inferred_type);
  }

  static bool is_expr_valid_as_return_self(AnyExprV return_expr) {
    // `return self`
    if (return_expr->type == ast_self_keyword) {
      return true;
    }
    // `return self.someMethod()`
    if (auto v_call = return_expr->try_as<ast_dot_method_call>()) {
      return v_call->fun_ref->does_return_self() && is_expr_valid_as_return_self(v_call->get_obj());
    }
    // `return cond ? ... : ...`
    if (auto v_ternary = return_expr->try_as<ast_ternary_operator>()) {
      return is_expr_valid_as_return_self(v_ternary->get_when_true()) && is_expr_valid_as_return_self(v_ternary->get_when_false());
    }
    return false;
  }

  void visit(V<ast_parenthesized_expression> v) override {
    parent::visit(v->get_expr());
    v->mutate()->assign_inferred_type(v->get_expr()->inferred_type);
  }

  void visit(V<ast_tensor> v) override {
    if (v->empty()) {
      v->mutate()->assign_inferred_type(TypeExpr::new_unit());
      return;
    }
    std::vector<TypeExpr*> types_list;
    types_list.reserve(v->get_items().size());
    for (AnyExprV item : v->get_items()) {
      parent::visit(item);
      types_list.emplace_back(item->inferred_type);
    }
    v->mutate()->assign_inferred_type(TypeExpr::new_tensor(std::move(types_list)));
  }

  void visit(V<ast_tensor_square> v) override {
    if (v->empty()) {
      v->mutate()->assign_inferred_type(TypeExpr::new_tuple(TypeExpr::new_unit()));
      return;
    }
    std::vector<TypeExpr*> types_list;
    types_list.reserve(v->get_items().size());
    for (AnyExprV item : v->get_items()) {
      parent::visit(item);
      types_list.emplace_back(item->inferred_type);
    }
    v->mutate()->assign_inferred_type(TypeExpr::new_tuple(TypeExpr::new_tensor(std::move(types_list), false)));
  }

  void visit(V<ast_identifier> v) override {
    if (const auto* glob_ref = v->sym->try_as<GlobalVarData>()) {
      v->mutate()->assign_inferred_type(glob_ref->declared_type);
    } else if (const auto* const_ref = v->sym->try_as<GlobalConstData>()) {
      v->mutate()->assign_inferred_type(const_ref->inferred_type);
    } else if (const auto* fun_ref = v->sym->try_as<FunctionData>()) {
      v->mutate()->assign_inferred_type(fun_ref->full_type);
    } else if (const auto* var_ref = v->sym->try_as<LocalVarData>()) {
      v->mutate()->assign_inferred_type(var_ref->declared_type);
    }
  }

  void visit(V<ast_int_const> v) override {
    v->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));
  }

  void visit(V<ast_string_const> v) override {
    switch (v->modifier) {
      case 0:
      case 's':
      case 'a':
        v->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Slice));
        break;
      case 'u':
      case 'h':
      case 'H':
      case 'c':
        v->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));
        break;
      default:
        break;
    }
  }

  void visit(V<ast_bool_const> v) override {
    v->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));
  }

  void visit(V<ast_null_keyword> v) override {
    const FunctionData* fun_ref = lookup_global_symbol("__null")->as<FunctionData>();
    TypeExpr* fun_type = TypeExpr::new_map(TypeExpr::new_unit(), TypeExpr::new_hole());
    TypeExpr* sym_type = fun_ref->full_type;
    try {
      unify(fun_type, sym_type);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "cannot apply function " << fun_ref->name << " : " << fun_ref->full_type << " to arguments of type "
         << fun_type->args[0] << ": " << ue;
      v->error(os.str());
    }
    TypeExpr* e_type = fun_type->args[1];
    TypeExpr::remove_indirect(e_type);
    v->mutate()->assign_inferred_type(e_type);
  }

  void visit(V<ast_self_keyword> v) override {
    v->mutate()->assign_inferred_type(v->param_ref->declared_type);
  }

  void visit(V<ast_argument> v) override {
    parent::visit(v->get_expr());
    v->mutate()->assign_inferred_type(v->get_expr()->inferred_type);
  }

  void visit(V<ast_argument_list> v) override {
    if (v->empty()) {
      v->mutate()->assign_inferred_type(TypeExpr::new_unit());
      return;
    }
    std::vector<TypeExpr*> types_list;
    types_list.reserve(v->size());
    for (AnyExprV item : v->get_arguments()) {
      parent::visit(item);
      types_list.emplace_back(item->inferred_type);
    }
    v->mutate()->assign_inferred_type(TypeExpr::new_tensor(std::move(types_list)));
  }

  void visit(V<ast_function_call> v) override {
    // special error for "null()" which is a FunC syntax
    if (v->get_called_f()->type == ast_null_keyword) {
      v->error("null is not a function: use `null`, not `null()`");
    }

    parent::visit(v->get_called_f());
    visit(v->get_arg_list());

    // most likely it's a global function, but also may be `some_var(args)` or even `getF()(args)`
    const FunctionData* fun_ref = v->fun_maybe;
    if (!fun_ref) {
      TypeExpr* arg_tensor = v->get_arg_list()->inferred_type;
      TypeExpr* lhs_type = v->get_called_f()->inferred_type;
      TypeExpr* fun_type = TypeExpr::new_map(arg_tensor, TypeExpr::new_hole());
      try {
        unify(fun_type, lhs_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "cannot apply expression of type " << lhs_type << " to an expression of type " << arg_tensor
           << ": " << ue;
        v->error(os.str());
      }
      TypeExpr* e_type = fun_type->args[1];
      TypeExpr::remove_indirect(e_type);
      v->mutate()->assign_inferred_type(e_type);
      return;
    }

    TypeExpr* arg_tensor = v->get_arg_list()->inferred_type;
    TypeExpr* fun_type = TypeExpr::new_map(arg_tensor, TypeExpr::new_hole());
    TypeExpr* sym_type = fun_ref->full_type;
    try {
      unify(fun_type, sym_type);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "cannot apply function " << fun_ref->name << " : " << fun_ref->full_type << " to arguments of type "
         << fun_type->args[0] << ": " << ue;
      v->error(os.str());
    }
    TypeExpr* e_type = fun_type->args[1];
    TypeExpr::remove_indirect(e_type);

    if (fun_ref->has_mutate_params()) {
      tolk_assert(e_type->constr == TypeExpr::te_Tensor);
      e_type = e_type->args[e_type->args.size() - 1];
    }

    v->mutate()->assign_inferred_type(e_type);
  }
  
  void visit(V<ast_dot_method_call> v) override {
    parent::visit(v->get_obj());
    visit(v->get_arg_list());
    std::vector<TypeExpr*> arg_types;
    arg_types.reserve(1 + v->get_num_args());
    arg_types.push_back(v->get_obj()->inferred_type);
    for (int i = 0; i < v->get_num_args(); ++i) {
      arg_types.push_back(v->get_arg(i)->inferred_type);
    }

    TypeExpr* arg_tensor = TypeExpr::new_tensor(std::move(arg_types));
    TypeExpr* fun_type = TypeExpr::new_map(arg_tensor, TypeExpr::new_hole());
    TypeExpr* sym_type = v->fun_ref->full_type;
    try {
      unify(fun_type, sym_type);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "cannot apply function " << v->fun_ref->name << " : " << v->fun_ref->full_type << " to arguments of type "
         << fun_type->args[0] << ": " << ue;
      v->error(os.str());
    }
    TypeExpr* e_type = fun_type->args[1];
    TypeExpr::remove_indirect(e_type);

    if (v->fun_ref->has_mutate_params()) {
      tolk_assert(e_type->constr == TypeExpr::te_Tensor);
      e_type = e_type->args[e_type->args.size() - 1];
    }
    if (v->fun_ref->does_return_self()) {
      e_type = v->get_obj()->inferred_type;
      TypeExpr::remove_indirect(e_type);
    }

    v->mutate()->assign_inferred_type(e_type);
  }

  void visit(V<ast_underscore> v) override {
    v->mutate()->assign_inferred_type(TypeExpr::new_hole());
  }

  void visit(V<ast_unary_operator> v) override {
    parent::visit(v->get_rhs());
    if (!expect_integer(v->get_rhs())) {
      v->error("operator `" + static_cast<std::string>(v->operator_name) + "` expects integer operand");
    }
    v->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));
  }

  void visit(V<ast_binary_operator> v) override {
    parent::visit(v->get_lhs());
    parent::visit(v->get_rhs());
    switch (v->tok) {
      case tok_assign: {
        TypeExpr* lhs_type = v->get_lhs()->inferred_type;
        TypeExpr* rhs_type = v->get_rhs()->inferred_type;
        try {
          unify(lhs_type, rhs_type);
        } catch (UnifyError& ue) {
          std::ostringstream os;
          os << "cannot assign an expression of type " << rhs_type << " to a variable or pattern of type "
             << lhs_type << ": " << ue;
          v->error(os.str());
        }
        TypeExpr* e_type = lhs_type;
        TypeExpr::remove_indirect(e_type);
        v->mutate()->assign_inferred_type(e_type);
        break;
      }
      case tok_eq:
      case tok_neq:
      case tok_spaceship: {
        if (!expect_integer(v->get_lhs()) || !expect_integer(v->get_rhs())) {
          v->error("comparison operators `== !=` can compare only integers");
        }
        v->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));
        break;
      }
      case tok_logical_and:
      case tok_logical_or: {
        if (!expect_integer(v->get_lhs()) || !expect_integer(v->get_rhs())) {
          v->error("logical operators `&& ||` expect integer operands");
        }
        v->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));
        break;
      }
      default:
        if (!expect_integer(v->get_lhs()) || !expect_integer(v->get_rhs())) {
          v->error("operator `" + static_cast<std::string>(v->operator_name) + "` expects integer operands");
        }
        v->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));
    }
  }

  void visit(V<ast_ternary_operator> v) override {
    parent::visit(v->get_cond());
    if (!expect_integer(v->get_cond())) {
      v->get_cond()->error("condition of ternary ?: operator must be an integer");
    }
    parent::visit(v->get_when_true());
    parent::visit(v->get_when_false());

    TypeExpr* res = TypeExpr::new_hole();
    TypeExpr *ttrue = v->get_when_true()->inferred_type;
    TypeExpr *tfals = v->get_when_false()->inferred_type;
    unify(res, ttrue);
    unify(res, tfals);
    v->mutate()->assign_inferred_type(res);
  }

  void visit(V<ast_if_statement> v) override {
    parent::visit(v->get_cond());
    parent::visit(v->get_if_body());
    parent::visit(v->get_else_body());
    TypeExpr* flag_type = TypeExpr::new_atomic(TypeExpr::_Int);
    TypeExpr* cond_type = v->get_cond()->inferred_type;
    try {
      
      unify(cond_type, flag_type);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "`if` condition value of type " << cond_type << " is not an integer: " << ue;
      v->get_cond()->error(os.str());
    }
    v->get_cond()->mutate()->assign_inferred_type(cond_type);
  }

  void visit(V<ast_repeat_statement> v) override {
    parent::visit(v->get_cond());
    parent::visit(v->get_body());
    TypeExpr* cnt_type = TypeExpr::new_atomic(TypeExpr::_Int);
    TypeExpr* cond_type = v->get_cond()->inferred_type;
    try {
      unify(cond_type, cnt_type);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "repeat count value of type " << cond_type << " is not an integer: " << ue;
      v->get_cond()->error(os.str());
    }
    v->get_cond()->mutate()->assign_inferred_type(cond_type);
  }

  void visit(V<ast_while_statement> v) override {
    parent::visit(v->get_cond());
    parent::visit(v->get_body());
    TypeExpr* cnt_type = TypeExpr::new_atomic(TypeExpr::_Int);
    TypeExpr* cond_type = v->get_cond()->inferred_type;
    try {
      unify(cond_type, cnt_type);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "`while` condition value of type " << cond_type << " is not an integer: " << ue;
      v->get_cond()->error(os.str());
    }
    v->get_cond()->mutate()->assign_inferred_type(cond_type);
  }

  void visit(V<ast_do_while_statement> v) override {
    parent::visit(v->get_body());
    parent::visit(v->get_cond());
    TypeExpr* cnt_type = TypeExpr::new_atomic(TypeExpr::_Int);
    TypeExpr* cond_type = v->get_cond()->inferred_type;
    try {
      unify(cond_type, cnt_type);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "`while` condition value of type " << cond_type << " is not an integer: " << ue;
      v->get_cond()->error(os.str());
    }
    v->get_cond()->mutate()->assign_inferred_type(cond_type);
  }

  void visit(V<ast_return_statement> v) override {
    parent::visit(v->get_return_value());
    if (current_function->does_return_self()) {
      if (!is_expr_valid_as_return_self(v->get_return_value())) {
        v->error("invalid return from `self` function");
      }
      return;
    }
    TypeExpr* expr_type = v->get_return_value()->inferred_type;
    TypeExpr* ret_type = current_function->full_type;
    if (ret_type->constr == TypeExpr::te_ForAll) {
      ret_type = ret_type->args[0];
    }
    tolk_assert(ret_type->constr == TypeExpr::te_Map);
    ret_type = ret_type->args[1];
    if (current_function->has_mutate_params()) {
      tolk_assert(ret_type->constr == TypeExpr::te_Tensor);
      ret_type = ret_type->args[ret_type->args.size() - 1];
    }
    try {
      unify(expr_type, ret_type);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "previous function return type " << ret_type
         << " cannot be unified with return statement expression type " << expr_type << ": " << ue;
      v->error(os.str());
    }
  }

  void visit(V<ast_local_var> v) override {
    if (v->var_maybe) {  // not underscore
      if (const auto* var_ref = v->var_maybe->try_as<LocalVarData>()) {
        v->mutate()->assign_inferred_type(var_ref->declared_type);
      } else if (const auto* glob_ref = v->var_maybe->try_as<GlobalVarData>()) {
        v->mutate()->assign_inferred_type(glob_ref->declared_type);
      } else {
        tolk_assert(0);
      }
    } else if (v->declared_type) {  // underscore with type
      v->mutate()->assign_inferred_type(v->declared_type);
    } else {  // just underscore
      v->mutate()->assign_inferred_type(TypeExpr::new_hole());
    }
    v->get_identifier()->mutate()->assign_inferred_type(v->inferred_type);
  }

  void visit(V<ast_local_vars_declaration> v) override {
    parent::visit(v->get_lhs());
    parent::visit(v->get_assigned_val());
    TypeExpr* lhs = v->get_lhs()->inferred_type;
    TypeExpr* rhs = v->get_assigned_val()->inferred_type;
    try {
      unify(lhs, rhs);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "cannot assign an expression of type " << rhs << " to a variable or pattern of type " << lhs << ": " << ue;
      v->error(os.str());
    }
  }

  void visit(V<ast_try_catch_statement> v) override {
    parent::visit(v->get_try_body());
    parent::visit(v->get_catch_expr());

    TypeExpr* tvm_error_type = TypeExpr::new_tensor(TypeExpr::new_var(), TypeExpr::new_atomic(TypeExpr::_Int));
    tolk_assert(v->get_catch_expr()->size() == 2);
    TypeExpr* type1 = v->get_catch_expr()->get_item(0)->inferred_type;
    unify(type1, tvm_error_type->args[1]);
    TypeExpr* type2 = v->get_catch_expr()->get_item(1)->inferred_type;
    unify(type2, tvm_error_type->args[0]);

    parent::visit(v->get_catch_body());
  }

  void visit(V<ast_throw_statement> v) override {
    parent::visit(v->get_thrown_code());
    if (!expect_integer(v->get_thrown_code())) {
      v->get_thrown_code()->error("excNo of `throw` must be an integer");
    }
    if (v->has_thrown_arg()) {
      parent::visit(v->get_thrown_arg());
    }
  }
  
  void visit(V<ast_assert_statement> v) override {
    parent::visit(v->get_cond());
    if (!expect_integer(v->get_cond())) {
      v->get_cond()->error("condition of `assert` must be an integer");
    }
    parent::visit(v->get_thrown_code());
  }

public:
  void start_visiting_function(V<ast_function_declaration> v_function) override {
    current_function = v_function->fun_ref;
    parent::visit(v_function->get_body());
    if (current_function->is_implicit_return()) {
      if (current_function->does_return_self()) {
        throw ParseError(v_function->get_body()->as<ast_sequence>()->loc_end, "missing return; forgot `return self`?");
      }
      TypeExpr* expr_type = TypeExpr::new_unit();
      TypeExpr* ret_type = current_function->full_type;
      if (ret_type->constr == TypeExpr::te_ForAll) {
        ret_type = ret_type->args[0];
      }
      tolk_assert(ret_type->constr == TypeExpr::te_Map);
      ret_type = ret_type->args[1];
      if (current_function->has_mutate_params()) {
        ret_type = ret_type->args[ret_type->args.size() - 1];
      }
      try {
        unify(expr_type, ret_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "implicit function return type " << expr_type
           << " cannot be unified with inferred return type " << ret_type << ": " << ue;
        v_function->error(os.str());
      }
    }
  }
};

void pipeline_infer_and_check_types(const AllSrcFiles& all_src_files) {
  visit_ast_of_all_functions<InferAndCheckTypesInsideFunctionVisitor>(all_src_files);
}

} // namespace tolk
