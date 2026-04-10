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
#include "smart-casts-cfg.h"
#include "type-system.h"

/*
 *   This pipe checks lvalue/rvalue for validity.
 *   It happens after type inferring (after methods binding) and after lvalue/rvalue are refined based on fun_ref.
 *
 *   Lvalue can only arise from three sources:
 *     1) LHS of assignment: `lhs = rhs`, `lhs += rhs`
 *     2) `mutate` argument: `f(mutate lhs)`
 *     3) `mutate self` method: `lhs.mutatingMethod()`
 *   For each source, is_valid_lvalue_path(lhs) is checked.
 *   Additionally, readonly fields and immutable variables are checked.
 */

namespace tolk {

static Error err_modifying_immutable_variable(LocalVarPtr var_ref) {
  if (var_ref->param_idx == 0 && var_ref->name == "self") {
    return err("modifying `self`, which is immutable by default; probably, you want to declare `mutate self`");
  } else {
    return err("modifying immutable variable `{}`\n""hint: it's declared `val {}`, keyword 'val' means 'immutable'\n""hint: declare `var {}` to allow modifications", var_ref, var_ref, var_ref);
  }
}

static Error err_modifying_readonly_field(StructPtr struct_ref, StructFieldPtr field_ref) {
  return err("modifying readonly field `{}.{}`", struct_ref, field_ref);
}

// validate a function used as rvalue, like `var cb = f`
// it's not a generic function (ensured earlier at type inferring) and has some more restrictions
static void validate_function_used_as_noncall(FunctionPtr cur_f, AnyExprV v, FunctionPtr fun_ref) {
  if (!fun_ref->arg_order.empty() || !fun_ref->ret_order.empty()) {
    err("saving `{}` into a variable will most likely lead to invalid usage, since it changes the order of variables on the stack", fun_ref).collect(v, cur_f);
  }
  if (fun_ref->has_mutate_params()) {
    err("saving `{}` into a variable is impossible, since it has `mutate` parameters and thus can only be called directly", fun_ref).collect(v, cur_f);
  }
}

class CheckRValueLvalueVisitor final : public ASTVisitorFunctionBody {

  void on_reference_used_as_lvalue(const Symbol* sym, SrcRange range) const {
    tolk_assert(sym != nullptr);

    if (LocalVarPtr var_ref = sym->try_as<LocalVarPtr>()) {
      // deny `v = rhs` / `mutate v` / `v.field = rhs` / etc. if v is immutable
      if (var_ref->is_immutable()) {
        err_modifying_immutable_variable(var_ref).collect(range, cur_f);
      }
      var_ref->mutate()->assign_used_as_lval();

    } else if (GlobalConstPtr const_ref = sym->try_as<GlobalConstPtr>()) {
      // deny `SOME_CONST = rhs` / `mutate CONST_TENSOR.0` / etc.
      err("modifying immutable constant `{}`", const_ref->name).collect(range, cur_f);

    } else if (GlobalVarPtr glob_ref = sym->try_as<GlobalVarPtr>()) {
      // fire on `global = rhs` in a @pure function: it's easier to do this check here,
      // because it's very similar to checking immutable variables, especially `(global!).field = rhs`
      if (cur_f->is_marked_as_pure()) {
        err("modifying a global `{}` in a pure function", glob_ref->name).collect(range, cur_f);
      }

    } else if (sym->try_as<const TypeReferenceUsedAsSymbol*>()) {
      // `Point.create = f` or `Enum.value = v`
      err("invalid left side of assignment").collect(range, cur_f);
    }
  }

  // for `v.field = rhs`, only `v.field` has is_lvalue=true, `v` itself is rvalue;
  // so visit(ast_reference) won't analyze `v`;
  // we need to manually walk the lvalue path `v.field` to find root references
  // to fire an error if `v` is immutable
  void check_lvalue_path_references(AnyExprV v) const {
    if (auto as_ref = v->try_as<ast_reference>()) {
      on_reference_used_as_lvalue(as_ref->sym, v->range);
    } else if (auto as_dot = v->try_as<ast_dot_access>()) {
      check_lvalue_path_references(as_dot->get_obj());
    } else if (auto as_nn = v->try_as<ast_not_null_operator>()) {
      check_lvalue_path_references(as_nn->get_expr());
    } else if (auto as_tensor = v->try_as<ast_tensor>()) {
      for (int i = 0; i < as_tensor->size(); ++i) {
        check_lvalue_path_references(as_tensor->get_item(i));
      }
    } else if (auto as_sq = v->try_as<ast_square_brackets>()) {
      for (int i = 0; i < as_sq->size(); ++i) {
        check_lvalue_path_references(as_sq->get_item(i));
      }
    }
  }

  // analyze `v.field` as lvalue (`v.field = rhs` / `mutate v.field` / `v.field.mutatingMethod`)
  void check_lvalue_dot_chain(V<ast_dot_access> v) const {
    // fire if `field` is readonly
    AnyExprV leftmost_obj = v;
    while (true) {
      if (auto as_dot = leftmost_obj->try_as<ast_dot_access>()) {
        if (as_dot->is_target_struct_field()) {
          StructFieldPtr field_ref = std::get<StructFieldPtr>(as_dot->target);
          const TypeDataStruct* obj_type = as_dot->get_obj()->inferred_type->unwrap_alias()->try_as<TypeDataStruct>();
          tolk_assert(obj_type);
          if (field_ref->is_readonly) {
            err_modifying_readonly_field(obj_type->struct_ref, field_ref).collect(as_dot, cur_f);
          }
        }
        leftmost_obj = as_dot->get_obj();
      } else if (auto as_nn = leftmost_obj->try_as<ast_not_null_operator>()) {
        leftmost_obj = as_nn->get_expr();
      } else {
        break;
      }
    }
    // fire if `v` is immutable (declared as `val`, not `var`)
    check_lvalue_path_references(leftmost_obj);
  }

  void visit(V<ast_assign> v) override {
    AnyExprV lhs = v->get_lhs();
    tolk_assert(lhs->is_lvalue);

    // allow `v.field = rhs`, but not `v.method().field = rhs`
    if (!is_valid_lvalue_path(lhs)) {
      err("can not assign to a temporary expression").collect(lhs, cur_f);
    }
    parent::visit(v);
  }

  void visit(V<ast_set_assign> v) override {
    AnyExprV lhs = v->get_lhs();
    tolk_assert(lhs->is_lvalue);

    // allow `v.field += rhs`, but not `v.method().field += rhs`
    if (!is_valid_lvalue_path(lhs)) {
      err("can not assign to a temporary expression").collect(lhs, cur_f);
    }
    parent::visit(v);
  }

  void visit(V<ast_dot_access> v) override {
    // dig into `v.field` if it's assigned/mutated
    if (v->is_lvalue) {
      check_lvalue_dot_chain(v);
    }

    // a reference to a method used as rvalue, like `var v = t.tupleAt`
    if (v->is_rvalue && v->is_target_fun_ref()) {
      validate_function_used_as_noncall(cur_f, v, std::get<FunctionPtr>(v->target));
    }

    parent::visit(v);
  }

  void visit(V<ast_function_call> v) override {
    if (!v->fun_maybe) {
      parent::visit(v->get_callee());
    }

    // allow `v.increment()`, but not `v.id().increment()`
    // (but still allow `beginCell().storeUint()`, because `beginCell()` is NOT marked lvalue previously)
    AnyExprV self_obj = v->get_self_obj();
    if (v->fun_maybe && v->fun_maybe->does_mutate_self() && self_obj && self_obj->is_lvalue) {
      if (!is_valid_lvalue_path(self_obj)) {
        err("can not mutate a temporary expression").collect(self_obj, cur_f);
      }
    }
    if (self_obj) {
      parent::visit(self_obj);
    }

    // allow `f(mutate v)`, but not `f(mutate v.id())`
    for (int i = 0; i < v->get_num_args(); ++i) {
      auto ith_arg = v->get_arg(i);
      if (ith_arg->passed_as_mutate && !is_valid_lvalue_path(ith_arg->get_expr())) {
        err("can not mutate a temporary expression").collect(ith_arg, cur_f);
      }
      parent::visit(ith_arg);
    }
  }

  void visit(V<ast_reference> v) override {
    // is_lvalue is true for `v = rhs` or `(v, w) = rhs`, but NOT for `v` inside `v.field = rhs`
    // (the latter is handled by check_lvalue_path_references above)
    if (v->is_lvalue) {
      on_reference_used_as_lvalue(v->sym, v->range);
    }

    // a reference to a function used as rvalue, like `var v = someFunction`
    if (FunctionPtr fun_ref = v->sym->try_as<FunctionPtr>(); fun_ref && v->is_rvalue) {
      validate_function_used_as_noncall(cur_f, v, fun_ref);
    }
  }

  void visit(V<ast_underscore> v) override {
    if (v->is_rvalue) {
      err("`_` can't be used as a value; it's a placeholder for a left side of assignment").collect(v, cur_f);
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
  CheckRValueLvalueVisitor visitor;
  visit_ast_of_all_functions(visitor);
}

} // namespace tolk
