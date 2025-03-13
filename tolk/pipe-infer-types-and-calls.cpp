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
#include "smart-casts-cfg.h"

/*
 *   This is a complicated and crucial part of the pipeline. It simultaneously does the following:
 *   * infers types of all expressions; example: `2 + 3` both are TypeDataInt, result is also
 *   * AND binds function/method calls (assigns fun_ref); example: `globalF()`, fun_ref is assigned to `globalF` (unless generic)
 *   * AND instantiates generic functions; example: `t.tuplePush(2)` creates `tuplePush<int>` and assigns fun_ref to dot field
 *   * AND infers return type of functions if it's omitted (`fun f() { ... }` means "auto infer", not "void")
 *   * AND builds data flow graph, mostly used for smart casts (right at the time of inferring)
 *   Note, that type checking (errors about types mismatch) is a later compilation step, due to loops.
 *
 *   It's important to do all these parts simultaneously, they can't be split or separated.
 *   For example, we can't bind `f(2)` earlier, because if `f` is a generic `f<T>`, we should instantiate it,
 * and in order to do it, we need to know argument types.
 *   For example, we can't bind `c.cellHash()` earlier, because in order to bind it, we need to know object type.
 *   For example, we can't infer `var y = x` without smart casts, because if x's type is refined, it affects y.
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
 *   Control flow is represented NOT as a "graph with edges". Instead, it's a "structured DFS" for the AST:
 * 1) at every point of inferring, we have "current flow facts" (FlowContext)
 * 2) when we see an `if (...)`, we create two derived contexts (by cloning current)
 * 3) after `if`, finalize them at the end and unify
 * 4) if we detect unreachable code, we mark that path's context as "unreachable"
 *   In other words, we get the effect of a CFG but in a more direct approach. That's enough for AST-level data-flow.
 *   FlowContext contains "data-flow facts that are definitely known".
 *      // current facts: x is int?, t is (int, int)
 *      if (x != null && t.0 > 0)
 *         // current facts: x is int, t is (int, int), t.0 is positive
 *      else
 *         // current facts: x is null, t is (int, int), t.0 is not positive
 *   When branches rejoin, facts are merged back (int+null = int? and so on, here they would be equal to before if).
 *   See smart-casts-cfg.cpp for detailed comments.
 *
 *   About loops and partial re-entering. Consider the following:
 *      var x: int? = 5;
 *      // <- here x is `int` (smart cast)
 *      while (true) {
 *        // <- but here x is `int?` (not `int`) due to assignment in a loop
 *        if (...) { x = getNullableInt(); }
 *      }
 *   When building control flow, loops are inferred twice. In the above, at first iteration, x will be `int`,
 * but at the second, x will be `int?` (after merged with loop end).
 *   That's why type checking is done later, not to make false errors on the first iteration.
 *   Note, that it would also be better to postpone generics "materialization" also: here only to infer type arguments,
 * but to instantiate and re-assign fun_ref later. But it complicates the architecture significantly.
 * For now, generics may encounter problems within loops on first iteration, though it's unlikely to face this
 * in practice. (example: in the loop above, `genericFn(x)` will at first instantiate <int> and then <int?>)
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

static void infer_and_save_return_type_of_function(FunctionPtr fun_ref);
static void infer_and_save_type_of_constant(GlobalConstPtr const_ref);

static TypePtr get_or_infer_return_type(FunctionPtr fun_ref) {
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
static std::string to_string(FunctionPtr fun_ref) {
  return "`" + fun_ref->as_human_readable() + "`";
}

// fire a general error, just a wrapper over `throw`
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire(FunctionPtr cur_f, SrcLocation loc, const std::string& message) {
  throw ParseError(cur_f, loc, message);
}

// fire an error when `fun f<T>(...) asm ...` is called with T=(int,int) or other non-1 width on stack
// asm functions generally can't handle it, they expect T to be a TVM primitive
// (in FunC, `forall` type just couldn't be unified with non-primitives; in Tolk, generic T is expectedly inferred)
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_calling_asm_function_with_non1_stack_width_arg(FunctionPtr cur_f, SrcLocation loc, FunctionPtr fun_ref, const std::vector<TypePtr>& substitutions, int arg_idx) {
  fire(cur_f, loc, "can not call `" + fun_ref->as_human_readable() + "` with " + fun_ref->genericTs->get_nameT(arg_idx) + "=" + substitutions[arg_idx]->as_human_readable() + ", because it occupies " + std::to_string(substitutions[arg_idx]->get_width_on_stack()) + " stack slots in TVM, not 1");
}

// fire an error on `untypedTupleVar.0` when used without a hint
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_cannot_deduce_untyped_tuple_access(FunctionPtr cur_f, SrcLocation loc, int index) {
  std::string idx_access = "<tuple>." + std::to_string(index);
  fire(cur_f, loc, "can not deduce type of `" + idx_access + "`; either assign it to variable like `var c: int = " + idx_access + "` or cast the result like `" + idx_access + " as int`");
}


/*
 * This class handles all types of AST vertices and traverses them, filling all AnyExprV::inferred_type.
 * Note, that it isn't derived from ASTVisitor, it has manual `switch` over all existing vertex types.
 * There are two reasons for this:
 * 1) when a new AST node type is introduced, I want it to fail here, not to be left un-inferred with UB at next steps
 * 2) easy to maintain a hint (see comments at the top of the file)
 */
class InferTypesAndCallsAndFieldsVisitor final {
  FunctionPtr cur_f = nullptr;
  std::vector<AnyExprV> return_statements;

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

  static void assign_inferred_type(LocalVarPtr local_var_or_param, TypePtr inferred_type) {
#ifdef TOLK_DEBUG
    tolk_assert(inferred_type != nullptr && !inferred_type->has_unresolved_inside() && !inferred_type->has_genericT_inside());
#endif
    local_var_or_param->mutate()->assign_inferred_type(inferred_type);
  }

  static void assign_inferred_type(FunctionPtr fun_ref, TypePtr inferred_return_type, TypePtr inferred_full_type) {
#ifdef TOLK_DEBUG
    tolk_assert(inferred_return_type != nullptr && !inferred_return_type->has_unresolved_inside() && !inferred_return_type->has_genericT_inside());
#endif
    fun_ref->mutate()->assign_inferred_type(inferred_return_type, inferred_full_type);
  }

  // traverse children in any statement
  FlowContext process_any_statement(AnyV v, FlowContext&& flow) {
    switch (v->type) {
      case ast_sequence:
        return process_sequence(v->as<ast_sequence>(), std::move(flow));
      case ast_return_statement:
        return process_return_statement(v->as<ast_return_statement>(), std::move(flow));
      case ast_if_statement:
        return process_if_statement(v->as<ast_if_statement>(), std::move(flow));
      case ast_repeat_statement:
        return process_repeat_statement(v->as<ast_repeat_statement>(), std::move(flow));
      case ast_while_statement:
        return process_while_statement(v->as<ast_while_statement>(), std::move(flow));
      case ast_do_while_statement:
        return process_do_while_statement(v->as<ast_do_while_statement>(), std::move(flow));
      case ast_throw_statement:
        return process_throw_statement(v->as<ast_throw_statement>(), std::move(flow));
      case ast_assert_statement:
        return process_assert_statement(v->as<ast_assert_statement>(), std::move(flow));
      case ast_try_catch_statement:
        return process_try_catch_statement(v->as<ast_try_catch_statement>(), std::move(flow));
      case ast_empty_statement:
        return flow;
      default:
        return process_expression_statement(reinterpret_cast<AnyExprV>(v), std::move(flow));
    }
  }

  // assigns inferred_type for any expression (by calling assign_inferred_type)
  // returns ExprFlow: out_facts that are "definitely known" after evaluating the whole expression
  // if used_as_condition, true_facts/false_facts are also calculated (don't calculate them always for optimization)
  ExprFlow infer_any_expr(AnyExprV v, FlowContext&& flow, bool used_as_condition, TypePtr hint = nullptr) {
    switch (v->type) {
      case ast_int_const:
        return infer_int_const(v->as<ast_int_const>(), std::move(flow), used_as_condition);
      case ast_string_const:
        return infer_string_const(v->as<ast_string_const>(), std::move(flow), used_as_condition);
      case ast_bool_const:
        return infer_bool_const(v->as<ast_bool_const>(), std::move(flow), used_as_condition);
      case ast_local_vars_declaration:
        return infer_local_vars_declaration(v->as<ast_local_vars_declaration>(), std::move(flow), used_as_condition);
      case ast_local_var_lhs:
        return infer_local_var_lhs(v->as<ast_local_var_lhs>(), std::move(flow), used_as_condition);
      case ast_assign:
        return infer_assignment(v->as<ast_assign>(), std::move(flow), used_as_condition);
      case ast_set_assign:
        return infer_set_assign(v->as<ast_set_assign>(), std::move(flow), used_as_condition);
      case ast_unary_operator:
        return infer_unary_operator(v->as<ast_unary_operator>(), std::move(flow), used_as_condition);
      case ast_binary_operator:
        return infer_binary_operator(v->as<ast_binary_operator>(), std::move(flow), used_as_condition);
      case ast_ternary_operator:
        return infer_ternary_operator(v->as<ast_ternary_operator>(), std::move(flow), used_as_condition, hint);
      case ast_cast_as_operator:
        return infer_cast_as_operator(v->as<ast_cast_as_operator>(), std::move(flow), used_as_condition);
      case ast_not_null_operator:
        return infer_not_null_operator(v->as<ast_not_null_operator>(), std::move(flow), used_as_condition);
      case ast_is_null_check:
        return infer_is_null_check(v->as<ast_is_null_check>(), std::move(flow), used_as_condition);
      case ast_parenthesized_expression:
        return infer_parenthesized(v->as<ast_parenthesized_expression>(), std::move(flow), used_as_condition, hint);
      case ast_reference:
        return infer_reference(v->as<ast_reference>(), std::move(flow), used_as_condition);
      case ast_dot_access:
        return infer_dot_access(v->as<ast_dot_access>(), std::move(flow), used_as_condition, hint);
      case ast_function_call:
        return infer_function_call(v->as<ast_function_call>(), std::move(flow), used_as_condition, hint);
      case ast_tensor:
        return infer_tensor(v->as<ast_tensor>(), std::move(flow), used_as_condition, hint);
      case ast_typed_tuple:
        return infer_typed_tuple(v->as<ast_typed_tuple>(), std::move(flow), used_as_condition, hint);
      case ast_null_keyword:
        return infer_null_keyword(v->as<ast_null_keyword>(), std::move(flow), used_as_condition);
      case ast_underscore:
        return infer_underscore(v->as<ast_underscore>(), std::move(flow), used_as_condition, hint);
      case ast_empty_expression:
        return infer_empty_expression(v->as<ast_empty_expression>(), std::move(flow), used_as_condition);
      default:
        throw UnexpectedASTNodeType(v, "infer_any_expr");
    }
  }

  static ExprFlow infer_int_const(V<ast_int_const> v, FlowContext&& flow, bool used_as_condition) {
    assign_inferred_type(v, TypeDataInt::create());

    ExprFlow after_v(std::move(flow), used_as_condition);
    if (used_as_condition) {    // `if (0)` always false
      if (v->intval == 0) {
        after_v.true_flow.mark_unreachable(UnreachableKind::CantHappen);
      } else {
        after_v.false_flow.mark_unreachable(UnreachableKind::CantHappen);
      }
    }
    return after_v;
  }

  static ExprFlow infer_string_const(V<ast_string_const> v, FlowContext&& flow, bool used_as_condition) {
    assign_inferred_type(v, TypeDataSlice::create());
    return ExprFlow(std::move(flow), used_as_condition);
  }

  static ExprFlow infer_bool_const(V<ast_bool_const> v, FlowContext&& flow, bool used_as_condition) {
    assign_inferred_type(v, TypeDataBool::create());

    ExprFlow after_v(std::move(flow), used_as_condition);
    if (used_as_condition) {    // `if (false)` always false
      if (v->bool_val == false) {
        after_v.true_flow.mark_unreachable(UnreachableKind::CantHappen);
      } else {
        after_v.false_flow.mark_unreachable(UnreachableKind::CantHappen);
      }
    }
    return after_v;
  }

  ExprFlow infer_local_vars_declaration(V<ast_local_vars_declaration> v, FlowContext&& flow, bool used_as_condition) {
    flow = infer_any_expr(v->get_expr(), std::move(flow), used_as_condition).out_flow;
    assign_inferred_type(v, v->get_expr());
    return ExprFlow(std::move(flow), used_as_condition);
  }

  static ExprFlow infer_local_var_lhs(V<ast_local_var_lhs> v, FlowContext&& flow, bool used_as_condition) {
    // `var v = rhs`, inferring is called for `v`
    // at the moment of inferring left side of assignment, we don't know type of rhs (since lhs is executed first)
    // so, mark `v` as unknown
    // later, v's inferred_type will be reassigned; see process_assignment_lhs_after_infer_rhs()
    if (v->marked_as_redef) {
      assign_inferred_type(v, v->var_ref->declared_type);
    } else {
      assign_inferred_type(v, v->declared_type ? v->declared_type : TypeDataUnknown::create());
    }
    return ExprFlow(std::move(flow), used_as_condition);
  }

  ExprFlow infer_assignment(V<ast_assign> v, FlowContext&& flow, bool used_as_condition) {
    // v is assignment: `x = 5` / `var x = 5` / `var x: slice = 5` / `(cs,_) = f()` / `val (a,[b],_) = (a,t,0)`
    // execution flow is: lhs first, rhs second (at IR generation, also lhs is evaluated first, unlike FunC)
    // after inferring lhs, use it for hint when inferring rhs
    // example: `var i: int = t.tupleAt(0)` is ok (hint=int, T=int), but `var i = t.tupleAt(0)` not, since `tupleAt<T>(t,i): T`
    AnyExprV lhs = v->get_lhs();
    AnyExprV rhs = v->get_rhs();
    flow = infer_left_side_of_assignment(lhs, std::move(flow));
    flow = infer_any_expr(rhs, std::move(flow), false, lhs->inferred_type).out_flow;
    process_assignment_lhs_after_infer_rhs(lhs, rhs->inferred_type, flow);
    assign_inferred_type(v, rhs);     // note, that the resulting type is rhs, not lhs

    return ExprFlow(std::move(flow), used_as_condition);
  }

  // for `v = rhs` (NOT `var v = lhs`), variable `v` may be smart cast at this point
  // the purpose of this function is to drop smart casts from expressions used as left side of assignments
  // another example: `x.0 = rhs`, smart cast is dropped for `x.0` (not for `x`)
  // the goal of dropping smart casts is to have lhs->inferred_type as actually declared, used as hint to infer rhs
  FlowContext infer_left_side_of_assignment(AnyExprV lhs, FlowContext&& flow) {
    if (auto lhs_tensor = lhs->try_as<ast_tensor>()) {
      std::vector<TypePtr> types_list;
      types_list.reserve(lhs_tensor->size());
      for (int i = 0; i < lhs_tensor->size(); ++i) {
        flow = infer_left_side_of_assignment(lhs_tensor->get_item(i), std::move(flow));
        types_list.push_back(lhs_tensor->get_item(i)->inferred_type);
      }
      assign_inferred_type(lhs, TypeDataTensor::create(std::move(types_list)));

    } else if (auto lhs_tuple = lhs->try_as<ast_typed_tuple>()) {
      std::vector<TypePtr> types_list;
      types_list.reserve(lhs_tuple->size());
      for (int i = 0; i < lhs_tuple->size(); ++i) {
        flow = infer_left_side_of_assignment(lhs_tuple->get_item(i), std::move(flow));
        types_list.push_back(lhs_tuple->get_item(i)->inferred_type);
      }
      assign_inferred_type(lhs, TypeDataTypedTuple::create(std::move(types_list)));

    } else if (auto lhs_par = lhs->try_as<ast_parenthesized_expression>()) {
      flow = infer_left_side_of_assignment(lhs_par->get_expr(), std::move(flow));
      assign_inferred_type(lhs, lhs_par->get_expr()->inferred_type);

    } else {
      flow = infer_any_expr(lhs, std::move(flow), false).out_flow;
      if (extract_sink_expression_from_vertex(lhs)) {
        TypePtr lhs_declared_type = calc_declared_type_before_smart_cast(lhs);
        assign_inferred_type(lhs, lhs_declared_type);
      }
    }

    return flow;
  }

  // handle (and dig recursively) into `var lhs = rhs`
  // at this point, both lhs and rhs are already inferred, but lhs newly-declared vars are unknown (unless have declared_type)
  // examples: `var z = 5`, `var (x, [y]) = (2, [3])`, `var (x, [y]) = xy`
  // the purpose is to update inferred_type of lhs vars (z, x, y)
  // and to re-assign types of tensors/tuples inside: `var (x,[y]) = ...` was `(unknown,[unknown])`, becomes `(int,[int])`
  // while recursing, keep track of rhs if lhs and rhs have common shape (5 for z, 2 for x, [3] for [y], 3 for y)
  // (so that on type mismatch, point to corresponding rhs, example: `var (x, y:slice) = (1, 2)` point to 2
  static void process_assignment_lhs_after_infer_rhs(AnyExprV lhs, TypePtr rhs_type, FlowContext& out_flow) {
    tolk_assert(lhs->inferred_type != nullptr);

    // `var ... = rhs` - dig into left part
    if (auto lhs_decl = lhs->try_as<ast_local_vars_declaration>()) {
      process_assignment_lhs_after_infer_rhs(lhs_decl->get_expr(), rhs_type, out_flow);
      return;
    }

    // inside `var v: int = rhs` / `var _ = rhs` / `var v redef = rhs` (lhs is "v" / "_" / "v")
    if (auto lhs_var = lhs->try_as<ast_local_var_lhs>()) {
      TypePtr declared_type = lhs_var->marked_as_redef ? lhs_var->var_ref->declared_type : lhs_var->declared_type;
      if (lhs_var->inferred_type == TypeDataUnknown::create()) {
        assign_inferred_type(lhs_var, rhs_type);
        assign_inferred_type(lhs_var->var_ref, rhs_type);
      }
      TypePtr smart_casted_type = declared_type ? calc_smart_cast_type_on_assignment(declared_type, rhs_type) : rhs_type;
      out_flow.register_known_type(SinkExpression(lhs_var->var_ref), smart_casted_type);
      return;
    }

    // `(v1, v2) = rhs` / `var (v1, v2) = rhs` (rhs may be `(1,2)` or `tensorVar` or `someF()`, doesn't matter)
    // dig recursively into v1 and v2 with corresponding rhs i-th item of a tensor
    if (auto lhs_tensor = lhs->try_as<ast_tensor>()) {
      const TypeDataTensor* rhs_type_tensor = rhs_type->try_as<TypeDataTensor>();
      std::vector<TypePtr> types_list;
      types_list.reserve(lhs_tensor->size());
      for (int i = 0; i < lhs_tensor->size(); ++i) {
        TypePtr ith_rhs_type = rhs_type_tensor && i < rhs_type_tensor->size() ? rhs_type_tensor->items[i] : TypeDataUnknown::create();
        process_assignment_lhs_after_infer_rhs(lhs_tensor->get_item(i), ith_rhs_type, out_flow);
        types_list.push_back(lhs_tensor->get_item(i)->inferred_type);
      }
      assign_inferred_type(lhs, TypeDataTensor::create(std::move(types_list)));
      return;
    }

    // `[v1, v2] = rhs` / `var [v1, v2] = rhs` (rhs may be `[1,2]` or `tupleVar` or `someF()`, doesn't matter)
    // dig recursively into v1 and v2 with corresponding rhs i-th item of a tuple
    if (auto lhs_tuple = lhs->try_as<ast_typed_tuple>()) {
      const TypeDataTypedTuple* rhs_type_tuple = rhs_type->try_as<TypeDataTypedTuple>();
      std::vector<TypePtr> types_list;
      types_list.reserve(lhs_tuple->size());
      for (int i = 0; i < lhs_tuple->size(); ++i) {
        TypePtr ith_rhs_type = rhs_type_tuple && i < rhs_type_tuple->size() ? rhs_type_tuple->items[i] : TypeDataUnknown::create();
        process_assignment_lhs_after_infer_rhs(lhs_tuple->get_item(i), ith_rhs_type, out_flow);
        types_list.push_back(lhs_tuple->get_item(i)->inferred_type);
      }
      assign_inferred_type(lhs, TypeDataTypedTuple::create(std::move(types_list)));
      return;
    }

    // `(v) = (rhs)`, just surrounded by parenthesis
    if (auto lhs_par = lhs->try_as<ast_parenthesized_expression>()) {
      process_assignment_lhs_after_infer_rhs(lhs_par->get_expr(), rhs_type, out_flow);
      assign_inferred_type(lhs, lhs_par->get_expr());
      return;
    }

    // here is `v = rhs` (just assignment, not `var v = rhs`) / `a.0 = rhs` / `getObj(z=f()).0 = rhs` etc.
    // for instance, `tensorVar.0 = rhs` / `obj.field = rhs` has already checked index correctness while inferring lhs
    // for strange lhs like `f() = rhs` type inferring (and later checking) will pass, but will fail lvalue check later
    if (SinkExpression s_expr = extract_sink_expression_from_vertex(lhs)) {
      TypePtr lhs_declared_type = calc_declared_type_before_smart_cast(lhs);
      TypePtr smart_casted_type = calc_smart_cast_type_on_assignment(lhs_declared_type, rhs_type);
      out_flow.register_known_type(s_expr, smart_casted_type);
      assign_inferred_type(lhs, lhs_declared_type);
    }
  }

  ExprFlow infer_set_assign(V<ast_set_assign> v, FlowContext&& flow, bool used_as_condition) {
    AnyExprV lhs = v->get_lhs();
    AnyExprV rhs = v->get_rhs();
    ExprFlow after_lhs = infer_any_expr(lhs, std::move(flow), false);
    FlowContext rhs_flow = std::move(after_lhs.out_flow);
    ExprFlow after_rhs = infer_any_expr(rhs, std::move(rhs_flow), false, lhs->inferred_type);

    // all operators implementation is hardcoded by built-in functions `_+_` and similar
    std::string_view builtin_func = v->operator_name;   // "+" for operator +=

    assign_inferred_type(v, lhs);

    FunctionPtr builtin_sym = lookup_global_symbol("_" + static_cast<std::string>(builtin_func) + "_")->try_as<FunctionPtr>();
    v->mutate()->assign_fun_ref(builtin_sym);

    return ExprFlow(std::move(after_rhs.out_flow), used_as_condition);
  }

  ExprFlow infer_unary_operator(V<ast_unary_operator> v, FlowContext&& flow, bool used_as_condition) {
    AnyExprV rhs = v->get_rhs();
    ExprFlow after_rhs = infer_any_expr(rhs, std::move(flow), used_as_condition);

    // all operators implementation is hardcoded by built-in functions `~_` and similar
    std::string_view builtin_func = v->operator_name;

    switch (v->tok) {
      case tok_minus:
      case tok_plus:
      case tok_bitwise_not:
        assign_inferred_type(v, TypeDataInt::create());
        break;
      case tok_logical_not:
        if (rhs->inferred_type == TypeDataBool::create()) {
          builtin_func = "!b";  // "overloaded" for bool
        }
        assign_inferred_type(v, TypeDataBool::create());
        std::swap(after_rhs.false_flow, after_rhs.true_flow);
        break;
      default:
        tolk_assert(false);
    }

    FunctionPtr builtin_sym = lookup_global_symbol(static_cast<std::string>(builtin_func) + "_")->try_as<FunctionPtr>();
    v->mutate()->assign_fun_ref(builtin_sym);

    return after_rhs;
  }

  ExprFlow infer_binary_operator(V<ast_binary_operator> v, FlowContext&& flow, bool used_as_condition) {
    AnyExprV lhs = v->get_lhs();
    AnyExprV rhs = v->get_rhs();

    // almost all operators implementation is hardcoded by built-in functions `_+_` and similar
    std::string_view builtin_func = v->operator_name;

    switch (v->tok) {
      // comparison operators, returning bool
      case tok_eq:
      case tok_neq:
      case tok_lt:
      case tok_gt:
      case tok_leq:
      case tok_geq:
      case tok_spaceship:
        flow = infer_any_expr(lhs, std::move(flow), false).out_flow;
        flow = infer_any_expr(rhs, std::move(flow), false).out_flow;
        assign_inferred_type(v, TypeDataBool::create());
        break;
      // & | ^ are "overloaded" both for integers and booleans
      case tok_bitwise_and:
      case tok_bitwise_or:
      case tok_bitwise_xor:
        flow = infer_any_expr(lhs, std::move(flow), false).out_flow;
        flow = infer_any_expr(rhs, std::move(flow), false).out_flow;
        if (lhs->inferred_type == TypeDataBool::create() && rhs->inferred_type == TypeDataBool::create()) {
          assign_inferred_type(v, TypeDataBool::create());
        } else {
          assign_inferred_type(v, TypeDataInt::create());
        }
        break;
      // && || result in booleans, but building flow facts is tricky due to short-circuit
      case tok_logical_and: {
        ExprFlow after_lhs = infer_any_expr(lhs, std::move(flow), true);
        ExprFlow after_rhs = infer_any_expr(rhs, std::move(after_lhs.true_flow), true);
        assign_inferred_type(v, TypeDataBool::create());
        if (!used_as_condition) {
          FlowContext out_flow = FlowContext::merge_flow(std::move(after_lhs.false_flow), std::move(after_rhs.out_flow));
          return ExprFlow(std::move(out_flow), false);
        }
        FlowContext out_flow = FlowContext::merge_flow(std::move(after_lhs.out_flow), std::move(after_rhs.out_flow));
        FlowContext true_flow = std::move(after_rhs.true_flow);
        FlowContext false_flow = FlowContext::merge_flow(std::move(after_lhs.false_flow), std::move(after_rhs.false_flow));
        return ExprFlow(std::move(out_flow), std::move(true_flow), std::move(false_flow));
      }
      case tok_logical_or: {
        ExprFlow after_lhs = infer_any_expr(lhs, std::move(flow), true);
        ExprFlow after_rhs = infer_any_expr(rhs, std::move(after_lhs.false_flow), true);
        assign_inferred_type(v, TypeDataBool::create());
        if (!used_as_condition) {
          FlowContext out_flow = FlowContext::merge_flow(std::move(after_lhs.true_flow), std::move(after_rhs.out_flow));
          return ExprFlow(std::move(after_rhs.out_flow), false);
        }
        FlowContext out_flow = FlowContext::merge_flow(std::move(after_lhs.out_flow), std::move(after_rhs.out_flow));
        FlowContext true_flow = FlowContext::merge_flow(std::move(after_lhs.true_flow), std::move(after_rhs.true_flow));
        FlowContext false_flow = std::move(after_rhs.false_flow);
        return ExprFlow(std::move(out_flow), std::move(true_flow), std::move(false_flow));
      }
      // others are mathematical: + * ...
      // they are allowed for intN (int16 + int32 is ok) and always "fall back" to general int
      default:
        flow = infer_any_expr(lhs, std::move(flow), false).out_flow;
        flow = infer_any_expr(rhs, std::move(flow), false).out_flow;
        if ((v->tok == tok_plus || v->tok == tok_minus) && lhs->inferred_type == TypeDataCoins::create()) {
          assign_inferred_type(v, TypeDataCoins::create());   // coins + coins = coins
        } else {
          assign_inferred_type(v, TypeDataInt::create());     // int8 + int8 = int, as well as other operators/types
        }
    }

    if (!builtin_func.empty()) {
      FunctionPtr builtin_sym = lookup_global_symbol("_" + static_cast<std::string>(builtin_func) + "_")->try_as<FunctionPtr>();
      v->mutate()->assign_fun_ref(builtin_sym);
    }

    return ExprFlow(std::move(flow), used_as_condition);
  }

  ExprFlow infer_ternary_operator(V<ast_ternary_operator> v, FlowContext&& flow, bool used_as_condition, TypePtr hint) {
    ExprFlow after_cond = infer_any_expr(v->get_cond(), std::move(flow), true);
    v->get_cond()->mutate()->assign_always_true_or_false(after_cond.get_always_true_false_state());

    ExprFlow after_true = infer_any_expr(v->get_when_true(), std::move(after_cond.true_flow), used_as_condition, hint);
    ExprFlow after_false = infer_any_expr(v->get_when_false(), std::move(after_cond.false_flow), used_as_condition, hint);

    if (v->get_cond()->is_always_true) {
      assign_inferred_type(v, v->get_when_true());
      return after_true;
    }
    if (v->get_cond()->is_always_false) {
      assign_inferred_type(v, v->get_when_false());
      return after_false;
    }

    TypeInferringUnifyStrategy tern_type;
    tern_type.unify_with(v->get_when_true()->inferred_type);
    if (!tern_type.unify_with(v->get_when_false()->inferred_type)) {
      fire(cur_f, v->loc, "types of ternary branches are incompatible: " + to_string(v->get_when_true()) + " and " + to_string(v->get_when_false()));
    }
    assign_inferred_type(v, tern_type.get_result());

    FlowContext out_flow = FlowContext::merge_flow(std::move(after_true.out_flow), std::move(after_false.out_flow));
    return ExprFlow(std::move(out_flow), std::move(after_true.true_flow), std::move(after_false.false_flow));
  }

  ExprFlow infer_cast_as_operator(V<ast_cast_as_operator> v, FlowContext&& flow, bool used_as_condition) {
    // for `expr as <type>`, use this type for hint, so that `t.tupleAt(0) as int` is ok
    ExprFlow after_expr = infer_any_expr(v->get_expr(), std::move(flow), false, v->cast_to_type);
    assign_inferred_type(v, v->cast_to_type);

    if (!used_as_condition) {
      return after_expr;
    }
    return ExprFlow(std::move(after_expr.out_flow), true);
  }

  ExprFlow infer_is_null_check(V<ast_is_null_check> v, FlowContext&& flow, bool used_as_condition) {
    ExprFlow after_expr = infer_any_expr(v->get_expr(), std::move(flow), false);
    assign_inferred_type(v, TypeDataBool::create());

    TypePtr expr_type = v->get_expr()->inferred_type;
    TypePtr non_null_type = calculate_type_subtract_null(expr_type);
    if (expr_type == TypeDataNullLiteral::create()) {             // `expr == null` is always true
      v->mutate()->assign_always_true_or_false(v->is_negated ? 2 : 1);
    } else if (non_null_type == TypeDataNever::create()) {        // `expr == null` is always false
      v->mutate()->assign_always_true_or_false(v->is_negated ? 1 : 2);
    } else {
      v->mutate()->assign_always_true_or_false(0);
    }

    if (!used_as_condition) {
      return after_expr;
    }

    FlowContext true_flow = after_expr.out_flow.clone();
    FlowContext false_flow = after_expr.out_flow.clone();
    if (SinkExpression s_expr = extract_sink_expression_from_vertex(v->get_expr())) {
      if (v->is_always_true) {
        false_flow.mark_unreachable(UnreachableKind::CantHappen);
        false_flow.register_known_type(s_expr, TypeDataNever::create());
      } else if (v->is_always_false) {
        true_flow.mark_unreachable(UnreachableKind::CantHappen);
        true_flow.register_known_type(s_expr, TypeDataNever::create());
      } else if (!v->is_negated) {
        true_flow.register_known_type(s_expr, TypeDataNullLiteral::create());
        false_flow.register_known_type(s_expr, non_null_type);
      } else {
        true_flow.register_known_type(s_expr, non_null_type);
        false_flow.register_known_type(s_expr, TypeDataNullLiteral::create());
      }
    }
    return ExprFlow(std::move(after_expr.out_flow), std::move(true_flow), std::move(false_flow));
  }

  ExprFlow infer_not_null_operator(V<ast_not_null_operator> v, FlowContext&& flow, bool used_as_condition) {
    ExprFlow after_expr = infer_any_expr(v->get_expr(), std::move(flow), false);

    if (const auto* as_nullable = v->get_expr()->inferred_type->try_as<TypeDataNullable>()) {
      assign_inferred_type(v, as_nullable->inner);
    } else {
      assign_inferred_type(v, v->get_expr());
    }

    if (!used_as_condition) {
      return after_expr;
    }
    return ExprFlow(std::move(after_expr.out_flow), true);
  }

  ExprFlow infer_parenthesized(V<ast_parenthesized_expression> v, FlowContext&& flow, bool used_as_condition, TypePtr hint) {
    ExprFlow after_expr = infer_any_expr(v->get_expr(), std::move(flow), used_as_condition, hint);
    assign_inferred_type(v, v->get_expr());
    return after_expr;
  }

  ExprFlow infer_reference(V<ast_reference> v, FlowContext&& flow, bool used_as_condition) {
    if (LocalVarPtr var_ref = v->sym->try_as<LocalVarPtr>()) {
      TypePtr declared_or_smart_casted = flow.smart_cast_if_exists(SinkExpression(var_ref));
      tolk_assert(declared_or_smart_casted != nullptr);   // all local vars are presented in flow
      assign_inferred_type(v, declared_or_smart_casted);

    } else if (GlobalConstPtr const_ref = v->sym->try_as<GlobalConstPtr>()) {
      if (!const_ref->inferred_type) {
        infer_and_save_type_of_constant(const_ref);
      }
      assign_inferred_type(v, const_ref->inferred_type);

    } else if (GlobalVarPtr glob_ref = v->sym->try_as<GlobalVarPtr>()) {
      // there are no smart casts for globals, it's a way of preventing reading one global multiple times, it costs gas
      assign_inferred_type(v, glob_ref->declared_type);

    } else if (FunctionPtr fun_ref = v->sym->try_as<FunctionPtr>()) {
      // it's `globalF` / `globalF<int>` - references to functions used as non-call
      V<ast_instantiationT_list> v_instantiationTs = v->get_instantiationTs();

      if (fun_ref->is_generic_function() && !v_instantiationTs) {
        // `genericFn` is invalid as non-call, can't be used without <instantiation>
        fire(cur_f, v->loc, "can not use a generic function " + to_string(fun_ref) + " as non-call");

      } else if (fun_ref->is_generic_function()) {
        // `genericFn<int>` is valid, it's a reference to instantiation
        std::vector<TypePtr> substitutions = collect_fun_generic_substitutions_from_manually_specified(v->loc, fun_ref, v_instantiationTs);
        fun_ref = check_and_instantiate_generic_function(v->loc, fun_ref, std::move(substitutions));
        v->mutate()->assign_sym(fun_ref);

      } else if (v_instantiationTs != nullptr && !fun_ref->is_instantiation_of_generic_function()) {
        // non-generic function referenced like `return beginCell<builder>;`
        fire(cur_f, v_instantiationTs->loc, "not generic function used with generic T");
      }

      fun_ref->mutate()->assign_is_used_as_noncall();
      get_or_infer_return_type(fun_ref);
      assign_inferred_type(v, fun_ref->inferred_full_type);
      return ExprFlow(std::move(flow), used_as_condition);

    } else {
      tolk_assert(false);
    }

    // for non-functions: `local_var<int>` and similar not allowed
    if (UNLIKELY(v->has_instantiationTs())) {
      fire(cur_f, v->get_instantiationTs()->loc, "generic T not expected here");
    }
    return ExprFlow(std::move(flow), used_as_condition);
  }

  // given `genericF<int, slice>` / `t.tupleFirst<cell>` (the user manually specified instantiation Ts),
  // validate and collect them
  // returns: [int, slice] / [cell]
  std::vector<TypePtr> collect_fun_generic_substitutions_from_manually_specified(SrcLocation loc, FunctionPtr fun_ref, V<ast_instantiationT_list> instantiationT_list) const {
    if (fun_ref->genericTs->size() != instantiationT_list->get_items().size()) {
      fire(cur_f, loc, "wrong count of generic T: expected " + std::to_string(fun_ref->genericTs->size()) + ", got " + std::to_string(instantiationT_list->size()));
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
  FunctionPtr check_and_instantiate_generic_function(SrcLocation loc, FunctionPtr fun_ref, std::vector<TypePtr>&& substitutionTs) const {
    // T for asm function must be a TVM primitive (width 1), otherwise, asm would act incorrectly
    if (fun_ref->is_asm_function() || fun_ref->is_builtin_function()) {
      for (int i = 0; i < static_cast<int>(substitutionTs.size()); ++i) {
        if (substitutionTs[i]->get_width_on_stack() != 1) {
          fire_error_calling_asm_function_with_non1_stack_width_arg(cur_f, loc, fun_ref, substitutionTs, i);
        }
      }
    }

    std::string inst_name = generate_instantiated_name(fun_ref->name, substitutionTs);
    // make deep clone of `f<T>` with substitutionTs
    // (if `f<int>` was already instantiated, it will be immediately returned from a symbol table)
    return instantiate_generic_function(loc, fun_ref, inst_name, std::move(substitutionTs));
  }

  ExprFlow infer_dot_access(V<ast_dot_access> v, FlowContext&& flow, bool used_as_condition, TypePtr hint) {
    // it's NOT a method call `t.tupleSize()` (since such cases are handled by infer_function_call)
    // it's `t.0`, `getUser().id`, and `t.tupleSize` (as a reference, not as a call)
    flow = infer_any_expr(v->get_obj(), std::move(flow), false).out_flow;

    TypePtr obj_type = v->get_obj()->inferred_type;
    // our goal is to fill v->target knowing type of obj
    V<ast_identifier> v_ident = v->get_identifier();    // field/method name vertex
    V<ast_instantiationT_list> v_instantiationTs = v->get_instantiationTs();
    std::string_view field_name = v_ident->name;

    // it can be indexed access (`tensorVar.0`, `tupleVar.1`) or a method (`t.tupleSize`)
    // at first, check for indexed access
    if (field_name[0] >= '0' && field_name[0] <= '9') {
      int index_at = std::stoi(std::string(field_name));
      if (const auto* t_tensor = obj_type->try_as<TypeDataTensor>()) {
        if (index_at >= t_tensor->size()) {
          fire(cur_f, v_ident->loc, "invalid tensor index, expected 0.." + std::to_string(t_tensor->items.size() - 1));
        }
        v->mutate()->assign_target(index_at);
        TypePtr inferred_type = t_tensor->items[index_at];
        if (SinkExpression s_expr = extract_sink_expression_from_vertex(v)) {
          if (TypePtr smart_casted = flow.smart_cast_if_exists(s_expr)) {
            inferred_type = smart_casted;
          }
        }
        assign_inferred_type(v, inferred_type);
        return ExprFlow(std::move(flow), used_as_condition);
      }
      if (const auto* t_tuple = obj_type->try_as<TypeDataTypedTuple>()) {
        if (index_at >= t_tuple->size()) {
          fire(cur_f, v_ident->loc, "invalid tuple index, expected 0.." + std::to_string(t_tuple->items.size() - 1));
        }
        v->mutate()->assign_target(index_at);
        TypePtr inferred_type = t_tuple->items[index_at];
        if (SinkExpression s_expr = extract_sink_expression_from_vertex(v)) {
          if (TypePtr smart_casted = flow.smart_cast_if_exists(s_expr)) {
            inferred_type = smart_casted;
          }
        }
        assign_inferred_type(v, inferred_type);
        return ExprFlow(std::move(flow), used_as_condition);
      }
      if (obj_type->try_as<TypeDataTuple>()) {
        TypePtr item_type = nullptr;
        if (v->is_lvalue && !hint) {     // left side of assignment
          item_type = TypeDataUnknown::create();
        } else {
          if (hint == nullptr) {
            fire_error_cannot_deduce_untyped_tuple_access(cur_f, v->loc, index_at);
          }
          item_type = hint;
        }
        v->mutate()->assign_target(index_at);
        assign_inferred_type(v, item_type);
        return ExprFlow(std::move(flow), used_as_condition);
      }
      fire(cur_f, v_ident->loc, "type " + to_string(obj_type) + " is not indexable");
    }

    // for now, Tolk doesn't have fields and object-scoped methods; `t.tupleSize` is a global function `tupleSize`
    const Symbol* sym = lookup_global_symbol(field_name);
    FunctionPtr fun_ref = sym ? sym->try_as<FunctionPtr>() : nullptr;
    if (!fun_ref) {
      fire(cur_f, v_ident->loc, "non-existing field `" + static_cast<std::string>(field_name) + "` of type " + to_string(obj_type));
    }

    // `t.tupleSize` is ok, `cs.tupleSize` not
    if (!fun_ref->parameters[0].declared_type->can_rhs_be_assigned(obj_type)) {
      fire(cur_f, v_ident->loc, "referencing a method for " + to_string(fun_ref->parameters[0].declared_type) + " with object of type " + to_string(obj_type));
    }

    if (fun_ref->is_generic_function() && !v_instantiationTs) {
      // `genericFn` and `t.tupleAt` are invalid as non-call, they can't be used without <instantiation>
      fire(cur_f, v->loc, "can not use a generic function " + to_string(fun_ref) + " as non-call");

    } else if (fun_ref->is_generic_function()) {
      // `t.tupleAt<slice>` is valid, it's a reference to instantiation
      std::vector<TypePtr> substitutions = collect_fun_generic_substitutions_from_manually_specified(v->loc, fun_ref, v_instantiationTs);
      fun_ref = check_and_instantiate_generic_function(v->loc, fun_ref, std::move(substitutions));

    } else if (UNLIKELY(v_instantiationTs != nullptr)) {
      // non-generic method referenced like `var cb = c.cellHash<int>;`
      fire(cur_f, v_instantiationTs->loc, "not generic function used with generic T");
    }

    fun_ref->mutate()->assign_is_used_as_noncall();
    v->mutate()->assign_target(fun_ref);
    get_or_infer_return_type(fun_ref);
    assign_inferred_type(v, fun_ref->inferred_full_type);   // type of `t.tupleSize` is TypeDataFunCallable
    return ExprFlow(std::move(flow), used_as_condition);
  }

  ExprFlow infer_function_call(V<ast_function_call> v, FlowContext&& flow, bool used_as_condition, TypePtr hint) {
    AnyExprV callee = v->get_callee();

    // v is `globalF(args)` / `globalF<int>(args)` / `obj.method(args)` / `local_var(args)` / `getF()(args)`
    int delta_self = 0;
    AnyExprV dot_obj = nullptr;
    FunctionPtr fun_ref = nullptr;
    V<ast_instantiationT_list> v_instantiationTs = nullptr;

    if (auto v_ref = callee->try_as<ast_reference>()) {
      // `globalF()` / `globalF<int>()` / `local_var()` / `SOME_CONST()`
      fun_ref = v_ref->sym->try_as<FunctionPtr>();         // not null for `globalF`
      v_instantiationTs = v_ref->get_instantiationTs();     // present for `globalF<int>()`

    } else if (auto v_dot = callee->try_as<ast_dot_access>()) {
      // `obj.someMethod()` / `obj.someMethod<int>()` / `getF().someMethod()` / `obj.SOME_CONST()`
      // note, that dot_obj->target is not filled yet, since callee was not inferred yet
      delta_self = 1;
      dot_obj = v_dot->get_obj();
      v_instantiationTs = v_dot->get_instantiationTs();     // present for `obj.someMethod<int>()`
      flow = infer_any_expr(dot_obj, std::move(flow), false).out_flow;

      // it can be indexed access (`tensorVar.0()`, `tupleVar.1()`) or a method (`t.tupleSize()`)
      std::string_view field_name = v_dot->get_field_name();
      if (field_name[0] >= '0' && field_name[0] <= '9') {
        // indexed access `ab.2()`, then treat `ab.2` just like an expression, fun_ref remains nullptr
        // infer_dot_access() will be called for a callee, it will check index correctness
      } else {
        // for now, Tolk doesn't have fields and object-scoped methods; `t.tupleSize` is a global function `tupleSize`
        const Symbol* sym = lookup_global_symbol(field_name);
        fun_ref = sym ? sym->try_as<FunctionPtr>() : nullptr;
        if (!fun_ref) {
          fire(cur_f, v_dot->get_identifier()->loc, "non-existing method `" + static_cast<std::string>(field_name) + "` of type " + to_string(dot_obj));
        }
      }

    } else {
      // `getF()()` / `5()`
      // fun_ref remains nullptr
    }

    // handle `local_var()` / `getF()()` / `5()` / `SOME_CONST()` / `obj.method()()()` / `tensorVar.0()`
    if (!fun_ref) {
      // treat callee like a usual expression
      flow = infer_any_expr(callee, std::move(flow), false).out_flow;
      // it must have "callable" inferred type
      const TypeDataFunCallable* f_callable = callee->inferred_type->try_as<TypeDataFunCallable>();
      if (!f_callable) {   // `5()` / `SOME_CONST()` / `null()`
        fire(cur_f, v->loc, "calling a non-function " + to_string(callee->inferred_type));
      }
      // check arguments count (their types will be checked in a later pipe)
      if (v->get_num_args() != static_cast<int>(f_callable->params_types.size())) {
        fire(cur_f, v->loc, "expected " + std::to_string(f_callable->params_types.size()) + " arguments, got " + std::to_string(v->get_arg_list()->size()));
      }
      for (int i = 0; i < v->get_num_args(); ++i) {
        auto arg_i = v->get_arg(i)->get_expr();
        flow = infer_any_expr(arg_i, std::move(flow), false, f_callable->params_types[i]).out_flow;
        assign_inferred_type(v->get_arg(i), arg_i);
      }
      v->mutate()->assign_fun_ref(nullptr);   // no fun_ref to a global function
      assign_inferred_type(v, f_callable->return_type);
      return ExprFlow(std::move(flow), used_as_condition);
    }

    // so, we have a call `f(args)` or `obj.f(args)`, f is a global function (fun_ref) (code / asm / builtin)
    // we're going to iterate over passed arguments, and (if generic) infer substitutionTs
    // at first, check arguments count (Tolk doesn't have optional parameters, so just compare counts)
    int n_arguments = v->get_num_args() + delta_self;
    int n_parameters = fun_ref->get_num_params();
    if (!n_parameters && dot_obj) {
      fire(cur_f, v->loc, "`" + fun_ref->name + "` has no parameters and can not be called as method");
    }
    if (n_parameters < n_arguments) {
      fire(cur_f, v->loc, "too many arguments in call to `" + fun_ref->name + "`, expected " + std::to_string(n_parameters - delta_self) + ", have " + std::to_string(n_arguments - delta_self));
    }
    if (n_arguments < n_parameters) {
      fire(cur_f, v->loc, "too few arguments in call to `" + fun_ref->name + "`, expected " + std::to_string(n_parameters - delta_self) + ", have " + std::to_string(n_arguments - delta_self));
    }

    // now, for every passed argument, we need to infer its type
    // for regular functions, it's obvious
    // but for generic functions, we need to infer type arguments (substitutionTs) on the fly
    // (unless Ts are specified by a user like `f<int>(args)` / `t.tupleAt<slice>()`, take them)
    GenericSubstitutionsDeduceForCall* deducingTs = fun_ref->is_generic_function() ? new GenericSubstitutionsDeduceForCall(fun_ref) : nullptr;
    if (deducingTs && v_instantiationTs) {
      deducingTs->provide_manually_specified(collect_fun_generic_substitutions_from_manually_specified(v->loc, fun_ref, v_instantiationTs));
    }

    // loop over every argument, for `obj.method()` obj is the first one
    // if genericT deducing has a conflict, ParseError is thrown
    // note, that deducing Ts one by one is important to manage control flow (mutate params work like assignments)
    // a corner case, e.g. `f<T>(v1:T?, v2:T?)` and `f(null,2)` will fail on first argument, won't try the second one
    if (dot_obj) {
      const LocalVarData& param_0 = fun_ref->parameters[0];
      TypePtr param_type = param_0.declared_type;
      if (param_type->has_genericT_inside()) {
        param_type = deducingTs->auto_deduce_from_argument(cur_f, dot_obj->loc, param_type, dot_obj->inferred_type);
      }
      if (param_0.is_mutate_parameter() && dot_obj->inferred_type != param_type) {
        if (SinkExpression s_expr = extract_sink_expression_from_vertex(dot_obj)) {
          assign_inferred_type(dot_obj, calc_declared_type_before_smart_cast(dot_obj));
          flow.register_known_type(s_expr, param_type);
        }
      }
    }
    for (int i = 0; i < v->get_num_args(); ++i) {
      const LocalVarData& param_i = fun_ref->parameters[delta_self + i];
      AnyExprV arg_i = v->get_arg(i)->get_expr();
      TypePtr param_type = param_i.declared_type;
      if (param_type->has_genericT_inside() && deducingTs->is_manually_specified()) {   // `f<int>(a)`
        param_type = deducingTs->replace_by_manually_specified(param_type);
      }
      if (param_type->has_genericT_inside()) {    // `f(a)` where f is generic: use `a` to infer param type
        // then arg_i is inferred without any hint
        flow = infer_any_expr(arg_i, std::move(flow), false).out_flow;
        param_type = deducingTs->auto_deduce_from_argument(cur_f, arg_i->loc, param_type, arg_i->inferred_type);
      } else {
        // param_type is hint, helps infer arg_i
        flow = infer_any_expr(arg_i, std::move(flow), false, param_type).out_flow;
      }
      assign_inferred_type(v->get_arg(i), arg_i);  // arg itself is an expression
      if (param_i.is_mutate_parameter() && arg_i->inferred_type != param_type) {
        if (SinkExpression s_expr = extract_sink_expression_from_vertex(arg_i)) {
          assign_inferred_type(arg_i, calc_declared_type_before_smart_cast(arg_i));
          flow.register_known_type(s_expr, param_type);
        }
      }
    }

    // if it's a generic function `f<T>`, we need to instantiate it, like `f<int>`
    // same for generic methods `t.tupleAt<T>`, need to achieve `t.tupleAt<int>`

    if (fun_ref->is_generic_function()) {
      // if `f(args)` was called, Ts were inferred; check that all of them are known
      int idx = deducingTs->get_first_not_deduced_idx();
      if (idx != -1 && hint && fun_ref->declared_return_type->has_genericT_inside()) {
        // example: `t.tupleFirst()`, T doesn't depend on arguments, but is determined by return type
        // if used like `var x: int = t.tupleFirst()` / `t.tupleFirst() as int` / etc., use hint
        deducingTs->auto_deduce_from_argument(cur_f, v->loc, fun_ref->declared_return_type, hint);
        idx = deducingTs->get_first_not_deduced_idx();
      }
      if (idx != -1) {
        fire(cur_f, v->loc, "can not deduce " + fun_ref->genericTs->get_nameT(idx));
      }
      fun_ref = check_and_instantiate_generic_function(v->loc, fun_ref, deducingTs->flush());
      delete deducingTs;

    } else if (UNLIKELY(v_instantiationTs != nullptr)) {
      // non-generic function/method called with type arguments, like `c.cellHash<int>()` / `beginCell<builder>()`
      fire(cur_f, v_instantiationTs->loc, "calling a not generic function with generic T");
    }

    v->mutate()->assign_fun_ref(fun_ref);
    // since for `t.tupleAt()`, infer_dot_access() not called for callee = "t.tupleAt", assign its target here
    if (v->is_dot_call()) {
      v->get_callee()->as<ast_dot_access>()->mutate()->assign_target(fun_ref);
    }
    // get return type either from user-specified declaration or infer here on demand traversing its body
    get_or_infer_return_type(fun_ref);
    TypePtr inferred_type = dot_obj && fun_ref->does_return_self() ? dot_obj->inferred_type : fun_ref->inferred_return_type;
    assign_inferred_type(v, inferred_type);
    assign_inferred_type(callee, fun_ref->inferred_full_type);
    if (inferred_type == TypeDataNever::create()) {
      flow.mark_unreachable(UnreachableKind::CallNeverReturnFunction);
    }
    // note, that mutate params don't affect typing, they are handled when converting to IR
    return ExprFlow(std::move(flow), used_as_condition);
  }

  ExprFlow infer_tensor(V<ast_tensor> v, FlowContext&& flow, bool used_as_condition, TypePtr hint) {
    const TypeDataTensor* tensor_hint = hint ? hint->try_as<TypeDataTensor>() : nullptr;
    std::vector<TypePtr> types_list;
    types_list.reserve(v->get_items().size());
    for (int i = 0; i < v->size(); ++i) {
      AnyExprV item = v->get_item(i);
      flow = infer_any_expr(item, std::move(flow), false, tensor_hint && i < tensor_hint->size() ? tensor_hint->items[i] : nullptr).out_flow;
      types_list.emplace_back(item->inferred_type);
    }
    assign_inferred_type(v, TypeDataTensor::create(std::move(types_list)));
    return ExprFlow(std::move(flow), used_as_condition);
  }

  ExprFlow infer_typed_tuple(V<ast_typed_tuple> v, FlowContext&& flow, bool used_as_condition, TypePtr hint) {
    const TypeDataTypedTuple* tuple_hint = hint ? hint->try_as<TypeDataTypedTuple>() : nullptr;
    std::vector<TypePtr> types_list;
    types_list.reserve(v->get_items().size());
    for (int i = 0; i < v->size(); ++i) {
      AnyExprV item = v->get_item(i);
      flow = infer_any_expr(item, std::move(flow), false, tuple_hint && i < tuple_hint->size() ? tuple_hint->items[i] : nullptr).out_flow;
      types_list.emplace_back(item->inferred_type);
    }
    assign_inferred_type(v, TypeDataTypedTuple::create(std::move(types_list)));
    return ExprFlow(std::move(flow), used_as_condition);
  }

  static ExprFlow infer_null_keyword(V<ast_null_keyword> v, FlowContext&& flow, bool used_as_condition) {
    assign_inferred_type(v, TypeDataNullLiteral::create());

    return ExprFlow(std::move(flow), used_as_condition);
  }

  static ExprFlow infer_underscore(V<ast_underscore> v, FlowContext&& flow, bool used_as_condition, TypePtr hint) {
    // if execution is here, underscore is either used as lhs of assignment, or incorrectly, like `f(_)`
    // more precise is to always set unknown here, but for incorrect usages, instead of an error
    // "can not pass unknown to X" would better be an error it can't be used as a value, at later steps
    assign_inferred_type(v, hint ? hint : TypeDataUnknown::create());
    return ExprFlow(std::move(flow), used_as_condition);
  }

  static ExprFlow infer_empty_expression(V<ast_empty_expression> v, FlowContext&& flow, bool used_as_condition) {
    assign_inferred_type(v, TypeDataUnknown::create());
    return ExprFlow(std::move(flow), used_as_condition);
  }

  FlowContext process_sequence(V<ast_sequence> v, FlowContext&& flow) {
    // we'll print a warning if after some statement, control flow became unreachable
    // (but don't print a warning if it's already unreachable, for example we're inside always-false if)
    bool initially_unreachable = flow.is_unreachable();
    for (AnyV item : v->get_items()) {
      if (flow.is_unreachable() && !initially_unreachable && !v->first_unreachable && item->type != ast_empty_statement) {
        v->mutate()->assign_first_unreachable(item);    // a warning will be printed later, after type checking
      }
      flow = process_any_statement(item, std::move(flow));
    }
    return flow;
  }

  FlowContext process_return_statement(V<ast_return_statement> v, FlowContext&& flow) {
    if (v->has_return_value()) {
      flow = infer_any_expr(v->get_return_value(), std::move(flow), false, cur_f->declared_return_type).out_flow;
    } else {
      assign_inferred_type(v->get_return_value(), TypeDataVoid::create());
    }
    flow.mark_unreachable(UnreachableKind::ReturnStatement);

    if (!cur_f->declared_return_type) {
      return_statements.push_back(v->get_return_value());   // for future unification
    }
    return flow;
  }

  FlowContext process_if_statement(V<ast_if_statement> v, FlowContext&& flow) {
    ExprFlow after_cond = infer_any_expr(v->get_cond(), std::move(flow), true);
    v->get_cond()->mutate()->assign_always_true_or_false(after_cond.get_always_true_false_state());

    FlowContext true_flow = process_any_statement(v->get_if_body(), std::move(after_cond.true_flow));
    FlowContext false_flow = process_any_statement(v->get_else_body(), std::move(after_cond.false_flow));

    return FlowContext::merge_flow(std::move(true_flow), std::move(false_flow));
  }

  FlowContext process_repeat_statement(V<ast_repeat_statement> v, FlowContext&& flow) {
    ExprFlow after_cond = infer_any_expr(v->get_cond(), std::move(flow), false);

    return process_any_statement(v->get_body(), std::move(after_cond.out_flow));
  }

  FlowContext process_while_statement(V<ast_while_statement> v, FlowContext&& flow) {
    // loops are inferred twice, to merge body outcome with the state before the loop
    // (a more correct approach would be not "twice", but "find a fixed point when state stop changing")
    // also remember, we don't have a `break` statement, that's why when loop exits, condition became false
    FlowContext loop_entry_facts = flow.clone();
    ExprFlow after_cond = infer_any_expr(v->get_cond(), std::move(flow), true);
    FlowContext body_out = process_any_statement(v->get_body(), std::move(after_cond.true_flow));
    // second time, to refine all types
    flow = FlowContext::merge_flow(std::move(loop_entry_facts), std::move(body_out));
    ExprFlow after_cond2 = infer_any_expr(v->get_cond(), std::move(flow), true);
    v->get_cond()->mutate()->assign_always_true_or_false(after_cond2.get_always_true_false_state());

    process_any_statement(v->get_body(), std::move(after_cond2.true_flow));

    return std::move(after_cond2.false_flow);
  }

  FlowContext process_do_while_statement(V<ast_do_while_statement> v, FlowContext&& flow) {
    // do while is also handled twice; read comments above
    FlowContext loop_entry_facts = flow.clone();
    flow = process_any_statement(v->get_body(), std::move(flow));
    ExprFlow after_cond = infer_any_expr(v->get_cond(), std::move(flow), true);
    // second time
    flow = FlowContext::merge_flow(std::move(loop_entry_facts), std::move(after_cond.true_flow));
    flow = process_any_statement(v->get_body(), std::move(flow));
    ExprFlow after_cond2 = infer_any_expr(v->get_cond(), std::move(flow), true);
    v->get_cond()->mutate()->assign_always_true_or_false(after_cond2.get_always_true_false_state());

    return std::move(after_cond2.false_flow);
  }

  FlowContext process_throw_statement(V<ast_throw_statement> v, FlowContext&& flow) {
    flow = infer_any_expr(v->get_thrown_code(), std::move(flow), false).out_flow;
    flow = infer_any_expr(v->get_thrown_arg(), std::move(flow), false).out_flow;
    flow.mark_unreachable(UnreachableKind::ThrowStatement);
    return flow;
  }

  FlowContext process_assert_statement(V<ast_assert_statement> v, FlowContext&& flow) {
    ExprFlow after_cond = infer_any_expr(v->get_cond(), std::move(flow), true);
    v->get_cond()->mutate()->assign_always_true_or_false(after_cond.get_always_true_false_state());

    ExprFlow after_throw = infer_any_expr(v->get_thrown_code(), std::move(after_cond.false_flow), false);
    return std::move(after_cond.true_flow);
  }

  static FlowContext process_catch_variable(AnyExprV catch_var, TypePtr catch_var_type, FlowContext&& flow) {
    if (auto v_ref = catch_var->try_as<ast_reference>(); v_ref && v_ref->sym) { // not underscore
      LocalVarPtr var_ref = v_ref->sym->try_as<LocalVarPtr>();
      assign_inferred_type(var_ref, catch_var_type);
      flow.register_known_type(SinkExpression(var_ref), catch_var_type);
    }
    assign_inferred_type(catch_var, catch_var_type);
    return flow;
  }

  FlowContext process_try_catch_statement(V<ast_try_catch_statement> v, FlowContext&& flow) {
    FlowContext before_try = flow.clone();
    FlowContext try_end = process_any_statement(v->get_try_body(), std::move(flow));

    // `catch` has exactly 2 variables: excNo and arg (when missing, they are implicit underscores)
    // `arg` is a curious thing, it can be any TVM primitive, so assign unknown to it
    // hence, using `fInt(arg)` (int from parameter is a target type) or `arg as slice` works well
    // it's not truly correct, because `arg as (int,int)` also compiles, but can never happen, but let it be user responsibility
    FlowContext catch_flow = std::move(before_try);
    tolk_assert(v->get_catch_expr()->size() == 2);
    std::vector<TypePtr> types_list = {TypeDataInt::create(), TypeDataUnknown::create()};
    catch_flow = process_catch_variable(v->get_catch_expr()->get_item(0), types_list[0], std::move(catch_flow));
    catch_flow = process_catch_variable(v->get_catch_expr()->get_item(1), types_list[1], std::move(catch_flow));
    assign_inferred_type(v->get_catch_expr(), TypeDataTensor::create(std::move(types_list)));

    FlowContext catch_end = process_any_statement(v->get_catch_body(), std::move(catch_flow));
    return FlowContext::merge_flow(std::move(try_end), std::move(catch_end));
  }

  FlowContext process_expression_statement(AnyExprV v, FlowContext&& flow) {
    ExprFlow after_v = infer_any_expr(v, std::move(flow), false);
    return std::move(after_v.out_flow);
  }

public:
  static void assign_fun_full_type(FunctionPtr fun_ref, TypePtr inferred_return_type) {
    // calculate function full type `(params) -> ret_type`
    std::vector<TypePtr> params_types;
    params_types.reserve(fun_ref->get_num_params());
    for (const LocalVarData& param : fun_ref->parameters) {
      params_types.push_back(param.declared_type);
    }
    assign_inferred_type(fun_ref, inferred_return_type, TypeDataFunCallable::create(std::move(params_types), inferred_return_type));
  }

  void start_visiting_function(FunctionPtr fun_ref, V<ast_function_declaration> v_function) {
    TypePtr inferred_return_type = fun_ref->declared_return_type;
    if (fun_ref->is_code_function()) {
      FlowContext body_start;
      for (const LocalVarData& param : fun_ref->parameters) {
        body_start.register_known_type(SinkExpression(&param), param.declared_type);
      }

      cur_f = fun_ref;
      FlowContext body_end = process_any_statement(v_function->get_body(), std::move(body_start));
      cur_f = nullptr;

      if (!body_end.is_unreachable()) {
        fun_ref->mutate()->assign_is_implicit_return();
        if (fun_ref->declared_return_type == TypeDataNever::create()) {   // `never` can only be declared, it can't be inferred
          fire(fun_ref, v_function->get_body()->as<ast_sequence>()->loc_end, "a function returning `never` can not have a reachable endpoint");
        }
      }

      if (!fun_ref->declared_return_type) {
        TypeInferringUnifyStrategy return_unifier;
        if (fun_ref->does_return_self()) {
          return_unifier.unify_with(fun_ref->parameters[0].declared_type);
        }
        for (AnyExprV return_value : return_statements) {
          if (!return_unifier.unify_with(return_value->inferred_type)) {
            fire(cur_f, return_value->loc, "can not unify type " + to_string(return_value) + " with previous return type " + to_string(return_unifier.get_result()));
          }
        }
        if (!body_end.is_unreachable()) {
          if (!return_unifier.unify_with_implicit_return_void()) {
            fire(cur_f, v_function->get_body()->as<ast_sequence>()->loc_end, "missing return");
          }
        }
        inferred_return_type = return_unifier.get_result();
        if (inferred_return_type == nullptr && body_end.is_unreachable()) {
          inferred_return_type = TypeDataVoid::create();
        }
      }
    } else {
      // asm functions should be strictly typed, this was checked earlier
      tolk_assert(fun_ref->declared_return_type);
    }

    assign_fun_full_type(fun_ref, inferred_return_type);
    fun_ref->mutate()->assign_is_type_inferring_done();
  }

  // given `const a = 2 + 3` infer that it's int
  // for `const a: int = ...` still infer all sub expressions (to be checked in a later pipe)
  void start_visiting_constant(GlobalConstPtr const_ref) {
    FlowContext const_flow;
    infer_any_expr(const_ref->init_value, std::move(const_flow), false, const_ref->declared_type);
    const_ref->mutate()->assign_inferred_type(const_ref->declared_type == nullptr ? const_ref->init_value->inferred_type : const_ref->declared_type);
  }
};

class LaunchInferTypesAndMethodsOnce final {
public:
  static bool should_visit_function(FunctionPtr fun_ref) {
    // since inferring can be requested on demand, prevent second execution from a regular pipeline launcher
    return !fun_ref->is_type_inferring_done() && !fun_ref->is_generic_function();
  }

  static void start_visiting_function(FunctionPtr fun_ref, V<ast_function_declaration> v_function) {
    InferTypesAndCallsAndFieldsVisitor visitor;
    visitor.start_visiting_function(fun_ref, v_function);
  }
};

// infer return type "on demand"
// example: `fun f() { return g(); } fun g() { ... }`
// when analyzing `f()`, we need to infer what fun_ref=g returns
// (if `g` is generic, it was already instantiated, so fun_ref=g<int> is here)
static void infer_and_save_return_type_of_function(FunctionPtr fun_ref) {
  static std::vector<FunctionPtr> called_stack;

  tolk_assert(!fun_ref->is_generic_function() && !fun_ref->is_type_inferring_done());
  // if `g` has return type declared, like `fun g(): int { ... }`, don't traverse its body
  if (fun_ref->declared_return_type) {
    InferTypesAndCallsAndFieldsVisitor::assign_fun_full_type(fun_ref, fun_ref->declared_return_type);
    return;
  }

  // prevent recursion of untyped functions, like `fun f() { return g(); } fun g() { return f(); }`
  bool contains = std::find(called_stack.begin(), called_stack.end(), fun_ref) != called_stack.end();
  if (contains) {
    fire(fun_ref, fun_ref->loc, "could not infer return type of " + to_string(fun_ref) + ", because it appears in a recursive call chain; specify `: <return_type>` manually");
  }

  // dig into g's body; it's safe, since the compiler is single-threaded
  // on finish, fun_ref->inferred_return_type is filled, and won't be called anymore
  called_stack.push_back(fun_ref);
  InferTypesAndCallsAndFieldsVisitor visitor;
  visitor.start_visiting_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
  called_stack.pop_back();
}

// infer constant type "on demand"
// example: `const a = 1 + b;`
// when analyzing `a`, we need to infer what type const_ref=b has
static void infer_and_save_type_of_constant(GlobalConstPtr const_ref) {
  static std::vector<GlobalConstPtr> called_stack;

  // prevent recursion like `const a = b; const b = a`
  bool contains = std::find(called_stack.begin(), called_stack.end(), const_ref) != called_stack.end();
  if (contains) {
    throw ParseError(const_ref->loc, "const `" + const_ref->name + "` appears, directly or indirectly, in its own initializer");
  }

  called_stack.push_back(const_ref);
  InferTypesAndCallsAndFieldsVisitor visitor;
  visitor.start_visiting_constant(const_ref);
  called_stack.pop_back();
}

void pipeline_infer_types_and_calls_and_fields() {
  visit_ast_of_all_functions<LaunchInferTypesAndMethodsOnce>();

  // analyze constants that weren't referenced by any function
  InferTypesAndCallsAndFieldsVisitor visitor;
  for (GlobalConstPtr const_ref : get_all_declared_constants()) {
    if (!const_ref->inferred_type) {
      visitor.start_visiting_constant(const_ref);
    }
  }
  // (later, at constant folding, `const a = 2 + 3` will be evaluated to 5)
}

void pipeline_infer_types_and_calls_and_fields(FunctionPtr fun_ref) {
  InferTypesAndCallsAndFieldsVisitor visitor;
  visitor.start_visiting_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
}

} // namespace tolk
