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
#include "generics-helpers.h"
#include "type-system.h"

/*
 *   This is a complicated and crucial part of the pipeline. It simultaneously does the following:
 *   * infers types of all expressions; example: `2 + 3` both are TypeDataInt, result is also
 *   * AND checks types for assignment, arguments passing, etc.; example: `fInt(cs)` is error passing slice to int
 *   * AND binds function/method calls (assigns fun_ref); example: `globalF()`, fun_ref is assigned to `globalF` (unless generic)
 *   * AND instantiates generic functions; example: `t.tuplePush(2)` creates `tuplePush<int>` and assigns fun_ref to dot field
 *   * AND infers return type of functions if it's omitted (`fun f() { ... }` means "auto infer", not "void")
 *
 *   It's important to do all these parts simultaneously, they can't be split or separated.
 *   For example, we can't bind `f(2)` earlier, because if `f` is a generic `f<T>`, we should instantiate it,
 * and in order to do it, we need to know argument types.
 *   For example, we can't bind `c.cellHash()` earlier, because in the future we'll have overloads (`cell.hash()` and `slice.hash()`),
 * and in order to bind it, we need to know object type.
 *   And vice versa, to infer type of expression in the middle, we need to have inferred all expressions preceding it,
 * which may also include generics, etc.
 *
 *   About generics. They are more like "C++ templates". If `f<int>` and `f<slice>` called from somewhere,
 * there will be TWO new functions, inserted into symtable, and both will be code generated to Fift.
 *   Body of a generic function is NOT analyzed. Hence, `fun f<T>(v: T) { v.method(); }` we don't know
 * whether `v.method()` is a valid call until instantiate it with `f<slice>` for example.
 * Same for `v + 2`, we don't know whether + operator can be applied until instantiation.
 *   In other words, we have a closed type system, not open.
 *   That's why generic functions' bodies aren't traversed here (and in most following pipes).
 * Instead, when an instantiated function is created, it follows all the preceding pipeline (registering symbols, etc.),
 * and type inferring is done inside instantiated functions (which can recursively instantiate another, etc.).
 *
 *   A noticeable part of inferring is "hints".
 *   Example: `var a: User = { id: 3, name: "" }`. To infer type of `{...}` we need to know it's `User`. This hint is taken from lhs.
 *   Example: `fun tupleAt<T>(t: tuple, idx: int):T`, just `t.tupleGet(2)` can't be deduced (T left unspecified),
 *            but for assignment with left-defined type, or a call to `fInt(t.tupleGet(2))` hint "int" helps deduce T.
 *
 *   Unlike other pipes, inferring can dig recursively on demand.
 *   Example:
 *       fun getInt() { return 1; }
 *       fun main() { var i = getInt(); }
 *   If `main` is handled the first, it should know the return type if `getInt`. It's not declared, so we need
 * to launch type inferring for `getInt` and then proceed back to `main`.
 *   When a generic function is instantiated, type inferring inside it is also run.
 */

namespace tolk {

static void infer_and_save_return_type_of_function(const FunctionData* fun_ref);

static TypePtr get_or_infer_return_type(const FunctionData* fun_ref) {
  if (!fun_ref->inferred_return_type) {
    infer_and_save_return_type_of_function(fun_ref);
  }
  return fun_ref->inferred_return_type;
}

GNU_ATTRIBUTE_NOINLINE
static std::string to_string(TypePtr type) {
  return "`" + type->as_human_readable() + "`";
}

GNU_ATTRIBUTE_NOINLINE
static std::string to_string(AnyExprV v_with_type) {
  return "`" + v_with_type->inferred_type->as_human_readable() + "`";
}

GNU_ATTRIBUTE_NOINLINE
static std::string to_string(const LocalVarData& var_ref) {
  return "`" + var_ref.declared_type->as_human_readable() + "`";
}

GNU_ATTRIBUTE_NOINLINE
static std::string to_string(const FunctionData* fun_ref) {
  return "`" + fun_ref->as_human_readable() + "`";
}

// fire an error when `fun f<T>(...) asm ...` is called with T=(int,int) or other non-1 width on stack
// asm functions generally can't handle it, they expect T to be a TVM primitive
// (in FunC, `forall` type just couldn't be unified with non-primitives; in Tolk, generic T is expectedly inferred)
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_calling_asm_function_with_non1_stack_width_arg(SrcLocation loc, const FunctionData* fun_ref, const std::vector<TypePtr>& substitutions, int arg_idx) {
  throw ParseError(loc, "can not call `" + fun_ref->as_human_readable() + "` with " + fun_ref->genericTs->get_nameT(arg_idx) + "=" + substitutions[arg_idx]->as_human_readable() + ", because it occupies " + std::to_string(substitutions[arg_idx]->calc_width_on_stack()) + " stack slots in TVM, not 1");
}

// fire an error on `var n = null`
// technically it's correct, type of `n` is TypeDataNullLiteral, but it's not what the user wanted
// so, it's better to see an error on assignment, that later, on `n` usage and types mismatch
// (most common is situation above, but generally, `var (x,n) = xn` where xn is a tensor with 2-nd always-null, can be)
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_assign_always_null_to_variable(SrcLocation loc, const LocalVarData* assigned_var, bool is_assigned_null_literal) {
  std::string var_name = assigned_var->name;
  throw ParseError(loc, "can not infer type of `" + var_name + "`, it's always null; specify its type with `" + var_name + ": <type>`" + (is_assigned_null_literal ? " or use `null as <type>`" : ""));
}

// fire an error on `!cell` / `+slice`
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_cannot_apply_operator(SrcLocation loc, std::string_view operator_name, AnyExprV unary_expr) {
  std::string op = static_cast<std::string>(operator_name);
  throw ParseError(loc, "can not apply operator `" + op + "` to " + to_string(unary_expr->inferred_type));
}

// fire an error on `int + cell` / `slice & int`
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_cannot_apply_operator(SrcLocation loc, std::string_view operator_name, AnyExprV lhs, AnyExprV rhs) {
  std::string op = static_cast<std::string>(operator_name);
  throw ParseError(loc, "can not apply operator `" + op + "` to " + to_string(lhs->inferred_type) + " and " + to_string(rhs->inferred_type));
}

// check correctness of called arguments counts and their type matching
static void check_function_arguments(const FunctionData* fun_ref, V<ast_argument_list> v, AnyExprV lhs_of_dot_call) {
  int delta_self = lhs_of_dot_call ? 1 : 0;
  int n_arguments = v->size() + delta_self;
  int n_parameters = fun_ref->get_num_params();

  // Tolk doesn't have optional parameters currently, so just compare counts
  if (!n_parameters && lhs_of_dot_call) {
    v->error("`" + fun_ref->name + "` has no parameters and can not be called as method");
  }
  if (n_parameters < n_arguments) {
    v->error("too many arguments in call to `" + fun_ref->name + "`, expected " + std::to_string(n_parameters - delta_self) + ", have " + std::to_string(n_arguments - delta_self));
  }
  if (n_arguments < n_parameters) {
    v->error("too few arguments in call to `" + fun_ref->name + "`, expected " + std::to_string(n_parameters - delta_self) + ", have " + std::to_string(n_arguments - delta_self));
  }

  if (lhs_of_dot_call) {
    if (!fun_ref->parameters[0].declared_type->can_rhs_be_assigned(lhs_of_dot_call->inferred_type)) {
      lhs_of_dot_call->error("can not call method for " + to_string(fun_ref->parameters[0]) + " with object of type " + to_string(lhs_of_dot_call));
    }
  }
  for (int i = 0; i < v->size(); ++i) {
    if (!fun_ref->parameters[i + delta_self].declared_type->can_rhs_be_assigned(v->get_arg(i)->inferred_type)) {
      v->get_arg(i)->error("can not pass " + to_string(v->get_arg(i)) + " to " + to_string(fun_ref->parameters[i + delta_self]));
    }
  }
}

/*
 * TypeInferringUnifyStrategy unifies types from various branches to a common result (lca).
 * It's used to auto infer function return type based on return statements, like in TypeScript.
 * Example: `fun f() { ... return 1; ... return null; }` inferred as `int`.
 *
 * Besides function returns, it's also useful for ternary `return cond ? 1 : null` and `match` expression.
 * If types can't be unified (a function returns int and cell, for example), `unify()` returns false, handled outside.
 * BTW, don't confuse this way of inferring with Hindley-Milner, they have nothing in common.
 */
class TypeInferringUnifyStrategy {
  TypePtr unified_result = nullptr;

  static TypePtr calculate_type_lca(TypePtr t1, TypePtr t2) {
    if (t1 == t2) {
      return t1;
    }
    if (t1->can_rhs_be_assigned(t2)) {
      return t1;
    }
    if (t2->can_rhs_be_assigned(t1)) {
      return t2;
    }

    const auto* tensor1 = t1->try_as<TypeDataTensor>();
    const auto* tensor2 = t2->try_as<TypeDataTensor>();
    if (tensor1 && tensor2 && tensor1->size() == tensor2->size()) {
      std::vector<TypePtr> types_lca;
      types_lca.reserve(tensor1->size());
      for (int i = 0; i < tensor1->size(); ++i) {
        TypePtr next = calculate_type_lca(tensor1->items[i], tensor2->items[i]);
        if (next == nullptr) {
          return nullptr;
        }
        types_lca.push_back(next);
      }
      return TypeDataTensor::create(std::move(types_lca));
    }

    const auto* tuple1 = t1->try_as<TypeDataTypedTuple>();
    const auto* tuple2 = t2->try_as<TypeDataTypedTuple>();
    if (tuple1 && tuple2 && tuple1->size() == tuple2->size()) {
      std::vector<TypePtr> types_lca;
      types_lca.reserve(tuple1->size());
      for (int i = 0; i < tuple1->size(); ++i) {
        TypePtr next = calculate_type_lca(tuple1->items[i], tuple2->items[i]);
        if (next == nullptr) {
          return nullptr;
        }
        types_lca.push_back(next);
      }
      return TypeDataTypedTuple::create(std::move(types_lca));
    }

    return nullptr;
  }

public:
  bool unify_with(TypePtr next) {
    if (unified_result == nullptr) {
      unified_result = next;
      return true;
    }
    if (unified_result == next) {
      return true;
    }

    TypePtr combined = calculate_type_lca(unified_result, next);
    if (!combined) {
      return false;
    }

    unified_result = combined;
    return true;
  }

  bool unify_with_implicit_return_void() {
    if (unified_result == nullptr) {
      unified_result = TypeDataVoid::create();
      return true;
    }

    return unified_result == TypeDataVoid::create();
  }

  TypePtr get_result() const { return unified_result; }
};

/*
 * This class handles all types of AST vertices and traverses them, filling all AnyExprV::inferred_type.
 * Note, that it isn't derived from ASTVisitor, it has manual `switch` over all existing vertex types.
 * There are two reasons for this:
 * 1) when a new AST node type is introduced, I want it to fail here, not to be left un-inferred with UB at next steps
 * 2) easy to maintain a hint (see comments at the top of the file)
 */
class InferCheckTypesAndCallsAndFieldsVisitor final {
  const FunctionData* current_function = nullptr;
  TypeInferringUnifyStrategy return_unifier;

  GNU_ATTRIBUTE_ALWAYS_INLINE
  static void assign_inferred_type(AnyExprV dst, AnyExprV src) {
#ifdef TOLK_DEBUG
    tolk_assert(src->inferred_type != nullptr && !src->inferred_type->has_unresolved_inside() && !src->inferred_type->has_genericT_inside());
#endif
    dst->mutate()->assign_inferred_type(src->inferred_type);
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE
  static void assign_inferred_type(AnyExprV dst, TypePtr inferred_type) {
#ifdef TOLK_DEBUG
    tolk_assert(inferred_type != nullptr && !inferred_type->has_unresolved_inside() && !inferred_type->has_genericT_inside());
#endif
    dst->mutate()->assign_inferred_type(inferred_type);
  }

  static void assign_inferred_type(const LocalVarData* local_var_or_param, TypePtr inferred_type) {
#ifdef TOLK_DEBUG
    tolk_assert(inferred_type != nullptr && !inferred_type->has_unresolved_inside() && !inferred_type->has_genericT_inside());
#endif
    local_var_or_param->mutate()->assign_inferred_type(inferred_type);
  }

  static void assign_inferred_type(const FunctionData* fun_ref, TypePtr inferred_return_type, TypePtr inferred_full_type) {
#ifdef TOLK_DEBUG
    tolk_assert(inferred_return_type != nullptr && !inferred_return_type->has_unresolved_inside() && !inferred_return_type->has_genericT_inside());
#endif
    fun_ref->mutate()->assign_inferred_type(inferred_return_type, inferred_full_type);
  }

  // traverse children in any statement
  void process_any_statement(AnyV v) {
    switch (v->type) {
      case ast_sequence:
        return process_sequence(v->as<ast_sequence>());
      case ast_return_statement:
        return process_return_statement(v->as<ast_return_statement>());
      case ast_if_statement:
        return process_if_statement(v->as<ast_if_statement>());
      case ast_repeat_statement:
        return process_repeat_statement(v->as<ast_repeat_statement>());
      case ast_while_statement:
        return process_while_statement(v->as<ast_while_statement>());
      case ast_do_while_statement:
        return process_do_while_statement(v->as<ast_do_while_statement>());
      case ast_throw_statement:
        return process_throw_statement(v->as<ast_throw_statement>());
      case ast_assert_statement:
        return process_assert_statement(v->as<ast_assert_statement>());
      case ast_try_catch_statement:
        return process_try_catch_statement(v->as<ast_try_catch_statement>());
      case ast_empty_statement:
        return;
      default:
        infer_any_expr(reinterpret_cast<AnyExprV>(v));
    }
  }

  // assigns inferred_type for any expression (by calling assign_inferred_type)
  void infer_any_expr(AnyExprV v, TypePtr hint = nullptr) {
    switch (v->type) {
      case ast_int_const:
        return infer_int_const(v->as<ast_int_const>());
      case ast_string_const:
        return infer_string_const(v->as<ast_string_const>());
      case ast_bool_const:
        return infer_bool_const(v->as<ast_bool_const>());
      case ast_local_vars_declaration:
        return infer_local_vars_declaration(v->as<ast_local_vars_declaration>());
      case ast_assign:
        return infer_assignment(v->as<ast_assign>());
      case ast_set_assign:
        return infer_set_assign(v->as<ast_set_assign>());
      case ast_unary_operator:
        return infer_unary_operator(v->as<ast_unary_operator>());
      case ast_binary_operator:
        return infer_binary_operator(v->as<ast_binary_operator>());
      case ast_ternary_operator:
        return infer_ternary_operator(v->as<ast_ternary_operator>(), hint);
      case ast_cast_as_operator:
        return infer_cast_as_operator(v->as<ast_cast_as_operator>());
      case ast_parenthesized_expression:
        return infer_parenthesized(v->as<ast_parenthesized_expression>(), hint);
      case ast_reference:
        return infer_reference(v->as<ast_reference>());
      case ast_dot_access:
        return infer_dot_access(v->as<ast_dot_access>(), hint);
      case ast_function_call:
        return infer_function_call(v->as<ast_function_call>(), hint);
      case ast_tensor:
        return infer_tensor(v->as<ast_tensor>(), hint);
      case ast_typed_tuple:
        return infer_typed_tuple(v->as<ast_typed_tuple>(), hint);
      case ast_null_keyword:
        return infer_null_keyword(v->as<ast_null_keyword>());
      case ast_underscore:
        return infer_underscore(v->as<ast_underscore>(), hint);
      case ast_empty_expression:
        return infer_empty_expression(v->as<ast_empty_expression>());
      default:
        throw UnexpectedASTNodeType(v, "infer_any_expr");
    }
  }

  static bool expect_integer(AnyExprV v_inferred) {
    return v_inferred->inferred_type == TypeDataInt::create();
  }

  static bool expect_boolean(AnyExprV v_inferred) {
    return v_inferred->inferred_type == TypeDataBool::create();
  }

  static void infer_int_const(V<ast_int_const> v) {
    assign_inferred_type(v, TypeDataInt::create());
  }

  static void infer_string_const(V<ast_string_const> v) {
    if (v->is_bitslice()) {
      assign_inferred_type(v, TypeDataSlice::create());
    } else {
      assign_inferred_type(v, TypeDataInt::create());
    }
  }

  static void infer_bool_const(V<ast_bool_const> v) {
    assign_inferred_type(v, TypeDataBool::create());
  }

  static void infer_local_vars_declaration(V<ast_local_vars_declaration>) {
    // it can not appear as a standalone expression
    // `var ... = rhs` is handled by ast_assign
    tolk_assert(false);
  }

  void infer_assignment(V<ast_assign> v) {
    // v is assignment: `x = 5` / `var x = 5` / `var x: slice = 5` / `(cs,_) = f()` / `val (a,[b],_) = (a,t,0)`
    // it's a tricky node to handle, because to infer rhs, at first we need to create hint from lhs
    // and then to apply/check inferred rhs onto lhs
    // about a hint: `var i: int = t.tupleAt(0)` is ok, but `var i = t.tupleAt(0)` not, since `tupleAt<T>(t,i): T`
    AnyExprV lhs = v->get_lhs();
    AnyExprV rhs = v->get_rhs();
    infer_any_expr(rhs, calc_hint_from_assignment_lhs(lhs));
    process_assignment_lhs_after_infer_rhs(lhs, rhs->inferred_type, rhs);
    assign_inferred_type(v, lhs);
  }

  // having assignment like `var (i: int, s) = rhs` (its lhs is local vars declaration),
  // create a contextual infer hint for rhs, `(int, unknown)` in this case
  // this hint helps to deduce generics and to resolve unknown types while inferring rhs
  static TypePtr calc_hint_from_assignment_lhs(AnyExprV lhs) {
    // `var ... = rhs` - dig into left part
    if (auto lhs_decl = lhs->try_as<ast_local_vars_declaration>()) {
      return calc_hint_from_assignment_lhs(lhs_decl->get_expr());
    }

    // inside `var v: int = rhs` / `var _ = rhs` / `var v redef = rhs` (lhs is "v" / "_" / "v")
    if (auto lhs_var = lhs->try_as<ast_local_var_lhs>()) {
      if (lhs_var->marked_as_redef) {
        return lhs_var->var_ref->declared_type;
      }
      if (lhs_var->declared_type) {
        return lhs_var->declared_type;
      }
      return TypeDataUnknown::create();
    }

    // `v = rhs` / `(c1, c2) = rhs` (lhs is "v" / "_" / "c1" / "c2" after recursion)
    if (auto lhs_ref = lhs->try_as<ast_reference>()) {
      if (const auto* var_ref = lhs_ref->sym->try_as<LocalVarData>()) {
        return var_ref->declared_type;
      }
      if (const auto* glob_ref = lhs_ref->sym->try_as<GlobalVarData>()) {
        return glob_ref->declared_type;
      }
      return TypeDataUnknown::create();
    }

    // `(v1, v2) = rhs` / `var (v1, v2) = rhs`
    if (auto lhs_tensor = lhs->try_as<ast_tensor>()) {
      std::vector<TypePtr> sub_hints;
      sub_hints.reserve(lhs_tensor->size());
      for (AnyExprV item : lhs_tensor->get_items()) {
        sub_hints.push_back(calc_hint_from_assignment_lhs(item));
      }
      return TypeDataTensor::create(std::move(sub_hints));
    }

    // `[v1, v2] = rhs` / `var [v1, v2] = rhs`
    if (auto lhs_tuple = lhs->try_as<ast_typed_tuple>()) {
      std::vector<TypePtr> sub_hints;
      sub_hints.reserve(lhs_tuple->size());
      for (AnyExprV item : lhs_tuple->get_items()) {
        sub_hints.push_back(calc_hint_from_assignment_lhs(item));
      }
      return TypeDataTypedTuple::create(std::move(sub_hints));
    }

    return TypeDataUnknown::create();
  }

  // handle (and dig recursively) into `var lhs = rhs`
  // examples: `var z = 5`, `var (x, [y]) = (2, [3])`, `var (x, [y]) = xy`
  // while recursing, keep track of rhs if lhs and rhs have common shape (5 for z, 2 for x, [3] for [y], 3 for y)
  // (so that on type mismatch, point to corresponding rhs, example: `var (x, y:slice) = (1, 2)` point to 2
  void process_assignment_lhs_after_infer_rhs(AnyExprV lhs, TypePtr rhs_type, AnyExprV corresponding_maybe_rhs) {
    AnyExprV err_loc = corresponding_maybe_rhs ? corresponding_maybe_rhs : lhs;

    // `var ... = rhs` - dig into left part
    if (auto lhs_decl = lhs->try_as<ast_local_vars_declaration>()) {
      process_assignment_lhs_after_infer_rhs(lhs_decl->get_expr(), rhs_type, corresponding_maybe_rhs);
      assign_inferred_type(lhs, lhs_decl->get_expr()->inferred_type);
      return;
    }

    // inside `var v: int = rhs` / `var _ = rhs` / `var v redef = rhs` (lhs is "v" / "_" / "v")
    if (auto lhs_var = lhs->try_as<ast_local_var_lhs>()) {
      TypePtr declared_type = lhs_var->declared_type;  // `var v: int = rhs` (otherwise, nullptr)
      if (lhs_var->marked_as_redef) {
        tolk_assert(lhs_var->var_ref && lhs_var->var_ref->declared_type);
        declared_type = lhs_var->var_ref->declared_type;
      }
      if (declared_type) {
        if (!declared_type->can_rhs_be_assigned(rhs_type)) {
          err_loc->error("can not assign " + to_string(rhs_type) + " to variable of type " + to_string(declared_type));
        }
        assign_inferred_type(lhs, declared_type);
      } else {
        if (rhs_type == TypeDataNullLiteral::create()) {
          fire_error_assign_always_null_to_variable(err_loc->loc, lhs_var->var_ref->try_as<LocalVarData>(), corresponding_maybe_rhs && corresponding_maybe_rhs->type == ast_null_keyword);
        }
        assign_inferred_type(lhs, rhs_type);
        assign_inferred_type(lhs_var->var_ref, lhs_var->inferred_type);
      }
      return;
    }

    // `v = rhs` / `(c1, c2) = rhs` (lhs is "v" / "_" / "c1" / "c2" after recursion)
    if (lhs->try_as<ast_reference>()) {
      infer_any_expr(lhs);
      if (!lhs->inferred_type->can_rhs_be_assigned(rhs_type)) {
        err_loc->error("can not assign " + to_string(rhs_type) + " to variable of type " + to_string(lhs));
      }
      return;
    }

    // `(v1, v2) = rhs` / `var (v1, v2) = rhs` (rhs may be `(1,2)` or `tensorVar` or `someF()`, doesn't matter)
    // dig recursively into v1 and v2 with corresponding rhs i-th item of a tensor
    if (auto lhs_tensor = lhs->try_as<ast_tensor>()) {
      const TypeDataTensor* rhs_type_tensor = rhs_type->try_as<TypeDataTensor>();
      if (!rhs_type_tensor) {
        err_loc->error("can not assign " + to_string(rhs_type) + " to a tensor");
      }
      if (lhs_tensor->size() != rhs_type_tensor->size()) {
        err_loc->error("can not assign " + to_string(rhs_type) + ", sizes mismatch");
      }
      V<ast_tensor> rhs_tensor_maybe = corresponding_maybe_rhs ? corresponding_maybe_rhs->try_as<ast_tensor>() : nullptr;
      std::vector<TypePtr> types_list;
      types_list.reserve(lhs_tensor->size());
      for (int i = 0; i < lhs_tensor->size(); ++i) {
        process_assignment_lhs_after_infer_rhs(lhs_tensor->get_item(i), rhs_type_tensor->items[i], rhs_tensor_maybe ? rhs_tensor_maybe->get_item(i) : nullptr);
        types_list.push_back(lhs_tensor->get_item(i)->inferred_type);
      }
      assign_inferred_type(lhs, TypeDataTensor::create(std::move(types_list)));
      return;
    }

    // `[v1, v2] = rhs` / `var [v1, v2] = rhs` (rhs may be `[1,2]` or `tupleVar` or `someF()`, doesn't matter)
    // dig recursively into v1 and v2 with corresponding rhs i-th item of a tuple
    if (auto lhs_tuple = lhs->try_as<ast_typed_tuple>()) {
      const TypeDataTypedTuple* rhs_type_tuple = rhs_type->try_as<TypeDataTypedTuple>();
      if (!rhs_type_tuple) {
        err_loc->error("can not assign " + to_string(rhs_type) + " to a tuple");
      }
      if (lhs_tuple->size() != rhs_type_tuple->size()) {
        err_loc->error("can not assign " + to_string(rhs_type) + ", sizes mismatch");
      }
      V<ast_typed_tuple> rhs_tuple_maybe = corresponding_maybe_rhs ? corresponding_maybe_rhs->try_as<ast_typed_tuple>() : nullptr;
      std::vector<TypePtr> types_list;
      types_list.reserve(lhs_tuple->size());
      for (int i = 0; i < lhs_tuple->size(); ++i) {
        process_assignment_lhs_after_infer_rhs(lhs_tuple->get_item(i), rhs_type_tuple->items[i], rhs_tuple_maybe ? rhs_tuple_maybe->get_item(i) : nullptr);
        types_list.push_back(lhs_tuple->get_item(i)->inferred_type);
      }
      assign_inferred_type(lhs, TypeDataTypedTuple::create(std::move(types_list)));
      return;
    }

    // `_ = rhs`
    if (lhs->type == ast_underscore) {
      assign_inferred_type(lhs, TypeDataUnknown::create());
      return;
    }

    // here is something strange and unhandled, like `f() = rhs`
    // it will fail on later compilation steps (like rvalue/lvalue checks), but type inferring should pass
    infer_any_expr(lhs, rhs_type);
    if (!lhs->inferred_type->can_rhs_be_assigned(rhs_type)) {
      err_loc->error("can not assign " + to_string(rhs_type) + " to " + to_string(lhs));
    }
  }

  void infer_set_assign(V<ast_set_assign> v) {
    AnyExprV lhs = v->get_lhs();
    AnyExprV rhs = v->get_rhs();
    infer_any_expr(lhs);
    infer_any_expr(rhs, lhs->inferred_type);

    // almost all operators implementation is hardcoded by built-in functions `_+_` and similar
    std::string_view builtin_func = v->operator_name;   // "+" for operator +=

    switch (v->tok) {
      // &= |= ^= are "overloaded" both for integers and booleans, (int &= bool) is NOT allowed
      case tok_set_bitwise_and:
      case tok_set_bitwise_or:
      case tok_set_bitwise_xor: {
        bool both_int = expect_integer(lhs) && expect_integer(rhs);
        bool both_bool = expect_boolean(lhs) && expect_boolean(rhs);
        if (!both_int && !both_bool) {
          fire_error_cannot_apply_operator(v->loc, v->operator_name, lhs, rhs);
        }
        break;
      }
      // others are mathematical: += *= ...
      default:
        if (!expect_integer(lhs) || !expect_integer(rhs)) {
          fire_error_cannot_apply_operator(v->loc, v->operator_name, lhs, rhs);
        }
    }

    assign_inferred_type(v, lhs);
    if (!builtin_func.empty()) {
      const FunctionData* builtin_sym = lookup_global_symbol("_" + static_cast<std::string>(builtin_func) + "_")->as<FunctionData>();
      tolk_assert(builtin_sym);
      v->mutate()->assign_fun_ref(builtin_sym);
    }
  }

  void infer_unary_operator(V<ast_unary_operator> v) {
    AnyExprV rhs = v->get_rhs();
    infer_any_expr(rhs);

    // all operators implementation is hardcoded by built-in functions `~_` and similar
    std::string_view builtin_func = v->operator_name;

    switch (v->tok) {
      case tok_minus:
      case tok_plus:
      case tok_bitwise_not:
        if (!expect_integer(rhs)) {
          fire_error_cannot_apply_operator(v->loc, v->operator_name, rhs);
        }
        assign_inferred_type(v, TypeDataInt::create());
        break;
      case tok_logical_not:
        if (expect_boolean(rhs)) {
          builtin_func = "!b";  // "overloaded" for bool
        } else if (!expect_integer(rhs)) {
          fire_error_cannot_apply_operator(v->loc, v->operator_name, rhs);
        }
        assign_inferred_type(v, TypeDataBool::create());
        break;
      default:
        tolk_assert(false);
    }

    if (!builtin_func.empty()) {
      const FunctionData* builtin_sym = lookup_global_symbol(static_cast<std::string>(builtin_func) + "_")->as<FunctionData>();
      tolk_assert(builtin_sym);
      v->mutate()->assign_fun_ref(builtin_sym);
    }
  }

  void infer_binary_operator(V<ast_binary_operator> v) {
    AnyExprV lhs = v->get_lhs();
    AnyExprV rhs = v->get_rhs();
    infer_any_expr(lhs);
    infer_any_expr(rhs);

    // almost all operators implementation is hardcoded by built-in functions `_+_` and similar
    std::string_view builtin_func = v->operator_name;

    switch (v->tok) {
      // == != can compare both integers and booleans, (int == bool) is NOT allowed
      case tok_eq:
      case tok_neq: {
        bool both_int = expect_integer(lhs) && expect_integer(rhs);
        bool both_bool = expect_boolean(lhs) && expect_boolean(rhs);
        if (!both_int && !both_bool) {
          if (lhs->inferred_type == rhs->inferred_type) {   // compare slice with slice
            v->error("type " + to_string(lhs) + " can not be compared with `== !=`");
          } else {
            fire_error_cannot_apply_operator(v->loc, v->operator_name, lhs, rhs);
          }
        }
        assign_inferred_type(v, TypeDataBool::create());
        break;
      }
      // < > can compare only integers
      case tok_lt:
      case tok_gt:
      case tok_leq:
      case tok_geq:
      case tok_spaceship: {
        if (!expect_integer(lhs) || !expect_integer(rhs)) {
          fire_error_cannot_apply_operator(v->loc, v->operator_name, lhs, rhs);
        }
        assign_inferred_type(v, TypeDataBool::create());
        break;
      }
      // & | ^ are "overloaded" both for integers and booleans, (int & bool) is NOT allowed
      case tok_bitwise_and:
      case tok_bitwise_or:
      case tok_bitwise_xor: {
        bool both_int = expect_integer(lhs) && expect_integer(rhs);
        bool both_bool = expect_boolean(lhs) && expect_boolean(rhs);
        if (!both_int && !both_bool) {
          fire_error_cannot_apply_operator(v->loc, v->operator_name, lhs, rhs);
        }
        assign_inferred_type(v, rhs);   // (int & int) is int, (bool & bool) is bool
        break;
      }
      // && || can work with integers and booleans, (int && bool) is allowed
      case tok_logical_and:
      case tok_logical_or: {
        bool lhs_ok = expect_integer(lhs) || expect_boolean(lhs);
        bool rhs_ok = expect_integer(rhs) || expect_boolean(rhs);
        if (!lhs_ok || !rhs_ok) {
          fire_error_cannot_apply_operator(v->loc, v->operator_name, lhs, rhs);
        }
        assign_inferred_type(v, TypeDataBool::create());
        builtin_func = {};    // no built-in functions, logical operators are expressed as IFs at IR level
        break;
      }
      // others are mathematical: + * ...
      default:
        if (!expect_integer(lhs) || !expect_integer(rhs)) {
          fire_error_cannot_apply_operator(v->loc, v->operator_name, lhs, rhs);
        }
        assign_inferred_type(v, TypeDataInt::create());
    }

    if (!builtin_func.empty()) {
      const FunctionData* builtin_sym = lookup_global_symbol("_" + static_cast<std::string>(builtin_func) + "_")->as<FunctionData>();
      tolk_assert(builtin_sym);
      v->mutate()->assign_fun_ref(builtin_sym);
    }
  }

  void infer_ternary_operator(V<ast_ternary_operator> v, TypePtr hint) {
    AnyExprV cond = v->get_cond();
    infer_any_expr(cond);
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      cond->error("can not use " + to_string(cond) + " as a boolean condition");
    }
    infer_any_expr(v->get_when_true(), hint);
    infer_any_expr(v->get_when_false(), hint);

    TypeInferringUnifyStrategy tern_type;
    tern_type.unify_with(v->get_when_true()->inferred_type);
    if (!tern_type.unify_with(v->get_when_false()->inferred_type)) {
      v->error("types of ternary branches are incompatible");
    }
    assign_inferred_type(v, tern_type.get_result());
  }

  void infer_cast_as_operator(V<ast_cast_as_operator> v) {
    // for `expr as <type>`, use this type for hint, so that `t.tupleAt(0) as int` is ok
    infer_any_expr(v->get_expr(), v->cast_to_type);
    if (!v->get_expr()->inferred_type->can_be_casted_with_as_operator(v->cast_to_type)) {
      v->error("type " + to_string(v->get_expr()) + " can not be cast to " + to_string(v->cast_to_type));
    }
    assign_inferred_type(v, v->cast_to_type);
  }

  void infer_parenthesized(V<ast_parenthesized_expression> v, TypePtr hint) {
    infer_any_expr(v->get_expr(), hint);
    assign_inferred_type(v, v->get_expr());
  }

  static void infer_reference(V<ast_reference> v) {
    if (const auto* var_ref = v->sym->try_as<LocalVarData>()) {
      assign_inferred_type(v, var_ref->declared_type);

    } else if (const auto* const_ref = v->sym->try_as<GlobalConstData>()) {
      assign_inferred_type(v, const_ref->is_int_const() ? TypeDataInt::create() : TypeDataSlice::create());

    } else if (const auto* glob_ref = v->sym->try_as<GlobalVarData>()) {
      assign_inferred_type(v, glob_ref->declared_type);

    } else if (const auto* fun_ref = v->sym->try_as<FunctionData>()) {
      // it's `globalF` / `globalF<int>` - references to functions used as non-call
      V<ast_instantiationT_list> v_instantiationTs = v->get_instantiationTs();

      if (fun_ref->is_generic_function() && !v_instantiationTs) {
        // `genericFn` is invalid as non-call, can't be used without <instantiation>
        v->error("can not use a generic function " + to_string(fun_ref) + " as non-call");

      } else if (fun_ref->is_generic_function()) {
        // `genericFn<int>` is valid, it's a reference to instantiation
        std::vector<TypePtr> substitutions = collect_fun_generic_substitutions_from_manually_specified(v->loc, fun_ref, v_instantiationTs);
        fun_ref = check_and_instantiate_generic_function(v->loc, fun_ref, std::move(substitutions));
        v->mutate()->assign_sym(fun_ref);

      } else if (UNLIKELY(v_instantiationTs != nullptr)) {
        // non-generic function referenced like `return beginCell<builder>;`
        v_instantiationTs->error("not generic function used with generic T");
      }

      fun_ref->mutate()->assign_is_used_as_noncall();
      get_or_infer_return_type(fun_ref);
      assign_inferred_type(v, fun_ref->inferred_full_type);
      return;

    } else {
      tolk_assert(false);
    }

    // for non-functions: `local_var<int>` and similar not allowed
    if (UNLIKELY(v->has_instantiationTs())) {
      v->get_instantiationTs()->error("generic T not expected here");
    }
  }

  // given `genericF<int, slice>` / `t.tupleFirst<cell>` (the user manually specified instantiation Ts),
  // validate and collect them
  // returns: [int, slice] / [cell]
  static std::vector<TypePtr> collect_fun_generic_substitutions_from_manually_specified(SrcLocation loc, const FunctionData* fun_ref, V<ast_instantiationT_list> instantiationT_list) {
    if (fun_ref->genericTs->size() != instantiationT_list->get_items().size()) {
      throw ParseError(loc, "wrong count of generic T: expected " + std::to_string(fun_ref->genericTs->size()) + ", got " + std::to_string(instantiationT_list->size()));
    }

    std::vector<TypePtr> substitutions;
    substitutions.reserve(instantiationT_list->size());
    for (int i = 0; i < instantiationT_list->size(); ++i) {
      substitutions.push_back(instantiationT_list->get_item(i)->substituted_type);
    }

    return substitutions;
  }

  // when generic Ts have been collected from user-specified or deduced from arguments,
  // instantiate a generic function
  // example: was `t.tuplePush(2)`, deduced <int>, instantiate `tuplePush<int>`
  // example: was `t.tuplePush<slice>(2)`, read <slice>, instantiate `tuplePush<slice>` (will later fail type check)
  // example: was `var cb = t.tupleFirst<int>;` (used as reference, as non-call), instantiate `tupleFirst<int>`
  // returns fun_ref to instantiated function
  static const FunctionData* check_and_instantiate_generic_function(SrcLocation loc, const FunctionData* fun_ref, std::vector<TypePtr>&& substitutionTs) {
    // T for asm function must be a TVM primitive (width 1), otherwise, asm would act incorrectly
    if (fun_ref->is_asm_function() || fun_ref->is_builtin_function()) {
      for (int i = 0; i < static_cast<int>(substitutionTs.size()); ++i) {
        if (substitutionTs[i]->calc_width_on_stack() != 1) {
          fire_error_calling_asm_function_with_non1_stack_width_arg(loc, fun_ref, substitutionTs, i);
        }
      }
    }

    std::string inst_name = generate_instantiated_name(fun_ref->name, substitutionTs);
    try {
      // make deep clone of `f<T>` with substitutionTs
      // (if `f<int>` was already instantiated, it will be immediately returned from a symbol table)
      return instantiate_generic_function(loc, fun_ref, inst_name, std::move(substitutionTs));
    } catch (const ParseError& ex) {
      throw ParseError(ex.where, "while instantiating generic function `" + inst_name + "` at " + loc.to_string() + ": " + ex.message);
    }
  }

  void infer_dot_access(V<ast_dot_access> v, TypePtr hint) {
    // it's NOT a method call `t.tupleSize()` (since such cases are handled by infer_function_call)
    // it's `t.0`, `getUser().id`, and `t.tupleSize` (as a reference, not as a call)
    infer_any_expr(v->get_obj());
    // our goal is to fill v->target knowing type of obj
    V<ast_identifier> v_ident = v->get_identifier();    // field/method name vertex
    V<ast_instantiationT_list> v_instantiationTs = v->get_instantiationTs();
    std::string_view field_name = v_ident->name;

    // for now, Tolk doesn't have structures, properties, and object-scoped methods
    // so, only `t.tupleSize` is allowed, look up a global function
    const Symbol* sym = lookup_global_symbol(field_name);
    if (!sym) {
      v_ident->error("undefined symbol `" + static_cast<std::string>(field_name) + "`");
    }
    const FunctionData* fun_ref = sym->try_as<FunctionData>();
    if (!fun_ref) {
      v_ident->error("referencing a non-function");
    }

    // `t.tupleSize` is ok, `cs.tupleSize` not
    if (!fun_ref->parameters[0].declared_type->can_rhs_be_assigned(v->get_obj()->inferred_type)) {
      v_ident->error("referencing a method for " + to_string(fun_ref->parameters[0]) + " with an object of type " + to_string(v->get_obj()));
    }

    if (fun_ref->is_generic_function() && !v_instantiationTs) {
      // `genericFn` and `t.tupleAt` are invalid as non-call, they can't be used without <instantiation>
      v->error("can not use a generic function " + to_string(fun_ref) + " as non-call");

    } else if (fun_ref->is_generic_function()) {
      // `t.tupleAt<slice>` is valid, it's a reference to instantiation
      std::vector<TypePtr> substitutions = collect_fun_generic_substitutions_from_manually_specified(v->loc, fun_ref, v_instantiationTs);
      fun_ref = check_and_instantiate_generic_function(v->loc, fun_ref, std::move(substitutions));

    } else if (UNLIKELY(v_instantiationTs != nullptr)) {
      // non-generic method referenced like `var cb = c.cellHash<int>;`
      v_instantiationTs->error("not generic function used with generic T");
    }

    fun_ref->mutate()->assign_is_used_as_noncall();
    v->mutate()->assign_target(fun_ref);
    get_or_infer_return_type(fun_ref);
    assign_inferred_type(v, fun_ref->inferred_full_type);   // type of `t.tupleSize` is TypeDataFunCallable
  }

  void infer_function_call(V<ast_function_call> v, TypePtr hint) {
    AnyExprV callee = v->get_callee();

    // v is `globalF(args)` / `globalF<int>(args)` / `obj.method(args)` / `local_var(args)` / `getF()(args)`
    int delta_self = 0;
    AnyExprV dot_obj = nullptr;
    const FunctionData* fun_ref = nullptr;
    V<ast_instantiationT_list> v_instantiationTs = nullptr;

    if (auto v_ref = callee->try_as<ast_reference>()) {
      // `globalF()` / `globalF<int>()` / `local_var()` / `SOME_CONST()`
      fun_ref = v_ref->sym->try_as<FunctionData>();         // not null for `globalF`
      v_instantiationTs = v_ref->get_instantiationTs();     // present for `globalF<int>()`

    } else if (auto v_dot = callee->try_as<ast_dot_access>()) {
      // `obj.someMethod()` / `obj.someMethod<int>()` / `getF().someMethod()` / `obj.SOME_CONST()`
      delta_self = 1;
      dot_obj = v_dot->get_obj();
      v_instantiationTs = v_dot->get_instantiationTs();     // present for `obj.someMethod<int>()`
      infer_any_expr(dot_obj);

      // for now, Tolk doesn't have object-scoped methods, so method resolving doesn't depend on obj type
      // (in other words, `globalFunction(a)` = `a.globalFunction()`)
      std::string_view method_name = v_dot->get_field_name();
      const Symbol* sym = lookup_global_symbol(method_name);
      if (!sym) {
        v_dot->get_identifier()->error("undefined symbol `" + static_cast<std::string>(method_name) + "`");
      }
      fun_ref = sym->try_as<FunctionData>();
      if (!fun_ref) {
        v_dot->get_identifier()->error("calling a non-function");
      }

    } else {
      // `getF()()` / `5()`
      // fun_ref remains nullptr
    }

    // infer argument types, looking at fun_ref's parameters as hints
    for (int i = 0; i < v->get_num_args(); ++i) {
      TypePtr param_type = fun_ref && i < fun_ref->get_num_params() - delta_self ? fun_ref->parameters[delta_self + i].declared_type : nullptr;
      auto arg_i = v->get_arg(i);
      infer_any_expr(arg_i->get_expr(), param_type && !param_type->has_genericT_inside() ? param_type : nullptr);
      assign_inferred_type(arg_i, arg_i->get_expr());
    }

    // handle `local_var()` / `getF()()` / `5()` / `SOME_CONST()` / `obj.method()()()`
    if (!fun_ref) {
      // treat callee like a usual expression, which must have "callable" inferred type
      infer_any_expr(callee);
      const TypeDataFunCallable* f_callable = callee->inferred_type->try_as<TypeDataFunCallable>();
      if (!f_callable) {   // `5()` / `SOME_CONST()` / `null()`
        v->error("calling a non-function");
      }
      // check arguments count and their types
      if (v->get_num_args() != static_cast<int>(f_callable->params_types.size())) {
        v->error("expected " + std::to_string(f_callable->params_types.size()) + " arguments, got " + std::to_string(v->get_arg_list()->size()));
      }
      for (int i = 0; i < v->get_num_args(); ++i) {
        if (!f_callable->params_types[i]->can_rhs_be_assigned(v->get_arg(i)->inferred_type)) {
          v->get_arg(i)->error("can not pass " + to_string(v->get_arg(i)) + " to " + to_string(f_callable->params_types[i]));
        }
      }
      v->mutate()->assign_fun_ref(nullptr);   // no fun_ref to a global function
      assign_inferred_type(v, f_callable->return_type);
      return;
    }

    // so, we have a call `f(args)` or `obj.f(args)`, f is a global function (fun_ref) (code / asm / builtin)
    // if it's a generic function `f<T>`, we need to instantiate it, like `f<int>`
    // same for generic methods `t.tupleAt<T>`, need to achieve `t.tupleAt<int>`

    if (fun_ref->is_generic_function() && v_instantiationTs) {
      // if Ts are specified by a user like `f<int>(args)` / `t.tupleAt<slice>()`, take them
      std::vector<TypePtr> substitutions = collect_fun_generic_substitutions_from_manually_specified(v->loc, fun_ref, v_instantiationTs);
      fun_ref = check_and_instantiate_generic_function(v->loc, fun_ref, std::move(substitutions));

    } else if (fun_ref->is_generic_function()) {
      // if `f<T>` called like `f(args)`, deduce T from arg types
      std::vector<TypePtr> arg_types;
      arg_types.reserve(delta_self + v->get_num_args());
      if (dot_obj) {
        arg_types.push_back(dot_obj->inferred_type);
      }
      for (int i = 0; i < v->get_num_args(); ++i) {
        arg_types.push_back(v->get_arg(i)->inferred_type);
      }

      td::Result<std::vector<TypePtr>> deduced = deduce_substitutionTs_on_generic_func_call(fun_ref, std::move(arg_types), hint);
      if (deduced.is_error()) {
        v->error(deduced.error().message().str() + " for generic function " + to_string(fun_ref));
      }
      fun_ref = check_and_instantiate_generic_function(v->loc, fun_ref, deduced.move_as_ok());

    } else if (UNLIKELY(v_instantiationTs != nullptr)) {
      // non-generic function/method called with type arguments, like `c.cellHash<int>()` / `beginCell<builder>()`
      v_instantiationTs->error("calling a not generic function with generic T");
    }

    v->mutate()->assign_fun_ref(fun_ref);
    // since for `t.tupleAt()`, infer_dot_access() not called for callee = "t.tupleAt", assign its target here
    if (v->is_dot_call()) {
      v->get_callee()->as<ast_dot_access>()->mutate()->assign_target(fun_ref);
      v->get_callee()->as<ast_dot_access>()->mutate()->assign_inferred_type(fun_ref->inferred_full_type);
    }
    // check arguments count and their types
    check_function_arguments(fun_ref, v->get_arg_list(), dot_obj);
    // get return type either from user-specified declaration or infer here on demand traversing its body
    get_or_infer_return_type(fun_ref);
    TypePtr inferred_type = dot_obj && fun_ref->does_return_self() ? dot_obj->inferred_type : fun_ref->inferred_return_type;
    assign_inferred_type(v, inferred_type);
    assign_inferred_type(callee, fun_ref->inferred_full_type);
    // note, that mutate params don't affect typing, they are handled when converting to IR
  }

  void infer_tensor(V<ast_tensor> v, TypePtr hint) {
    const TypeDataTensor* tensor_hint = hint ? hint->try_as<TypeDataTensor>() : nullptr;
    std::vector<TypePtr> types_list;
    types_list.reserve(v->get_items().size());
    for (int i = 0; i < v->size(); ++i) {
      AnyExprV item = v->get_item(i);
      infer_any_expr(item, tensor_hint && i < tensor_hint->size() ? tensor_hint->items[i] : nullptr);
      types_list.emplace_back(item->inferred_type);
    }
    assign_inferred_type(v, TypeDataTensor::create(std::move(types_list)));
  }

  void infer_typed_tuple(V<ast_typed_tuple> v, TypePtr hint) {
    const TypeDataTypedTuple* tuple_hint = hint ? hint->try_as<TypeDataTypedTuple>() : nullptr;
    std::vector<TypePtr> types_list;
    types_list.reserve(v->get_items().size());
    for (int i = 0; i < v->size(); ++i) {
      AnyExprV item = v->get_item(i);
      infer_any_expr(item, tuple_hint && i < tuple_hint->size() ? tuple_hint->items[i] : nullptr);
      types_list.emplace_back(item->inferred_type);
    }
    assign_inferred_type(v, TypeDataTypedTuple::create(std::move(types_list)));
  }

  static void infer_null_keyword(V<ast_null_keyword> v) {
    assign_inferred_type(v, TypeDataNullLiteral::create());
  }

  static void infer_underscore(V<ast_underscore> v, TypePtr hint) {
    // if execution is here, underscore is either used as lhs of assignment, or incorrectly, like `f(_)`
    // more precise is to always set unknown here, but for incorrect usages, instead of an error
    // "can not pass unknown to X" would better be an error it can't be used as a value, at later steps
    assign_inferred_type(v, hint ? hint : TypeDataUnknown::create());
  }

  static void infer_empty_expression(V<ast_empty_expression> v) {
    assign_inferred_type(v, TypeDataUnknown::create());
  }

  void process_sequence(V<ast_sequence> v) {
    for (AnyV item : v->get_items()) {
      process_any_statement(item);
    }
  }

  static bool is_expr_valid_as_return_self(AnyExprV return_expr) {
    // `return self`
    if (return_expr->type == ast_reference && return_expr->as<ast_reference>()->get_name() == "self") {
      return true;
    }
    // `return self.someMethod()`
    if (auto v_call = return_expr->try_as<ast_function_call>(); v_call && v_call->is_dot_call()) {
      return v_call->fun_maybe && v_call->fun_maybe->does_return_self() && is_expr_valid_as_return_self(v_call->get_dot_obj());
    }
    // `return cond ? ... : ...`
    if (auto v_ternary = return_expr->try_as<ast_ternary_operator>()) {
      return is_expr_valid_as_return_self(v_ternary->get_when_true()) && is_expr_valid_as_return_self(v_ternary->get_when_false());
    }
    return false;
  }

  void process_return_statement(V<ast_return_statement> v) {
    if (v->has_return_value()) {
      infer_any_expr(v->get_return_value(), current_function->declared_return_type);
    } else {
      assign_inferred_type(v->get_return_value(), TypeDataVoid::create());
    }
    if (current_function->does_return_self()) {
      return_unifier.unify_with(current_function->parameters[0].declared_type);
      if (!is_expr_valid_as_return_self(v->get_return_value())) {
        v->error("invalid return from `self` function");
      }
      return;
    }

    TypePtr expr_type = v->get_return_value()->inferred_type;
    if (current_function->declared_return_type) {
      if (!current_function->declared_return_type->can_rhs_be_assigned(expr_type)) {
        v->get_return_value()->error("can not convert type " + to_string(expr_type) + " to return type " + to_string(current_function->declared_return_type));
      }
    } else {
      if (!return_unifier.unify_with(expr_type)) {
        v->get_return_value()->error("can not unify type " + to_string(expr_type) + " with previous return type " + to_string(return_unifier.get_result()));
      }
    }
  }

  void process_if_statement(V<ast_if_statement> v) {
    AnyExprV cond = v->get_cond();
    infer_any_expr(cond);
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      cond->error("can not use " + to_string(cond) + " as a boolean condition");
    }
    process_any_statement(v->get_if_body());
    process_any_statement(v->get_else_body());
  }

  void process_repeat_statement(V<ast_repeat_statement> v) {
    AnyExprV cond = v->get_cond();
    infer_any_expr(cond);
    if (!expect_integer(cond)) {
      cond->error("condition of `repeat` must be an integer, got " + to_string(cond));
    }
    process_any_statement(v->get_body());
  }

  void process_while_statement(V<ast_while_statement> v) {
    AnyExprV cond = v->get_cond();
    infer_any_expr(cond);
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      cond->error("can not use " + to_string(cond) + " as a boolean condition");
    }
    process_any_statement(v->get_body());
  }

  void process_do_while_statement(V<ast_do_while_statement> v) {
    process_any_statement(v->get_body());
    AnyExprV cond = v->get_cond();
    infer_any_expr(cond);
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      cond->error("can not use " + to_string(cond) + " as a boolean condition");
    }
  }

  void process_throw_statement(V<ast_throw_statement> v) {
    infer_any_expr(v->get_thrown_code());
    if (!expect_integer(v->get_thrown_code())) {
      v->get_thrown_code()->error("excNo of `throw` must be an integer, got " + to_string(v->get_thrown_code()));
    }
    infer_any_expr(v->get_thrown_arg());
    if (v->has_thrown_arg() && v->get_thrown_arg()->inferred_type->calc_width_on_stack() != 1) {
      v->get_thrown_arg()->error("can not throw " + to_string(v->get_thrown_arg()) + ", exception arg must occupy exactly 1 stack slot");
    }
  }

  void process_assert_statement(V<ast_assert_statement> v) {
    AnyExprV cond = v->get_cond();
    infer_any_expr(cond);
    if (!expect_integer(cond) && !expect_boolean(cond)) {
      cond->error("can not use " + to_string(cond) + " as a boolean condition");
    }
    infer_any_expr(v->get_thrown_code());
    if (!expect_integer(v->get_thrown_code())) {
      v->get_cond()->error("thrown excNo of `assert` must be an integer, got " + to_string(v->get_cond()));
    }
  }

  static void process_catch_variable(AnyExprV catch_var, TypePtr catch_var_type) {
    if (auto v_ref = catch_var->try_as<ast_reference>(); v_ref && v_ref->sym) { // not underscore
      assign_inferred_type(v_ref->sym->as<LocalVarData>(), catch_var_type);
    }
    assign_inferred_type(catch_var, catch_var_type);
  }

  void process_try_catch_statement(V<ast_try_catch_statement> v) {
    process_any_statement(v->get_try_body());

    // `catch` has exactly 2 variables: excNo and arg (when missing, they are implicit underscores)
    // `arg` is a curious thing, it can be any TVM primitive, so assign unknown to it
    // hence, using `fInt(arg)` (int from parameter is a hint) or `arg as slice` works well
    // it's not truly correct, because `arg as (int,int)` also compiles, but can never happen, but let it be user responsibility
    tolk_assert(v->get_catch_expr()->size() == 2);
    std::vector<TypePtr> types_list = {TypeDataInt::create(), TypeDataUnknown::create()};
    process_catch_variable(v->get_catch_expr()->get_item(0), types_list[0]);
    process_catch_variable(v->get_catch_expr()->get_item(1), types_list[1]);
    assign_inferred_type(v->get_catch_expr(), TypeDataTensor::create(std::move(types_list)));

    process_any_statement(v->get_catch_body());
  }

public:
  static void assign_fun_full_type(const FunctionData* fun_ref, TypePtr inferred_return_type) {
    // calculate function full type `fun(params) -> ret_type`
    std::vector<TypePtr> params_types;
    params_types.reserve(fun_ref->get_num_params());
    for (const LocalVarData& param : fun_ref->parameters) {
      params_types.push_back(param.declared_type);
    }
    assign_inferred_type(fun_ref, inferred_return_type, TypeDataFunCallable::create(std::move(params_types), inferred_return_type));
  }

 void start_visiting_function(const FunctionData* fun_ref, V<ast_function_declaration> v_function) {
    if (fun_ref->is_code_function()) {
      current_function = fun_ref;
      process_any_statement(v_function->get_body());
      current_function = nullptr;

      if (fun_ref->is_implicit_return()) {
        bool is_ok_with_void = fun_ref->declared_return_type
                             ? fun_ref->declared_return_type->can_rhs_be_assigned(TypeDataVoid::create())
                             : return_unifier.unify_with_implicit_return_void();
        if (!is_ok_with_void || fun_ref->does_return_self()) {
          throw ParseError(v_function->get_body()->as<ast_sequence>()->loc_end, "missing return");
        }
      }
    } else {
      // asm functions should be strictly typed, this was checked earlier
      tolk_assert(fun_ref->declared_return_type);
    }

    TypePtr inferred_return_type = fun_ref->declared_return_type ? fun_ref->declared_return_type : return_unifier.get_result();
    assign_fun_full_type(fun_ref, inferred_return_type);
    fun_ref->mutate()->assign_is_type_inferring_done();
  }
};

class LaunchInferTypesAndMethodsOnce final {
public:
  static bool should_visit_function(const FunctionData* fun_ref) {
    // since inferring can be requested on demand, prevent second execution from a regular pipeline launcher
    return !fun_ref->is_type_inferring_done() && !fun_ref->is_generic_function();
  }

  static void start_visiting_function(const FunctionData* fun_ref, V<ast_function_declaration> v_function) {
    InferCheckTypesAndCallsAndFieldsVisitor visitor;
    visitor.start_visiting_function(fun_ref, v_function);
  }
};

// infer return type "on demand"
// example: `fun f() { return g(); } fun g() { ... }`
// when analyzing `f()`, we need to infer what fun_ref=g returns
// (if `g` is generic, it was already instantiated, so fun_ref=g<int> is here)
static void infer_and_save_return_type_of_function(const FunctionData* fun_ref) {
  static std::vector<const FunctionData*> called_stack;

  tolk_assert(!fun_ref->is_generic_function() && !fun_ref->is_type_inferring_done());
  // if `g` has return type declared, like `fun g(): int { ... }`, don't traverse its body
  if (fun_ref->declared_return_type) {
    InferCheckTypesAndCallsAndFieldsVisitor::assign_fun_full_type(fun_ref, fun_ref->declared_return_type);
    return;
  }

  // prevent recursion of untyped functions, like `fun f() { return g(); } fun g() { return f(); }`
  bool contains = std::find(called_stack.begin(), called_stack.end(), fun_ref) != called_stack.end();
  if (contains) {
    fun_ref->ast_root->error("could not infer return type of " + to_string(fun_ref) + ", because it appears in a recursive call chain; specify `: <return_type>` manually");
  }

  // dig into g's body; it's safe, since the compiler is single-threaded
  // on finish, fun_ref->inferred_return_type is filled, and won't be called anymore
  called_stack.push_back(fun_ref);
  InferCheckTypesAndCallsAndFieldsVisitor visitor;
  visitor.start_visiting_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
  called_stack.pop_back();
}

void pipeline_infer_types_and_calls_and_fields() {
  visit_ast_of_all_functions<LaunchInferTypesAndMethodsOnce>();
}

void pipeline_infer_types_and_calls_and_fields(const FunctionData* fun_ref) {
  InferCheckTypesAndCallsAndFieldsVisitor visitor;
  visitor.start_visiting_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
}

} // namespace tolk
