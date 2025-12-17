/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "ast.h"
#include "ast-visitor.h"
#include "compilation-errors.h"
#include "type-system.h"

/*
 *   This pipe checks lvalue/rvalue for validity.
 *   It happens after type inferring (after methods binding) and after lvalue/rvalue are refined based on fun_ref.
 *
 *   Example: `f() = 4`, `f()` was earlier marked as lvalue, it's incorrect.
 *   Example: `f(mutate 5)`, `5` was marked also, it's incorrect.
 */

namespace tolk {

static Error err_cannot_be_used_as_lvalue(const std::string& details) {
  // example: `f() = 32`
  // example: `loadUint(c.beginParse(), 32)` (since `loadUint()` mutates the first argument)
  return err("{} can not be used as lvalue", details);
}

static Error err_modifying_immutable_variable(LocalVarPtr var_ref) {
  if (var_ref->param_idx == 0 && var_ref->name == "self") {
    return err("modifying `self`, which is immutable by default; probably, you want to declare `mutate self`");
  } else {
    return err("modifying immutable variable `{}`", var_ref);
  }
}

static Error err_modifying_readonly_field(StructPtr struct_ref, StructFieldPtr field_ref) {
  return err("modifying readonly field `{}.{}`", struct_ref, field_ref);
}

// validate a function used as rvalue, like `var cb = f`
// it's not a generic function (ensured earlier at type inferring) and has some more restrictions
static void validate_function_used_as_noncall(FunctionPtr cur_f, AnyExprV v, FunctionPtr fun_ref) {
  if (!fun_ref->arg_order.empty() || !fun_ref->ret_order.empty()) {
    err("saving `{}` into a variable will most likely lead to invalid usage, since it changes the order of variables on the stack", fun_ref).fire(v, cur_f);
  }
  if (fun_ref->has_mutate_params()) {
    err("saving `{}` into a variable is impossible, since it has `mutate` parameters and thus can only be called directly", fun_ref).fire(v, cur_f);
  }
}

class CheckRValueLvalueVisitor final : public ASTVisitorFunctionBody {

  void on_var_used_as_lvalue(SrcRange range, LocalVarPtr var_ref) const {
    if (var_ref->is_immutable()) {
      err_modifying_immutable_variable(var_ref).fire(range, cur_f);
    }
    var_ref->mutate()->assign_used_as_lval();
  }

  void visit(V<ast_braced_expression> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("braced expression").fire(v, cur_f);
    }
    parent::visit(v);
  }

  void visit(V<ast_assign> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("assignment").fire(v, cur_f);
    }
    parent::visit(v);
  }

  void visit(V<ast_set_assign> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("assignment").fire(v, cur_f);
    }
    parent::visit(v);
  }

  void visit(V<ast_binary_operator> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("operator " + static_cast<std::string>(v->operator_name)).fire(v, cur_f);
    }
    parent::visit(v);
  }

  void visit(V<ast_unary_operator> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("operator " + static_cast<std::string>(v->operator_name)).fire(v, cur_f);
    }
    parent::visit(v);
  }

  void visit(V<ast_ternary_operator> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("operator ?:").fire(v, cur_f);
    }
    parent::visit(v);
  }

  void visit(V<ast_cast_as_operator> v) override {
    // if `x as int` is lvalue, then `x` is also lvalue, so check that `x` is ok
    parent::visit(v->get_expr());
  }

  void visit(V<ast_is_type_operator> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue(v->is_negated ? "operator !is" : "operator is").fire(v, cur_f);
    }
    parent::visit(v->get_expr());
  }

  void visit(V<ast_not_null_operator> v) override {
    // if `x!` is lvalue, then `x` is also lvalue, so check that `x` is ok
    parent::visit(v->get_expr());
  }

  void visit(V<ast_lazy_operator> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("lazy expression").fire(v, cur_f);
    }
    parent::visit(v->get_expr());
  }

  void visit(V<ast_int_const> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("literal").fire(v, cur_f);
    }
  }

  void visit(V<ast_string_const> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("literal").fire(v, cur_f);
    }
  }

  void visit(V<ast_bool_const> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("literal").fire(v, cur_f);
    }
  }

  void visit(V<ast_null_keyword> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("literal").fire(v, cur_f);
    }
  }

  void visit(V<ast_dot_access> v) override {
    // check for `immutableVal.field = rhs` or any other mutation of an immutable tensor/tuple/object
    // don't allow cheating like `((immutableVal!)).field = rhs`
    // same here: check for `obj.readonlyField = rhs` or any other mutation of a readonly field
    if (v->is_lvalue) {
      AnyExprV leftmost_obj = v;
      while (true) {
        if (auto as_dot = leftmost_obj->try_as<ast_dot_access>()) {
          if (as_dot->is_target_struct_field()) {
            StructFieldPtr field_ref = std::get<StructFieldPtr>(as_dot->target);
            const TypeDataStruct* obj_type = as_dot->get_obj()->inferred_type->unwrap_alias()->try_as<TypeDataStruct>();
            tolk_assert(obj_type);
            if (field_ref->is_readonly) {
              err_modifying_readonly_field(obj_type->struct_ref, field_ref).fire(as_dot, cur_f);
            }
          }

          leftmost_obj = as_dot->get_obj();
        } else if (auto as_par = leftmost_obj->try_as<ast_parenthesized_expression>()) {
          leftmost_obj = as_par->get_expr();
        } else if (auto as_cast = leftmost_obj->try_as<ast_cast_as_operator>()) {
          leftmost_obj = as_cast->get_expr();
        } else if (auto as_nn = leftmost_obj->try_as<ast_not_null_operator>()) {
          leftmost_obj = as_nn->get_expr();
        } else {
          break;
        }
      }

      if (auto as_ref = leftmost_obj->try_as<ast_reference>()) {
        if (LocalVarPtr var_ref = as_ref->sym->try_as<LocalVarPtr>()) {
          on_var_used_as_lvalue(leftmost_obj->range, var_ref);
        }
        if (as_ref->sym->try_as<const TypeReferenceUsedAsSymbol*>()) {  // `Point.create = f`
          if (v->is_target_enum_member()) {
            err("modifying immutable constant").fire(v, cur_f);
          }
          err("invalid left side of assignment").fire(v, cur_f);
        }
      }
    }

    // a reference to a method used as rvalue, like `var v = t.tupleAt`
    if (v->is_rvalue && v->is_target_fun_ref()) {
      validate_function_used_as_noncall(cur_f, v, std::get<FunctionPtr>(v->target));
    }

    parent::visit(v);
  }

  void visit(V<ast_function_call> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("function call").fire(v, cur_f);
    }
    if (!v->fun_maybe) {
      parent::visit(v->get_callee());
    }
    // for `f()` don't visit ast_reference `f`, to detect `f` usage as non-call, like `var cb = f`
    // same for `obj.method()`, don't visit ast_reference method, visit only obj
    if (AnyExprV self_obj = v->get_self_obj()) {
      parent::visit(self_obj);
    }

    for (int i = 0; i < v->get_num_args(); ++i) {
      parent::visit(v->get_arg(i));
    }
  }

  void visit(V<ast_match_expression> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("`match` expression").fire(v, cur_f);
    }
    parent::visit(v);
  }

  void visit(V<ast_local_var_lhs> v) override {
    if (v->marked_as_redef) {
      tolk_assert(v->var_ref);
      if (v->var_ref->is_immutable()) {
        err("`redef` for immutable variable").fire(v, cur_f);
      }
    }
  }

  void visit(V<ast_reference> v) override {
    if (v->is_lvalue) {
      tolk_assert(v->sym);
      if (LocalVarPtr var_ref = v->sym->try_as<LocalVarPtr>()) {
        on_var_used_as_lvalue(v->range, var_ref);
      } else if (v->sym->try_as<GlobalConstPtr>()) {
        err("modifying immutable constant").fire(v, cur_f);
      } else if (v->sym->try_as<FunctionPtr>()) {
        err("function can't be used as lvalue").fire(v, cur_f);
      }
    }

    // a reference to a function used as rvalue, like `var v = someFunction`
    if (FunctionPtr fun_ref = v->sym->try_as<FunctionPtr>(); fun_ref && v->is_rvalue) {
      validate_function_used_as_noncall(cur_f, v, fun_ref);
    }
  }

  void visit(V<ast_lambda_fun> v) override {
    if (v->is_lvalue) {
      err_cannot_be_used_as_lvalue("lambda").fire(v, cur_f);
    }
    // we don't traverse body: just detect `fun(){} = rhs`
  }

  void visit(V<ast_underscore> v) override {
    if (v->is_rvalue) {
      err("`_` can't be used as a value; it's a placeholder for a left side of assignment").fire(v, cur_f);
    }
  }

  void visit(V<ast_try_catch_statement> v) override {
    parent::visit(v->get_try_body());
    // skip catch(_,excNo), there are always vars due to grammar, lvalue/rvalue aren't set to them
    parent::visit(v->get_catch_body());
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }
};

void pipeline_check_rvalue_lvalue() {
  visit_ast_of_all_functions<CheckRValueLvalueVisitor>();
}

} // namespace tolk
