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
#include "ast.h"
#include "ast-visitor.h"
#include "type-system.h"

namespace tolk {

GNU_ATTRIBUTE_NOINLINE
static std::string to_string(TypePtr type) {
  return "`" + type->as_human_readable() + "`";
}

GNU_ATTRIBUTE_NOINLINE
static std::string to_string(AnyExprV v_with_type) {
  return "`" + v_with_type->inferred_type->as_human_readable() + "`";
}

GNU_ATTRIBUTE_NOINLINE
static std::string to_string(std::string_view string_view) {
  return static_cast<std::string>(string_view);
}

GNU_ATTRIBUTE_NOINLINE
static std::string expression_as_string(AnyExprV v) {
  if (auto v_ref = v->try_as<ast_reference>()) {
    if (v_ref->sym->try_as<LocalVarPtr>() || v_ref->sym->try_as<GlobalVarPtr>()) {
      return "variable `" + static_cast<std::string>(v_ref->get_identifier()->name) + "`";
    }
  }
  if (auto v_par = v->try_as<ast_parenthesized_expression>()) {
    return expression_as_string(v_par->get_expr());
  }
  return "expression";
}

// fire an error on `!cell` / `+slice`
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_cannot_apply_operator(FunctionPtr cur_f, SrcLocation loc, std::string_view operator_name, AnyExprV unary_expr) {
  std::string op = static_cast<std::string>(operator_name);
  fire(cur_f, loc, "can not apply operator `" + op + "` to " + to_string(unary_expr->inferred_type));
}

// fire an error on `int + cell` / `slice & int`
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_cannot_apply_operator(FunctionPtr cur_f, SrcLocation loc, std::string_view operator_name, AnyExprV lhs, AnyExprV rhs) {
  std::string op = static_cast<std::string>(operator_name);
  fire(cur_f, loc, "can not apply operator `" + op + "` to " + to_string(lhs->inferred_type) + " and " + to_string(rhs->inferred_type));
}

GNU_ATTRIBUTE_NOINLINE
static void warning_condition_always_true_or_false(FunctionPtr cur_f, SrcLocation loc, AnyExprV cond, const char* operator_name) {
  bool no_warning = cond->kind == ast_bool_const || cond->kind == ast_int_const;
  if (no_warning) {     // allow `while(true)` without a warning
    return;
  }
  loc.show_warning("condition of " + static_cast<std::string>(operator_name) + " is always " + (cond->is_always_true ? "true" : "false"));
}

// given `f(x: int)` and a call `f(expr)`, check that expr_type is assignable to `int`
static void check_function_argument_passed(FunctionPtr cur_f, TypePtr param_type, AnyExprV ith_arg, bool is_obj_of_dot_call) {
  if (!param_type->can_rhs_be_assigned(ith_arg->inferred_type)) {
    if (is_obj_of_dot_call) {
      fire(cur_f, ith_arg->loc, "can not call method for " + to_string(param_type) + " with object of type " + to_string(ith_arg));
    } else {
      fire(cur_f, ith_arg->loc, "can not pass " + to_string(ith_arg) + " to " + to_string(param_type));
    }
  }
}

// given `f(x: mutate int?)` and a call `f(expr)`, check that `int?` is assignable to expr_type
// (for instance, can't call `f(mutate intVal)`, since f can potentially assign null to it)
static void check_function_argument_mutate_back(FunctionPtr cur_f, TypePtr param_type, AnyExprV ith_arg, bool is_obj_of_dot_call) {
  if (!ith_arg->inferred_type->can_rhs_be_assigned(param_type)) {
    if (is_obj_of_dot_call) {
      fire(cur_f, ith_arg->loc,"can not call method for mutate " + to_string(param_type) + " with object of type " + to_string(ith_arg) + ", because mutation is not type compatible");
    } else {
      fire(cur_f, ith_arg->loc,"can not pass " + to_string(ith_arg) + " to mutate " + to_string(param_type) + ", because mutation is not type compatible");
    }
  }
}

// fire an error on `var n = null`
// technically it's correct, type of `n` is TypeDataNullLiteral, but it's not what the user wanted
// so, it's better to see an error on assignment, that later, on `n` usage and types mismatch
// (most common is situation above, but generally, `var (x,n) = xn` where xn is a tensor with 2-nd always-null, can be)
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_assign_always_null_to_variable(FunctionPtr cur_f, SrcLocation loc, LocalVarPtr assigned_var, bool is_assigned_null_literal) {
  std::string var_name = assigned_var->name;
  fire(cur_f, loc, "can not infer type of `" + var_name + "`, it's always null\nspecify its type with `" + var_name + ": <type>`" + (is_assigned_null_literal ? " or use `null as <type>`" : ""));
}

// fire an error on `untypedTupleVar.0` when inferred as (int,int), or `[int, (int,int)]`, or other non-1 width in a tuple
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_cannot_put_non1_stack_width_arg_to_tuple(FunctionPtr cur_f, SrcLocation loc, TypePtr inferred_type) {
  fire(cur_f, loc, "a tuple can not have " + to_string(inferred_type) + " inside, because it occupies " + std::to_string(inferred_type->get_width_on_stack()) + " stack slots in TVM, not 1");
}

// handle __expect_type(expr, "type") call
// this is used in compiler tests
GNU_ATTRIBUTE_NOINLINE GNU_ATTRIBUTE_COLD
static void handle_possible_compiler_internal_call(FunctionPtr cur_f, V<ast_function_call> v) {
  FunctionPtr fun_ref = v->fun_maybe;
  tolk_assert(fun_ref && fun_ref->is_builtin_function());

  if (fun_ref->name == "__expect_type") {
    tolk_assert(v->get_num_args() == 2);
    std::string_view expected_type_str = v->get_arg(1)->get_expr()->as<ast_string_const>()->str_val;
    TypePtr expr_type = v->get_arg(0)->inferred_type;
    if (expected_type_str != expr_type->as_human_readable()) {
      fire(cur_f, v->loc, "__expect_type failed: expected `" + to_string(expected_type_str) + "`, got " + to_string(expr_type));
    }
  }
}

static bool expect_integer(TypePtr inferred_type) {
  if (inferred_type == TypeDataInt::create()) {
    return true;
  }
  if (inferred_type->try_as<TypeDataIntN>() || inferred_type == TypeDataCoins::create()) {
    return true;
  }
  if (const TypeDataAlias* as_alias = inferred_type->try_as<TypeDataAlias>()) {
    return expect_integer(as_alias->underlying_type);
  }
  return false;
}

static bool expect_integer(AnyExprV v_inferred) {
  return expect_integer(v_inferred->inferred_type);
}

static bool expect_boolean(TypePtr inferred_type) {
  if (inferred_type == TypeDataBool::create()) {
    return true;
  }
  if (const TypeDataAlias* as_alias = inferred_type->try_as<TypeDataAlias>()) {
    return expect_boolean(as_alias->underlying_type);
  }
  return false;
}

static bool expect_boolean(AnyExprV v_inferred) {
  return expect_boolean(v_inferred->inferred_type);
}

static bool expect_address(TypePtr inferred_type) {
  if (inferred_type == TypeDataAddress::create()) {
    return true;
  }
  if (const TypeDataAlias* as_alias = inferred_type->try_as<TypeDataAlias>()) {
    return expect_address(as_alias->underlying_type);
  }
  return false;
}

static bool expect_address(AnyExprV v_inferred) {
  return expect_address(v_inferred->inferred_type);
}

class CheckInferredTypesVisitor final : public ASTVisitorFunctionBody {
  FunctionPtr cur_f = nullptr;          // may be nullptr if checking `const a = ...` init_value

protected:
  void visit(V<ast_set_assign> v) override {
    AnyExprV lhs = v->get_lhs();
    AnyExprV rhs = v->get_rhs();
    parent::visit(lhs);
    parent::visit(rhs);

    // all operators (+=, etc.) can work for integers (if both sides are integers)
    // for intN, they are also allowed (int16 |= int8 is ok, since int16 | int8 is ok, all arithmetic is int)
    bool types_ok = expect_integer(lhs) && expect_integer(rhs);
    // bitwise operators &= |= ^= are "overloaded" for booleans also (if both sides are booleans)
    if (!types_ok && (v->tok == tok_set_bitwise_and || v->tok == tok_set_bitwise_or || v->tok == tok_set_bitwise_xor)) {
      types_ok = expect_boolean(lhs) && expect_boolean(rhs);
    }
    // using += for other types (e.g. `tensorVar += tensorVar`) is not allowed
    if (!types_ok) {
      fire_error_cannot_apply_operator(cur_f, v->loc, v->operator_name, lhs, rhs);
    }
  }

  void visit(V<ast_unary_operator> v) override {
    AnyExprV rhs = v->get_rhs();
    parent::visit(rhs);

    switch (v->tok) {
      case tok_logical_not:
        if (!expect_integer(rhs) && !expect_boolean(rhs)) {
          fire_error_cannot_apply_operator(cur_f, v->loc, v->operator_name, rhs);
        }
        break;
      default:
        if (!expect_integer(rhs)) {
          fire_error_cannot_apply_operator(cur_f, v->loc, v->operator_name, rhs);
        }
    }
  }

  void visit(V<ast_binary_operator> v) override {
    AnyExprV lhs = v->get_lhs();
    AnyExprV rhs = v->get_rhs();
    parent::visit(lhs);
    parent::visit(rhs);

    switch (v->tok) {
      // == != can compare both integers and booleans, (int == bool) is NOT allowed
      // for intN, it also works: (int8 == int16) is ok, (int == uint32) is ok
      // note, that `int?` and `int?` can't be compared, since Fift `EQUAL` works with integers only
      // (if to allow `int?`/`int8?` in the future, `==` must be expressed in a complicated Fift code considering TVM NULL)
      case tok_eq:
      case tok_neq: {
        bool both_int = expect_integer(lhs) && expect_integer(rhs);
        bool both_bool = expect_boolean(lhs) && expect_boolean(rhs);
        if (!both_int && !both_bool) {
          bool both_address = expect_address(lhs) && expect_address(rhs);
          if (both_address) {     // address can be compared with ==, but it's not integer comparison, it's handled specially
            v->mutate()->assign_fun_ref(nullptr);
          } else if (lhs->inferred_type->equal_to(rhs->inferred_type)) {  // compare slice with slice, int? with int?
            fire(cur_f, v->loc, "type " + to_string(lhs) + " can not be compared with `== !=`");
          } else {
            fire_error_cannot_apply_operator(cur_f, v->loc, v->operator_name, lhs, rhs);
          }
        }
        break;
      }
      // < > can compare only strict integers
      case tok_lt:
      case tok_gt:
      case tok_leq:
      case tok_geq:
      case tok_spaceship:
        if (!expect_integer(lhs) || !expect_integer(rhs)) {
          fire_error_cannot_apply_operator(cur_f, v->loc, v->operator_name, lhs, rhs);
        }
        break;
      // & | ^ are "overloaded" both for integers and booleans, (int & bool) is NOT allowed
      // they are allowed for intN (int16 & int32 is ok) and always "fall back" to general int
      case tok_bitwise_and:
      case tok_bitwise_or:
      case tok_bitwise_xor: {
        bool both_int = expect_integer(lhs) && expect_integer(rhs);
        bool both_bool = expect_boolean(lhs) && expect_boolean(rhs);
        if (!both_int && !both_bool) {
          fire_error_cannot_apply_operator(cur_f, v->loc, v->operator_name, lhs, rhs);
        }
        break;
      }
      // && || can work with integers and booleans, (int && bool) is allowed, (int16 && int32) also
      case tok_logical_and:
      case tok_logical_or: {
        bool lhs_ok = expect_integer(lhs) || expect_boolean(lhs);
        bool rhs_ok = expect_integer(rhs) || expect_boolean(rhs);
        if (!lhs_ok || !rhs_ok) {
          fire_error_cannot_apply_operator(cur_f, v->loc, v->operator_name, lhs, rhs);
        }
        break;
      }
      // others are mathematical: + * ...
      // they are allowed for intN (int16 + int32 is ok) and always "fall back" to general int
      default:
        if (!expect_integer(lhs) || !expect_integer(rhs)) {
          fire_error_cannot_apply_operator(cur_f, v->loc, v->operator_name, lhs, rhs);
        }
    }
  }

  void visit(V<ast_cast_as_operator> v) override {
    parent::visit(v->get_expr());

    if (!v->get_expr()->inferred_type->can_be_casted_with_as_operator(v->type_node->resolved_type)) {
      fire(cur_f, v->loc, "type " + to_string(v->get_expr()) + " can not be cast to " + to_string(v->type_node->resolved_type));
    }
  }

  void visit(V<ast_is_type_operator> v) override {
    parent::visit(v->get_expr());
    TypePtr rhs_type = v->type_node->resolved_type;

    if (rhs_type->unwrap_alias()->try_as<TypeDataUnion>()) {   // `v is T1 | T2` / `v is T?` is disallowed
      fire(cur_f, v->loc, "union types are not allowed, use concrete types in `is`");
    }

    if ((v->is_always_true && !v->is_negated) || (v->is_always_false && v->is_negated)) {
      v->loc.show_warning(expression_as_string(v->get_expr()) + " is always " + to_string(rhs_type) + ", this condition is always " + (v->is_always_true ? "true" : "false"));
    }
    if ((v->is_always_false && !v->is_negated) || (v->is_always_true && v->is_negated)) {
      v->loc.show_warning(expression_as_string(v->get_expr()) + " of type " + to_string(v->get_expr()) + " can never be " + to_string(rhs_type) + ", this condition is always " + (v->is_always_true ? "true" : "false"));
    }
  }

  void visit(V<ast_not_null_operator> v) override {
    parent::visit(v->get_expr());

    if (v->get_expr()->inferred_type == TypeDataNullLiteral::create()) {
      // operator `!` used for always-null (proven by smart casts, for example), it's an error
      fire(cur_f, v->loc, "operator `!` used for always null expression");
    }
    // if operator `!` used for non-nullable, probably a warning should be printed
  }

  void visit(V<ast_bracket_tuple> v) override {
    parent::visit(v);

    for (int i = 0; i < v->size(); ++i) {
      AnyExprV item = v->get_item(i);
      if (item->inferred_type->get_width_on_stack() != 1) {
        fire_error_cannot_put_non1_stack_width_arg_to_tuple(cur_f, v->get_item(i)->loc, item->inferred_type);
      }
    }
  }

  void visit(V<ast_dot_access> v) override {
    parent::visit(v);

    if (v->is_target_indexed_access()) {
      TypePtr obj_type = v->get_obj()->inferred_type->unwrap_alias();
      if (v->inferred_type->get_width_on_stack() != 1 && (obj_type->try_as<TypeDataTuple>() || obj_type->try_as<TypeDataBrackets>())) {
        fire_error_cannot_put_non1_stack_width_arg_to_tuple(cur_f, v->loc, v->inferred_type);
      }
    }
  }

  void visit(V<ast_function_call> v) override {
    parent::visit(v);   // check against type mismatch inside nested arguments

    FunctionPtr fun_ref = v->fun_maybe;
    if (!fun_ref) {
      // `local_var(args)` and similar
      const TypeDataFunCallable* f_callable = v->get_callee()->inferred_type->unwrap_alias()->try_as<TypeDataFunCallable>();
      tolk_assert(f_callable && f_callable->params_size() == v->get_num_args());
      for (int i = 0; i < v->get_num_args(); ++i) {
        auto arg_i = v->get_arg(i)->get_expr();
        TypePtr param_type = f_callable->params_types[i];
        if (!param_type->can_rhs_be_assigned(arg_i->inferred_type)) {
          fire(cur_f, arg_i->loc, "can not pass " + to_string(arg_i) + " to " + to_string(param_type));
        }
      }
      return;
    }

    // so, we have a call `f(args)` or `obj.f(args)`, fun_ref is a function/method (code / asm / builtin)
    AnyExprV self_obj = v->get_self_obj();
    int delta_self = self_obj != nullptr;

    if (self_obj) {
      const LocalVarData& param_0 = fun_ref->parameters[0];
      TypePtr param_type = param_0.declared_type;
      check_function_argument_passed(cur_f, param_type, self_obj, true);
      if (param_0.is_mutate_parameter()) {
        check_function_argument_mutate_back(cur_f, param_type, self_obj, true);
      }
    }
    for (int i = 0; i < v->get_num_args(); ++i) {
      const LocalVarData& param_i = fun_ref->parameters[delta_self + i];
      AnyExprV arg_i = v->get_arg(i)->get_expr();
      TypePtr param_type = param_i.declared_type;
      check_function_argument_passed(cur_f, param_type, arg_i, false);
      if (param_i.is_mutate_parameter()) {
        check_function_argument_mutate_back(cur_f, param_type, arg_i, false);
      }
    }

    if (fun_ref->is_builtin_function() && fun_ref->name[0] == '_') {
      handle_possible_compiler_internal_call(cur_f, v);
    }
  }

  void visit(V<ast_assign> v) override {
    parent::visit(v->get_lhs());
    parent::visit(v->get_rhs());

    process_assignment_lhs(v->get_lhs(), v->get_rhs()->inferred_type, v->get_rhs());
  }

  // handle (and dig recursively) into `var lhs = rhs`
  // examples: `var z = 5`, `var (x, [y]) = (2, [3])`, `var (x, [y]) = xy`
  // while recursing, keep track of rhs if lhs and rhs have common shape (5 for z, 2 for x, [3] for [y], 3 for y)
  // (so that on type mismatch, point to corresponding rhs, example: `var (x, y:slice) = (1, 2)` point to 2
  void process_assignment_lhs(AnyExprV lhs, TypePtr rhs_type, AnyExprV corresponding_maybe_rhs) {
    AnyExprV err_loc = corresponding_maybe_rhs ? corresponding_maybe_rhs : lhs;

    // `var ... = rhs` - dig into left part
    if (auto lhs_decl = lhs->try_as<ast_local_vars_declaration>()) {
      process_assignment_lhs(lhs_decl->get_expr(), rhs_type, corresponding_maybe_rhs);
      return;
    }

    // inside `var v: int = rhs` / `var _ = rhs` / `var v redef = rhs` (lhs is "v" / "_" / "v")
    if (auto lhs_var = lhs->try_as<ast_local_var_lhs>()) {
      TypePtr declared_type = lhs_var->type_node ? lhs_var->type_node->resolved_type : nullptr;
      if (lhs_var->marked_as_redef) {
        tolk_assert(lhs_var->var_ref && lhs_var->var_ref->declared_type);
        declared_type = lhs_var->var_ref->declared_type;
      }
      if (declared_type) {
        if (!declared_type->can_rhs_be_assigned(rhs_type)) {
          fire(cur_f, err_loc->loc, "can not assign " + to_string(rhs_type) + " to variable of type " + to_string(declared_type));
        }
      } else {
        if (rhs_type == TypeDataNullLiteral::create()) {
          fire_error_assign_always_null_to_variable(cur_f, err_loc->loc, lhs_var->var_ref->try_as<LocalVarPtr>(), corresponding_maybe_rhs && corresponding_maybe_rhs->kind == ast_null_keyword);
        }
      }
      return;
    }

    // `(v1, v2) = rhs` / `var (v1, v2) = rhs` (rhs may be `(1,2)` or `tensorVar` or `someF()`, doesn't matter)
    // dig recursively into v1 and v2 with corresponding rhs i-th item of a tensor
    if (auto lhs_tensor = lhs->try_as<ast_tensor>()) {
      const TypeDataTensor* rhs_type_tensor = rhs_type->unwrap_alias()->try_as<TypeDataTensor>();
      if (!rhs_type_tensor) {
        fire(cur_f, err_loc->loc, "can not assign " + to_string(rhs_type) + " to a tensor");
      }
      if (lhs_tensor->size() != rhs_type_tensor->size()) {
        fire(cur_f, err_loc->loc, "can not assign " + to_string(rhs_type) + ", sizes mismatch");
      }
      V<ast_tensor> rhs_tensor_maybe = corresponding_maybe_rhs ? corresponding_maybe_rhs->try_as<ast_tensor>() : nullptr;
      for (int i = 0; i < lhs_tensor->size(); ++i) {
        process_assignment_lhs(lhs_tensor->get_item(i), rhs_type_tensor->items[i], rhs_tensor_maybe ? rhs_tensor_maybe->get_item(i) : nullptr);
      }
      return;
    }

    // `[v1, v2] = rhs` / `var [v1, v2] = rhs` (rhs may be `[1,2]` or `tupleVar` or `someF()`, doesn't matter)
    // dig recursively into v1 and v2 with corresponding rhs i-th item of a tuple
    if (auto lhs_tuple = lhs->try_as<ast_bracket_tuple>()) {
      const TypeDataBrackets* rhs_type_tuple = rhs_type->unwrap_alias()->try_as<TypeDataBrackets>();
      if (!rhs_type_tuple) {
        fire(cur_f, err_loc->loc, "can not assign " + to_string(rhs_type) + " to a tuple");
      }
      if (lhs_tuple->size() != rhs_type_tuple->size()) {
        fire(cur_f, err_loc->loc, "can not assign " + to_string(rhs_type) + ", sizes mismatch");
      }
      V<ast_bracket_tuple> rhs_tuple_maybe = corresponding_maybe_rhs ? corresponding_maybe_rhs->try_as<ast_bracket_tuple>() : nullptr;
      for (int i = 0; i < lhs_tuple->size(); ++i) {
        process_assignment_lhs(lhs_tuple->get_item(i), rhs_type_tuple->items[i], rhs_tuple_maybe ? rhs_tuple_maybe->get_item(i) : nullptr);
      }
      return;
    }

    // check `untypedTuple.0 = rhs_tensor` and other non-1 width elements
    if (auto lhs_dot = lhs->try_as<ast_dot_access>()) {
      if (lhs_dot->is_target_indexed_access() && lhs_dot->get_obj()->inferred_type->unwrap_alias() == TypeDataTuple::create()) {
        if (rhs_type->get_width_on_stack() != 1) {
          fire_error_cannot_put_non1_stack_width_arg_to_tuple(cur_f, err_loc->loc, rhs_type);
        }
      }
    }

    // here is `v = rhs` (just assignment, not `var v = rhs`) / `a.0 = rhs` / `getObj(z=f()).0 = rhs` etc.
    // types were already inferred, so just check their compatibility
    // for strange lhs like `f() = rhs` type checking will pass, but will fail lvalue check later
    if (!lhs->inferred_type->can_rhs_be_assigned(rhs_type)) {
      if (lhs->try_as<ast_reference>()) {
        fire(cur_f, err_loc->loc, "can not assign " + to_string(rhs_type) + " to variable of type " + to_string(lhs));
      } else if (lhs->try_as<ast_dot_access>()) {
        fire(cur_f, err_loc->loc, "can not assign " + to_string(rhs_type) + " to field of type " + to_string(lhs));
      } else {
        fire(cur_f, err_loc->loc, "can not assign " + to_string(rhs_type) + " to " + to_string(lhs));
      }
    }
  }

  void visit(V<ast_return_statement> v) override {
    parent::visit(v->get_return_value());

    if (cur_f->does_return_self()) {
      if (!is_expr_valid_as_return_self(v->get_return_value())) {
        fire(cur_f, v->loc, "invalid return from `self` function");
      }
      return;
    }

    TypePtr expr_type = v->get_return_value()->inferred_type;
    if (!cur_f->inferred_return_type->can_rhs_be_assigned(expr_type)) {
      fire(cur_f, v->get_return_value()->loc, "can not convert type " + to_string(expr_type) + " to return type " + to_string(cur_f->inferred_return_type));
    }
  }

  static bool is_expr_valid_as_return_self(AnyExprV return_expr) {
    // `return self`
    if (return_expr->kind == ast_reference && return_expr->as<ast_reference>()->get_name() == "self") {
      return true;
    }
    // `return self.someMethod()`
    if (auto v_call = return_expr->try_as<ast_function_call>(); v_call && v_call->get_self_obj()) {
      return v_call->fun_maybe && v_call->fun_maybe->does_return_self() && is_expr_valid_as_return_self(v_call->get_self_obj());
    }
    // `return cond ? ... : ...`
    if (auto v_ternary = return_expr->try_as<ast_ternary_operator>()) {
      return is_expr_valid_as_return_self(v_ternary->get_when_true()) && is_expr_valid_as_return_self(v_ternary->get_when_false());
    }
    return false;
  }

  void visit(V<ast_ternary_operator> v) override {
    parent::visit(v);

    AnyExprV cond = v->get_cond();
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      fire(cur_f, cond->loc, "can not use " + to_string(cond) + " as a boolean condition");
    }

    if (cond->is_always_true || cond->is_always_false) {
      warning_condition_always_true_or_false(cur_f, v->loc, cond, "ternary operator");
    }
  }

  void visit(V<ast_match_expression> v) override {
    parent::visit(v);

    bool has_type_arm = false;
    bool has_expr_arm = false;
    bool has_else_arm = false;
    AnyExprV v_subject = v->get_subject();
    TypePtr subject_type = v_subject->inferred_type->unwrap_alias();
    const TypeDataUnion* subject_union = subject_type->try_as<TypeDataUnion>();

    std::vector<int> covered_type_ids;    // for type-based `match`, what types are on the left of `=>`

    for (int i = 0; i < v->get_arms_count(); ++i) {
      auto v_arm = v->get_arm(i);
      switch (v_arm->pattern_kind) {
        case MatchArmKind::exact_type: {
          if (has_expr_arm) {
            fire(cur_f, v_arm->loc, "can not mix type and expression patterns in `match`");
          }
          if (has_else_arm) {
            fire(cur_f, v_arm->loc, "`else` branch should be the last");
          }
          has_type_arm = true;

          TypePtr lhs_type = v_arm->pattern_type_node->resolved_type->unwrap_alias();   // `lhs_type => ...`
          if (lhs_type->try_as<TypeDataUnion>()) {
            fire(cur_f, v_arm->loc, "wrong pattern matching: union types are not allowed, use concrete types in `match`");
          }
          bool can_happen = (subject_union && subject_union->has_variant_with_type_id(lhs_type)) ||
                           (!subject_union && subject_type->equal_to(lhs_type));
          if (!can_happen) {
            fire(cur_f, v_arm->loc, "wrong pattern matching: " + to_string(lhs_type) + " is not a variant of " + to_string(subject_type));
          }
          if (std::find(covered_type_ids.begin(), covered_type_ids.end(), lhs_type->get_type_id()) != covered_type_ids.end()) {
            fire(cur_f, v_arm->loc, "wrong pattern matching: duplicated " + to_string(lhs_type));
          }
          covered_type_ids.push_back(lhs_type->get_type_id());
          break;
        }
        case MatchArmKind::const_expression: {
          if (has_type_arm) {
            fire(cur_f, v_arm->loc, "can not mix type and expression patterns in `match`");
          }
          if (has_else_arm) {
            fire(cur_f, v_arm->loc, "`else` branch should be the last");
          }
          has_expr_arm = true;
          TypePtr pattern_type = v_arm->get_pattern_expr()->inferred_type->unwrap_alias();
          bool both_int = expect_integer(pattern_type) && expect_integer(subject_type);
          bool both_bool = expect_boolean(pattern_type) && expect_boolean(subject_type);
          if (!both_int && !both_bool) {
            if (pattern_type->equal_to(subject_type)) {   // `match` over `slice` etc., where operator `==` can't be applied
              fire(cur_f, v_arm->loc, "wrong pattern matching: can not compare type " + to_string(subject_type) + " in `match`");
            } else {
              fire(cur_f, v_arm->loc, "wrong pattern matching: can not compare type " + to_string(v_arm->get_pattern_expr()) + " with match subject of type " + to_string(v_subject));
            }
          }
          break;
        }
        default:
          if (has_else_arm) {
            fire(cur_f, v_arm->loc, "duplicated `else` branch");
          }
          if (has_type_arm) {
            fire(cur_f, v_arm->loc, "`else` is not allowed in `match` by type; you should cover all possible types");
          }
          has_else_arm = true;
      }
    }

    // fire if `match` by type is not exhaustive
    if (has_type_arm && subject_union && subject_union->variants.size() != covered_type_ids.size()) {
      std::string missing;
      for (TypePtr variant : subject_union->variants) {
        if (std::find(covered_type_ids.begin(), covered_type_ids.end(), variant->get_type_id()) == covered_type_ids.end()) {
          if (!missing.empty()) {
            missing += ", ";
          }
          missing += to_string(variant);
        }
      }
      fire(cur_f, v->loc, "`match` does not cover all possible types; missing types are: " + missing);
    }
    // `match` by expression, if it's not statement, should have `else` (unless it's match over bool with const true/false)
    if (has_expr_arm && !has_else_arm && !v->is_statement()) {
      bool needs_else_branch = true;
      if (expect_boolean(subject_type) && v->get_arms_count() == 2) {
        auto arm0 = v->get_arm(0)->get_pattern_expr()->try_as<ast_bool_const>();
        auto arm1 = v->get_arm(1)->get_pattern_expr()->try_as<ast_bool_const>();
        needs_else_branch = !(arm0 && arm1 && arm0->bool_val != arm1->bool_val);
      }
      if (needs_else_branch) {
        fire(cur_f, v->loc, "`match` expression should have `else` branch");
      }
    }
  }

  void visit(V<ast_object_field> v) override {
    parent::visit(v->get_init_val());

    if (!v->field_ref->declared_type->can_rhs_be_assigned(v->get_init_val()->inferred_type)) {
      fire(cur_f, v->get_init_val()->loc, "can not assign " + to_string(v->get_init_val()) + " to field of type " + to_string(v->field_ref->declared_type));
    }
  }

  void visit(V<ast_if_statement> v) override {
    parent::visit(v);

    AnyExprV cond = v->get_cond();
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      fire(cur_f, cond->loc, "can not use " + to_string(cond) + " as a boolean condition");
    }

    if (cond->is_always_true || cond->is_always_false) {
      warning_condition_always_true_or_false(cur_f, v->loc, cond, "`if`");
    }
  }

  void visit(V<ast_repeat_statement> v) override {
    parent::visit(v);

    AnyExprV cond = v->get_cond();
    if (!expect_integer(cond)) {
      fire(cur_f, cond->loc, "condition of `repeat` must be an integer, got " + to_string(cond));
    }
  }

  void visit(V<ast_while_statement> v) override {
    parent::visit(v);

    AnyExprV cond = v->get_cond();
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      fire(cur_f, cond->loc, "can not use " + to_string(cond) + " as a boolean condition");
    }

    if (cond->is_always_true || cond->is_always_false) {
      warning_condition_always_true_or_false(cur_f, v->loc, cond, "`while`");
    }
  }

  void visit(V<ast_do_while_statement> v) override {
    parent::visit(v);

    AnyExprV cond = v->get_cond();
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      fire(cur_f, cond->loc, "can not use " + to_string(cond) + " as a boolean condition");
    }

    if (cond->is_always_true || cond->is_always_false) {
      warning_condition_always_true_or_false(cur_f, v->loc, cond, "`do while`");
    }
  }

  void visit(V<ast_throw_statement> v) override {
    parent::visit(v);

    if (!expect_integer(v->get_thrown_code())) {
      fire(cur_f, v->get_thrown_code()->loc, "excNo of `throw` must be an integer, got " + to_string(v->get_thrown_code()));
    }
    if (v->has_thrown_arg() && v->get_thrown_arg()->inferred_type->get_width_on_stack() != 1) {
      fire(cur_f, v->get_thrown_arg()->loc, "can not throw " + to_string(v->get_thrown_arg()) + ", exception arg must occupy exactly 1 stack slot");
    }
  }

  void visit(V<ast_assert_statement> v) override {
    parent::visit(v);

    AnyExprV cond = v->get_cond();
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      fire(cur_f, cond->loc, "can not use " + to_string(cond) + " as a boolean condition");
    }
    if (!expect_integer(v->get_thrown_code())) {
      fire(cur_f, v->get_thrown_code()->loc, "thrown excNo of `assert` must be an integer, got " + to_string(v->get_thrown_code()));
    }

    if (cond->is_always_true || cond->is_always_false) {
      warning_condition_always_true_or_false(cur_f, v->loc, cond, "`assert`");
    }
  }

  void visit(V<ast_block_statement> v) override {
    parent::visit(v);

    if (v->first_unreachable) {
      // it's essential to print "unreachable code" warning AFTER type checking
      // (printing it while inferring might be a false positive if types are incorrect, due to smart casts for example)
      // a more correct approach would be to access cfg here somehow, but since cfg is now available only while inferring,
      // a special v->first_unreachable was set specifically for this warning (again, which is correct if types match)
      v->first_unreachable->loc.show_warning("unreachable code");
    }
  }

 public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }

  void start_visiting_function(FunctionPtr fun_ref, V<ast_function_declaration> v_function) override {
    cur_f = fun_ref;
    parent::visit(v_function->get_body());
    cur_f = nullptr;

    if (fun_ref->is_implicit_return() && fun_ref->declared_return_type) {
      if (!fun_ref->declared_return_type->can_rhs_be_assigned(TypeDataVoid::create()) || fun_ref->does_return_self()) {
        fire(fun_ref, v_function->get_body()->as<ast_block_statement>()->loc_end, "missing return");
      }
    }

    // visit default values of parameters
    for (int i = 0; i < fun_ref->get_num_params(); ++i) {
      if (LocalVarPtr param_ref = &fun_ref->get_param(i); param_ref->has_default_value()) {
        parent::visit(param_ref->default_value);

        TypePtr inferred_type = param_ref->default_value->inferred_type;
        if (!param_ref->declared_type->can_rhs_be_assigned(inferred_type)) {
          throw ParseError(param_ref->loc, "can not assign " + to_string(inferred_type) + " to " + to_string(param_ref->declared_type));
        }
      }
    }
  }

  // given `const a = 2 + 3` check types within its init_value
  // so, `const a = 1 + some_slice` will fire a reasonable error
  void start_visiting_constant(GlobalConstPtr const_ref) {
    parent::visit(const_ref->init_value);

    // if no errors occurred, init value has correct type
    // (though it may not be a valid constant expression, this would be checked after type inferring)
    if (const_ref->declared_type) {     // `const a: int = ...`
      TypePtr inferred_type = const_ref->init_value->inferred_type;
      if (!const_ref->declared_type->can_rhs_be_assigned(inferred_type)) {
        throw ParseError(const_ref->loc, "can not assign " + to_string(inferred_type) + " to " + to_string(const_ref->declared_type));
      }
    }
  }

  // given struct field `a: int = 2 + 3` check types within its default_value
  void start_visiting_field_default(StructFieldPtr field_ref) {
    parent::visit(field_ref->default_value);

    TypePtr inferred_type = field_ref->default_value->inferred_type;
    if (!field_ref->declared_type->can_rhs_be_assigned(inferred_type)) {
      throw ParseError(field_ref->loc, "can not assign " + to_string(inferred_type) + " to " + to_string(field_ref->declared_type));
    }
  }
};

void pipeline_check_inferred_types() {
  visit_ast_of_all_functions<CheckInferredTypesVisitor>();

  CheckInferredTypesVisitor visitor;
  for (GlobalConstPtr const_ref : get_all_declared_constants()) {
    visitor.start_visiting_constant(const_ref);
  }
  for (StructPtr struct_ref : get_all_declared_structs()) {
    for (StructFieldPtr field_ref : struct_ref->fields) {
      if (field_ref->has_default_value() && !struct_ref->is_generic_struct()) {
        visitor.start_visiting_field_default(field_ref);
      }
    }
  }
}

} // namespace tolk
