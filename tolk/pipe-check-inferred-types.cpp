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
#include "compilation-errors.h"
#include "type-system.h"

namespace tolk {

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

// make a general error on type mismatch; for example, "can not assign `cell` to `slice`";
// for instance, if `as` operator is applicable, compiler will suggest it
static Error err_type_mismatch(const char* text_tpl, TypePtr src, TypePtr dst) {
#ifdef TOLK_DEBUG
  tolk_assert(!dst->can_rhs_be_assigned(src));
#endif
  std::string message = text_tpl;
  message.replace(message.find("{src}"), 5, "`" + src->as_human_readable() + "`");
  message.replace(message.find("{dst}"), 5, "`" + dst->as_human_readable() + "`");
  if (src->can_be_casted_with_as_operator(dst)) {
    bool suggest_as = !dst->try_as<TypeDataTensor>() && !dst->try_as<TypeDataBrackets>();
    if ((src == TypeDataSlice::create() || dst == TypeDataSlice::create()) && (src->try_as<TypeDataAddress>() || dst->try_as<TypeDataAddress>())) {
      message += "\n""hint: unlike FunC, Tolk has a special type `address` (which is slice at the TVM level);";
      message += "\n""      most likely, you just need `address` everywhere";
      message += "\n""hint: alternatively, use `as` operator for UNSAFE casting: `<some_expr> as " + dst->as_human_readable() + "`";
    } else if (src == TypeDataAddress::any() && dst == TypeDataAddress::internal()) {
      message += "\n""hint: use `any_addr.castToInternal()` to check and get `address`";
      message += "\n""hint: alternatively, use `as` operator for UNSAFE casting: `<some_expr> as " + dst->as_human_readable() + "`";
    } else if (src == TypeDataAddress::internal() && dst == TypeDataAddress::any()) {
      message += "\n""hint: use `as` operator for conversion: `<some_expr> as any_address`";
    } else if (suggest_as) {
      message += "\n""hint: use `as` operator for UNSAFE casting: `<some_expr> as " + dst->as_human_readable() + "`";
    }
    if (src == TypeDataBool::create() && dst == TypeDataInt::create()) {
      message += "\n""caution! in TVM, bool TRUE is -1, not 1";
    }
  }
  if (const TypeDataUnion* src_nullable = src->try_as<TypeDataUnion>(); src_nullable && src_nullable->or_null) {
    if (dst->can_rhs_be_assigned(src_nullable->or_null)) {
      message += "\n""hint: probably, you should check on null";
      message += "\n""hint: alternatively, use `!` operator to bypass nullability checks: `<some_expr>!`";
    }
  }
  return err("{}", message);
}

// make an error on `!cell` / `+slice`
static Error err_cannot_apply_operator(std::string_view operator_name, AnyExprV unary_expr) {
  return err("can not apply operator `{}` to `{}`", operator_name, unary_expr->inferred_type);
}

// make an error on `int + cell` / `slice & int`
static Error err_cannot_apply_operator(std::string_view operator_name, AnyExprV lhs, AnyExprV rhs) {
  const TypeDataUnion* lhs_nullable = lhs->inferred_type->unwrap_alias()->try_as<TypeDataUnion>();
  const TypeDataUnion* rhs_nullable = rhs->inferred_type->unwrap_alias()->try_as<TypeDataUnion>();
  std::string hint = lhs_nullable || rhs_nullable ? "\n""hint: check on `null` first, or use unsafe operator `!`" : "";
  return err("can not apply operator `{}` to `{}` and `{}`{}", operator_name, lhs->inferred_type, rhs->inferred_type, hint);
}

GNU_ATTRIBUTE_NOINLINE
static void warning_condition_always_true_or_false(FunctionPtr cur_f, SrcRange keyword_range, AnyExprV cond, const char* operator_name) {
  bool no_warning = cond->kind == ast_bool_const || cond->kind == ast_int_const;
  if (no_warning) {     // allow `while(true)` without a warning
    return;
  }
  err("condition of {} is always {}", operator_name, cond->is_always_true).warning(keyword_range, cur_f);
}

// given `f(x: int)` and a call `f(expr)`, check that expr_type is assignable to `int`
static void check_function_argument_passed(FunctionPtr cur_f, TypePtr param_type, AnyExprV ith_arg, bool is_obj_of_dot_call) {
  if (!param_type->can_rhs_be_assigned(ith_arg->inferred_type)) {
    if (is_obj_of_dot_call) {
      err("can not call method for `{}` with object of type `{}`", param_type, ith_arg->inferred_type).fire(ith_arg, cur_f);
    } else {
      err_type_mismatch("can not pass {src} to {dst}", ith_arg->inferred_type, param_type).fire(ith_arg, cur_f);
    }
  }
}

// given `f(x: mutate int?)` and a call `f(expr)`, check that `int?` is assignable to expr_type
// (for instance, can't call `f(mutate intVal)`, since f can potentially assign null to it)
static void check_function_argument_mutate_back(FunctionPtr cur_f, TypePtr param_type, AnyExprV ith_arg, bool is_obj_of_dot_call) {
  if (!ith_arg->inferred_type->can_rhs_be_assigned(param_type)) {
    if (is_obj_of_dot_call) {
      err("can not call method for mutate `{}` with object of type `{}`, because mutation is not type compatible", param_type, ith_arg->inferred_type).fire(ith_arg, cur_f);
    } else {
      err("can not pass `{}` to mutate `{}`, because mutation is not type compatible", ith_arg->inferred_type, param_type).fire(ith_arg, cur_f);
    }
  }
}

// make an error on `var n = null`
// technically it's correct, type of `n` is TypeDataNullLiteral, but it's not what the user wanted
// so, it's better to see an error on assignment, that later, on `n` usage and types mismatch
// (most common is situation above, but generally, `var (x,n) = xn` where xn is a tensor with 2-nd always-null, can be)
static Error err_assign_always_null_to_variable(LocalVarPtr assigned_var, bool is_assigned_null_literal) {
  return err("can not infer type of `{}`, it's always null\nspecify its type with `{}: <type>`{}", assigned_var, assigned_var, (is_assigned_null_literal ? " or use `null as <type>`" : ""));
}

// make an error on `untypedTupleVar.0` when inferred as (int,int), or `[int, (int,int)]`, or other non-1 width in a tuple
static Error err_cannot_put_non1_stack_width_arg_to_tuple(TypePtr inferred_type) {
  return err("a tuple can not have `{}` inside, because it occupies {} stack slots in TVM, not 1", inferred_type, inferred_type->get_width_on_stack());
}

// handle __expect_type(expr, "type") call
// this is used in compiler tests
GNU_ATTRIBUTE_NOINLINE GNU_ATTRIBUTE_COLD
static void handle_possible_compiler_internal_call(FunctionPtr cur_f, V<ast_function_call> v) {
  FunctionPtr fun_ref = v->fun_maybe;
  tolk_assert(fun_ref && fun_ref->is_builtin());

  if (fun_ref->name == "__expect_type" && v->get_num_args() == 2) {
    // __expect_type(expr, "...") is a compiler built-in for testing, it's not indented to be called by users
    auto v_expected_str = v->get_arg(1)->get_expr()->try_as<ast_string_const>();
    tolk_assert(v_expected_str && "invalid __expect_type");
    TypePtr expr_type = v->get_arg(0)->inferred_type;
    if (v_expected_str->str_val != expr_type->as_human_readable()) {
      err("__expect_type failed: expected `{}`, got `{}`", v_expected_str->str_val, expr_type).fire(v, cur_f);
    }
  }
}

// detect `if (x = 1)` having its condition to fire a warning;
// note that `if ((x = f()) == null)` and other usages of assignment is rvalue is okay
static bool is_assignment_inside_condition(AnyExprV cond) {
  while (auto v_par = cond->try_as<ast_parenthesized_expression>()) {
    cond = v_par->get_expr();
  }
  return cond->kind == ast_assign || cond->kind == ast_set_assign;
}

// make an error for `if (x = 1)`
static Error err_assignment_inside_condition() {
  return err("assignment inside condition, probably it's a misprint\n""hint: if it's intentional, extract assignment as a separate statement for clarity");
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

static bool expect_thrown_code(TypePtr t_ex_no) {
  return expect_integer(t_ex_no) || t_ex_no->unwrap_alias()->try_as<TypeDataEnum>();
}

static bool check_eq_neq_operator(TypePtr lhs_type, TypePtr rhs_type, bool& not_integer_comparison) {
  if (expect_integer(lhs_type) && expect_integer(rhs_type)) {
    return true;
  }
  if (expect_boolean(lhs_type) && expect_boolean(rhs_type)) {
    return true;
  }
  if (lhs_type->unwrap_alias()->try_as<TypeDataAddress>() && rhs_type->unwrap_alias()->try_as<TypeDataAddress>()) {
    not_integer_comparison = true;   // `address` can be compared with ==, but it's handled specially
    return true;
  }

  const TypeDataEnum* lhs_enum = lhs_type->unwrap_alias()->try_as<TypeDataEnum>();
  const TypeDataEnum* rhs_enum = rhs_type->unwrap_alias()->try_as<TypeDataEnum>();
  if (lhs_enum && rhs_enum) {   // allow `someColor == anotherColor`, don't allow `someColor == 123`
    return lhs_enum->enum_ref == rhs_enum->enum_ref;
  }
  
  return false;
}

class CheckInferredTypesVisitor final : public ASTVisitorFunctionBody {

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
      err_cannot_apply_operator(v->operator_name, lhs, rhs).fire(v->operator_range, cur_f);
    }
  }

  void visit(V<ast_unary_operator> v) override {
    AnyExprV rhs = v->get_rhs();
    parent::visit(rhs);

    switch (v->tok) {
      case tok_logical_not:
        if (!expect_integer(rhs) && !expect_boolean(rhs)) {
          err_cannot_apply_operator(v->operator_name, rhs).fire(v->operator_range, cur_f);
        }
        break;
      default:
        if (!expect_integer(rhs)) {
          err_cannot_apply_operator(v->operator_name, rhs).fire(v->operator_range, cur_f);
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
        bool not_integer_comparison = false;
        if (!check_eq_neq_operator(lhs->inferred_type, rhs->inferred_type, not_integer_comparison)) {
          if (lhs->inferred_type->equal_to(rhs->inferred_type)) {  // compare slice with slice, int? with int?
            err("type `{}` can not be compared with `== !=`", lhs->inferred_type).fire(v->operator_range, cur_f);
          } else {
            err_cannot_apply_operator(v->operator_name, lhs, rhs).fire(v->operator_range, cur_f);
          }
        } 
        if (not_integer_comparison) {    // special handling at IR generation like for `address`
          v->mutate()->assign_fun_ref(nullptr);
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
          err_cannot_apply_operator(v->operator_name, lhs, rhs).fire(v->operator_range, cur_f);
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
          err_cannot_apply_operator(v->operator_name, lhs, rhs).fire(v->operator_range, cur_f);
        }
        break;
      }
      // && || can work with integers and booleans, (int && bool) is allowed, (int16 && int32) also
      case tok_logical_and:
      case tok_logical_or: {
        bool lhs_ok = expect_integer(lhs) || expect_boolean(lhs);
        bool rhs_ok = expect_integer(rhs) || expect_boolean(rhs);
        if (!lhs_ok || !rhs_ok) {
          err_cannot_apply_operator(v->operator_name, lhs, rhs).fire(v->operator_range, cur_f);
        }
        break;
      }
      // others are mathematical: + * ...
      // they are allowed for intN (int16 + int32 is ok) and always "fall back" to general int
      default:
        if (!expect_integer(lhs) || !expect_integer(rhs)) {
          err_cannot_apply_operator(v->operator_name, lhs, rhs).fire(v->operator_range, cur_f);
        }
    }
  }

  void visit(V<ast_cast_as_operator> v) override {
    parent::visit(v->get_expr());

    if (!v->get_expr()->inferred_type->can_be_casted_with_as_operator(v->type_node->resolved_type)) {
      err("type `{}` can not be cast to `{}`", v->get_expr()->inferred_type, v->type_node->resolved_type).fire(v, cur_f);
    }
  }

  void visit(V<ast_is_type_operator> v) override {
    parent::visit(v->get_expr());
    TypePtr rhs_type = v->type_node->resolved_type;

    if (rhs_type->unwrap_alias()->try_as<TypeDataUnion>()) {   // `v is T1 | T2` / `v is T?` is disallowed
      err("union types are not allowed, use concrete types in `is`").fire(v, cur_f);
    }

    if ((v->is_always_true && !v->is_negated) || (v->is_always_false && v->is_negated)) {
      err("{} is always `{}`, this condition is always {}", expression_as_string(v->get_expr()), rhs_type, v->is_always_true).warning(v, cur_f);
    }
    if ((v->is_always_false && !v->is_negated) || (v->is_always_true && v->is_negated)) {
      err("{} of type `{}` can never be `{}`, this condition is always {}", expression_as_string(v->get_expr()), v->get_expr()->inferred_type, rhs_type, v->is_always_true).warning(v, cur_f);
    }
  }

  void visit(V<ast_not_null_operator> v) override {
    parent::visit(v->get_expr());

    if (v->get_expr()->inferred_type == TypeDataNullLiteral::create()) {
      // operator `!` used for always-null (proven by smart casts, for example), it's an error
      err("operator `!` used for always null expression").fire(v, cur_f);
    }
    // if operator `!` used for non-nullable, probably a warning should be printed
  }

  void visit(V<ast_bracket_tuple> v) override {
    parent::visit(v);

    for (int i = 0; i < v->size(); ++i) {
      AnyExprV item = v->get_item(i);
      if (item->inferred_type->get_width_on_stack() != 1) {
        err_cannot_put_non1_stack_width_arg_to_tuple(item->inferred_type).fire(v->get_item(i), cur_f);
      }
    }
  }

  void visit(V<ast_dot_access> v) override {
    parent::visit(v);

    if (v->is_target_indexed_access()) {
      TypePtr obj_type = v->get_obj()->inferred_type->unwrap_alias();
      if (v->inferred_type->get_width_on_stack() != 1 && (obj_type->try_as<TypeDataTuple>() || obj_type->try_as<TypeDataBrackets>())) {
        err_cannot_put_non1_stack_width_arg_to_tuple(v->inferred_type).fire(v, cur_f);
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
          err_type_mismatch("can not pass {src} to {dst}", arg_i->inferred_type, param_type).fire(arg_i, cur_f);
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

    if (fun_ref->is_builtin() && fun_ref->name[0] == '_') {
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
          err_type_mismatch("can not assign {src} to variable of type {dst}", rhs_type, declared_type).fire(err_loc, cur_f);
        }
      } else {
        if (rhs_type == TypeDataNullLiteral::create()) {
          err_assign_always_null_to_variable(lhs_var->var_ref->try_as<LocalVarPtr>(), corresponding_maybe_rhs && corresponding_maybe_rhs->kind == ast_null_keyword).fire(err_loc, cur_f);
        }
      }
      return;
    }

    // `(v1, v2) = rhs` / `var (v1, v2) = rhs` (rhs may be `(1,2)` or `tensorVar` or `someF()`, doesn't matter)
    // dig recursively into v1 and v2 with corresponding rhs i-th item of a tensor
    if (auto lhs_tensor = lhs->try_as<ast_tensor>()) {
      const TypeDataTensor* rhs_type_tensor = rhs_type->unwrap_alias()->try_as<TypeDataTensor>();
      if (!rhs_type_tensor) {
        err("can not assign `{}` to a tensor", rhs_type).fire(err_loc, cur_f);
      }
      if (lhs_tensor->size() != rhs_type_tensor->size()) {
        err("can not assign `{}`, sizes mismatch", rhs_type).fire(err_loc, cur_f);
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
        err("can not assign `{}` to a tuple", rhs_type).fire(err_loc, cur_f);
      }
      if (lhs_tuple->size() != rhs_type_tuple->size()) {
        err("can not assign `{}`, sizes mismatch", rhs_type).fire(err_loc, cur_f);
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
          err_cannot_put_non1_stack_width_arg_to_tuple(rhs_type).fire(err_loc, cur_f);
        }
      }
    }

    // here is `v = rhs` (just assignment, not `var v = rhs`) / `a.0 = rhs` / `getObj(z=f()).0 = rhs` etc.
    // types were already inferred, so just check their compatibility
    // for strange lhs like `f() = rhs` type checking will pass, but will fail lvalue check later
    if (!lhs->inferred_type->can_rhs_be_assigned(rhs_type)) {
      if (lhs->try_as<ast_reference>()) {
        err_type_mismatch("can not assign {src} to variable of type {dst}", rhs_type, lhs->inferred_type).fire(err_loc, cur_f);
      } else if (lhs->try_as<ast_dot_access>()) {
        err_type_mismatch("can not assign {src} to field of type {dst}", rhs_type, lhs->inferred_type).fire(err_loc, cur_f);
      } else {
        err_type_mismatch("can not assign {src} to {dst}", rhs_type, lhs->inferred_type).fire(err_loc, cur_f);
      }
    }
  }

  void visit(V<ast_return_statement> v) override {
    parent::visit(v->get_return_value());

    if (cur_f->does_return_self()) {
      if (!is_expr_valid_as_return_self(v->get_return_value())) {
        err("invalid return from `self` function").fire(v, cur_f);
      }
      return;
    }

    TypePtr expr_type = v->get_return_value()->inferred_type;
    if (!cur_f->inferred_return_type->can_rhs_be_assigned(expr_type)) {
      err_type_mismatch("can not convert type {src} to return type {dst}", expr_type, cur_f->inferred_return_type).fire(v->get_return_value(), cur_f);
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
      err("can not use `{}` as a boolean condition", cond->inferred_type).fire(cond, cur_f);
    }

    if (cond->is_always_true || cond->is_always_false) {
      warning_condition_always_true_or_false(cur_f, cond->range, cond, "ternary operator");
    }
    if (is_assignment_inside_condition(cond)) {
      err_assignment_inside_condition().warning(cond, cur_f);
    }
  }

  void visit(V<ast_match_expression> v) override {
    parent::visit(v);

    bool has_type_arm = false;
    bool has_expr_arm = false;
    bool has_else_arm = false;
    AnyExprV v_subject = v->get_subject();
    TypePtr subject_type = v_subject->inferred_type;
    const TypeDataEnum* subject_enum = subject_type->unwrap_alias()->try_as<TypeDataEnum>();
    const TypeDataUnion* subject_union = subject_type->unwrap_alias()->try_as<TypeDataUnion>();

    std::vector<TypePtr> covered_types;       // for type-based `match`, what types are on the left of `=>`
    std::vector<EnumMemberPtr> covered_enum;  // for `match` over an enum, we want it to be exhaustive

    for (int i = 0; i < v->get_arms_count(); ++i) {
      auto v_arm = v->get_arm(i);
      switch (v_arm->pattern_kind) {
        case MatchArmKind::exact_type: {
          if (has_expr_arm) {
            err("can not mix type and expression patterns in `match`").fire(v_arm->get_pattern_expr(), cur_f);
          }
          if (has_else_arm) {
            err("`else` branch should be the last").fire(v_arm->get_pattern_expr(), cur_f);
          }
          has_type_arm = true;

          TypePtr lhs_type = v_arm->pattern_type_node->resolved_type;   // `lhs_type => ...`
          if (lhs_type->unwrap_alias()->try_as<TypeDataUnion>()) {
            err("wrong pattern matching: union types are not allowed, use concrete types in `match`").fire(v_arm->get_pattern_expr(), cur_f);
          }
          bool can_happen = (subject_union && subject_union->has_variant_equal_to(lhs_type)) ||
                           (!subject_union && subject_type->equal_to(lhs_type));
          if (!can_happen) {
            err("wrong pattern matching: `{}` is not a variant of `{}`", lhs_type, subject_type).fire(v_arm->get_pattern_expr(), cur_f);
          }
          auto it_mentioned = std::find_if(covered_types.begin(), covered_types.end(), [lhs_type](TypePtr existing) {
            return existing->equal_to(lhs_type);
          });
          if (it_mentioned != covered_types.end()) {
            err("wrong pattern matching: duplicated `{}`", lhs_type).fire(v_arm->get_pattern_expr(), cur_f);
          }
          covered_types.push_back(lhs_type);
          break;
        }
        case MatchArmKind::const_expression: {
          if (has_type_arm) {
            err("can not mix type and expression patterns in `match`").fire(v_arm->get_pattern_expr(), cur_f);
          }
          if (has_else_arm) {
            err("`else` branch should be the last").fire(v_arm->get_pattern_expr(), cur_f);
          }
          has_expr_arm = true;
          TypePtr pattern_type = v_arm->get_pattern_expr()->inferred_type;
          bool not_integer_comparison = false;
          if (!check_eq_neq_operator(pattern_type, subject_type, not_integer_comparison)) {
            if (pattern_type->equal_to(subject_type)) {   // `match` over `slice` etc., where operator `==` can't be applied
              err("wrong pattern matching: can not compare type `{}` in `match`", subject_type).fire(v_arm->get_pattern_expr(), cur_f);
            } else {
              err("wrong pattern matching: can not compare type `{}` with match subject of type `{}`", v_arm->get_pattern_expr()->inferred_type, v_subject->inferred_type).fire(v_arm->get_pattern_expr(), cur_f);
            }
          }
          if (subject_enum) {
            auto l_dot = v_arm->get_pattern_expr()->try_as<ast_dot_access>();
            if (!l_dot || !l_dot->is_target_enum_member()) {    // match (someColor) { anotherColor => } 
              err("wrong pattern matching: `match` should contain members of a enum").fire(v_arm->get_pattern_expr(), cur_f);
            }
            EnumMemberPtr member_ref = std::get<EnumMemberPtr>(l_dot->target);
            if (std::find(covered_enum.begin(), covered_enum.end(), member_ref) != covered_enum.end()) {
              err("wrong pattern matching: duplicated enum member in `match`").fire(v_arm->get_pattern_expr(), cur_f);
            }
            covered_enum.push_back(member_ref);
          }
          break;
        }
        default:
          if (has_else_arm) {
            err("duplicated `else` branch").fire(v_arm->get_pattern_expr(), cur_f);
          }
          if (has_type_arm) {
            // `else` is not allowed in `match` by type, but we don't fire an error here,
            // because it might turn out to be a lazy `match`, where `else` is allowed;
            // if it's not lazy, an error is fired later
          }
          has_else_arm = true;
      }
    }

    // only `else` branch
    if (has_else_arm && !has_type_arm && !has_expr_arm) {
      err("`match` contains only `else`, but no variants").fire(v->keyword_range(), cur_f);
    }

    // fire if `match` by type is not exhaustive
    if (has_type_arm && subject_union && subject_union->variants.size() != covered_types.size()) {
      std::string missing;
      for (TypePtr variant : subject_union->variants) {
        auto it_mentioned = std::find_if(covered_types.begin(), covered_types.end(), [variant](TypePtr existing) {
          return existing->equal_to(variant);
        });
        if (it_mentioned == covered_types.end()) {
          if (!missing.empty()) {
            missing += ", ";
          }
          missing += "`" + variant->as_human_readable() + "`";
        }
      }
      err("`match` does not cover all possible types; missing types are: {}", missing).fire(v->keyword_range(), cur_f);
    }
    // fire if `match` by enum is not exhaustive
    if (has_expr_arm && subject_enum && !has_else_arm && subject_enum->enum_ref->members.size() != covered_enum.size()) {
      std::string missing;
      for (EnumMemberPtr member_ref : subject_enum->enum_ref->members) {
        if (std::find(covered_enum.begin(), covered_enum.end(), member_ref) == covered_enum.end()) {
          if (!missing.empty()) {
            missing += ", ";
          }
          missing += member_ref->name;
        }
      }
      err("`match` does not cover all possible enum members; missing members are: {}", missing).fire(v->keyword_range(), cur_f);
    }
    // fire if `match` by enum covers all cases, but contains `else`
    // (note that `else` for types could exist for a lazy match; for non-lazy, it's fired later)
    if (has_expr_arm && subject_enum && has_else_arm && subject_enum->enum_ref->members.size() == covered_enum.size()) {
      for (int i = 0; i < v->get_arms_count(); ++i) {
        if (auto v_arm = v->get_arm(i); v_arm->pattern_kind == MatchArmKind::else_branch) {
          err("`match` already covers all possible enum members, `else` is invalid").fire(v_arm->get_pattern_expr(), cur_f);
        }
      }
    }
    // `match` by expression, if it's not a statement, should have `else` or cover all values
    if (!v->is_statement() && !v->is_exhaustive) {
      err("`match` expression should have `else` branch").fire(v->keyword_range(), cur_f);
    }
  }

  void visit(V<ast_object_field> v) override {
    parent::visit(v->get_init_val());

    if (!v->field_ref->declared_type->can_rhs_be_assigned(v->get_init_val()->inferred_type)) {
      err_type_mismatch("can not assign {src} to field of type {dst}", v->get_init_val()->inferred_type, v->field_ref->declared_type).fire(v->get_init_val(), cur_f);
    }
  }

  void visit(V<ast_if_statement> v) override {
    parent::visit(v);

    AnyExprV cond = v->get_cond();
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      err("can not use `{}` as a boolean condition", cond->inferred_type).fire(cond, cur_f);
    }

    if (cond->is_always_true || cond->is_always_false) {
      warning_condition_always_true_or_false(cur_f, v->keyword_range(), cond, "`if`");
    }
    if (is_assignment_inside_condition(cond)) {
      err_assignment_inside_condition().warning(cond, cur_f);
    }
  }

  void visit(V<ast_repeat_statement> v) override {
    parent::visit(v);

    AnyExprV cond = v->get_cond();
    if (!expect_integer(cond)) {
      err("condition of `repeat` must be an integer, got `{}`", cond->inferred_type).fire(cond, cur_f);
    }
  }

  void visit(V<ast_while_statement> v) override {
    parent::visit(v);

    AnyExprV cond = v->get_cond();
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      err("can not use `{}` as a boolean condition", cond->inferred_type).fire(cond, cur_f);
    }

    if (cond->is_always_true || cond->is_always_false) {
      warning_condition_always_true_or_false(cur_f, v->keyword_range(), cond, "`while`");
    }
    if (is_assignment_inside_condition(cond)) {
      err_assignment_inside_condition().warning(cond, cur_f);
    }
  }

  void visit(V<ast_do_while_statement> v) override {
    parent::visit(v);

    AnyExprV cond = v->get_cond();
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      err("can not use `{}` as a boolean condition", cond->inferred_type).fire(cond, cur_f);
    }

    if (cond->is_always_true || cond->is_always_false) {
      warning_condition_always_true_or_false(cur_f, v->keyword_range(), cond, "`do while`");
    }
    if (is_assignment_inside_condition(cond)) {
      err_assignment_inside_condition().warning(cond, cur_f);
    }
  }

  void visit(V<ast_throw_statement> v) override {
    parent::visit(v);

    if (!expect_thrown_code(v->get_thrown_code()->inferred_type)) {
      err("excNo of `throw` must be an integer, got `{}`", v->get_thrown_code()->inferred_type).fire(v->get_thrown_code(), cur_f);
    }
    if (v->has_thrown_arg() && v->get_thrown_arg()->inferred_type->get_width_on_stack() != 1) {
      err("can not throw `{}`, exception arg must occupy exactly 1 stack slot", v->get_thrown_arg()->inferred_type).fire(v->get_thrown_arg(), cur_f);
    }
  }

  void visit(V<ast_assert_statement> v) override {
    parent::visit(v);

    AnyExprV cond = v->get_cond();
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      err("can not use `{}` as a boolean condition", cond->inferred_type).fire(cond, cur_f);
    }
    if (!expect_thrown_code(v->get_thrown_code()->inferred_type)) {
      err("thrown excNo of `assert` must be an integer, got `{}`", v->get_thrown_code()->inferred_type).fire(v->get_thrown_code(), cur_f);
    }

    if (cond->is_always_true || cond->is_always_false) {
      warning_condition_always_true_or_false(cur_f, v->keyword_range(), cond, "`assert`");
    }
    if (is_assignment_inside_condition(cond)) {
      err_assignment_inside_condition().warning(cond, cur_f);
    }
  }

  void visit(V<ast_block_statement> v) override {
    parent::visit(v);

    if (v->first_unreachable) {
      // it's essential to print "unreachable code" warning AFTER type checking
      // (printing it while inferring might be a false positive if types are incorrect, due to smart casts for example)
      // a more correct approach would be to access cfg here somehow, but since cfg is now available only while inferring,
      // a special v->first_unreachable was set specifically for this warning (again, which is correct if types match)
      err("unreachable code").warning(v->first_unreachable, cur_f);
    }
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }

  void on_exit_function(V<ast_function_declaration> v_function) override {
    if (cur_f->is_implicit_return() && cur_f->declared_return_type) {
      if (!cur_f->declared_return_type->can_rhs_be_assigned(TypeDataVoid::create()) || cur_f->does_return_self()) {
        err("missing return").fire(SrcRange::empty_at_end(v_function->get_body()->range), cur_f);
      }
    }

    // visit default values of parameters
    for (int i = 0; i < cur_f->get_num_params(); ++i) {
      if (LocalVarPtr param_ref = &cur_f->get_param(i); param_ref->has_default_value()) {
        parent::visit(param_ref->default_value);

        TypePtr inferred_type = param_ref->default_value->inferred_type;
        if (!param_ref->declared_type->can_rhs_be_assigned(inferred_type)) {
          err_type_mismatch("can not assign {src} to {dst}", inferred_type, param_ref->declared_type).fire(param_ref->default_value, cur_f);
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
        err_type_mismatch("can not assign {src} to {dst}", inferred_type, const_ref->declared_type).fire(const_ref->init_value);
      }
    }
  }

  // given struct field `a: int = 2 + 3` check types within its default_value
  void start_visiting_field_default(StructFieldPtr field_ref) {
    parent::visit(field_ref->default_value);

    TypePtr inferred_type = field_ref->default_value->inferred_type;
    if (!field_ref->declared_type->can_rhs_be_assigned(inferred_type)) {
      err_type_mismatch("can not assign {src} to {dst}", inferred_type, field_ref->declared_type).fire(field_ref->default_value);
    }
  }

  // given enum member `Red = 1` check types within its init_value and the whole expression itself
  void start_visiting_enum_member(EnumDefPtr enum_ref, EnumMemberPtr member_ref) {
    parent::visit(member_ref->init_value);

    TypePtr m_type = member_ref->init_value->inferred_type;
    bool is_integer = m_type->equal_to(TypeDataInt::create())
                   || m_type->equal_to(TypeDataCoins::create())
                   || m_type->try_as<TypeDataIntN>()
                   || m_type->try_as<TypeDataEnum>();
    if (!is_integer) {
      err("enum member is `{}`, not `int`\n""hint: all enums must be integers", m_type).fire(member_ref->init_value);
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
  for (EnumDefPtr enum_ref : get_all_declared_enums()) {
    for (EnumMemberPtr member_ref : enum_ref->members) {
      if (member_ref->has_init_value()) {
        visitor.start_visiting_enum_member(enum_ref, member_ref);
      }
    }
  }
}

} // namespace tolk
