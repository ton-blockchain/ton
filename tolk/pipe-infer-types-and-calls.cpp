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
#include <charconv>

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

GNU_ATTRIBUTE_NOINLINE
static std::string to_string(StructPtr struct_ref) {
  return "`" + struct_ref->as_human_readable() + "`";
}

GNU_ATTRIBUTE_NOINLINE
static std::string to_string(AliasDefPtr alias_ref) {
  return "`" + alias_ref->as_human_readable() + "`";
}

GNU_ATTRIBUTE_NOINLINE
static std::string to_string(std::string_view string_view) {
  return static_cast<std::string>(string_view);
}

// fire an error when `fun f<T>(...) asm ...` is called with T=(int,int) or other non-1 width on stack
// asm functions generally can't handle it, they expect T to be a TVM primitive
// (in FunC, `forall` type just couldn't be unified with non-primitives; in Tolk, generic T is expectedly inferred)
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_calling_asm_function_with_non1_stack_width_arg(FunctionPtr cur_f, SrcLocation loc, FunctionPtr fun_ref, const GenericsSubstitutions& substitutions, int arg_idx) {
  fire(cur_f, loc, "can not call `" + fun_ref->as_human_readable() + "` with " + to_string(substitutions.nameT_at(arg_idx)) + "=" + substitutions.typeT_at(arg_idx)->as_human_readable() + ", because it occupies " + std::to_string(substitutions.typeT_at(arg_idx)->get_width_on_stack()) + " stack slots in TVM, not 1");
}

// fire an error on `untypedTupleVar.0` when used without a hint
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_cannot_deduce_untyped_tuple_access(FunctionPtr cur_f, SrcLocation loc, int index) {
  std::string idx_access = "<tuple>." + std::to_string(index);
  fire(cur_f, loc, "can not deduce type of `" + idx_access + "`\neither assign it to variable like `var c: int = " + idx_access + "` or cast the result like `" + idx_access + " as int`");
}

// fire an error on using lateinit variable before definite assignment
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_using_lateinit_variable_uninitialized(FunctionPtr cur_f, SrcLocation loc, std::string_view name) {
  fire(cur_f, loc, "using variable `" + static_cast<std::string>(name) + "` before it's definitely assigned");
}

// fire an error when `obj.f()`, method `f` not found, try to locate a method for another type
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_method_not_found(FunctionPtr cur_f, SrcLocation loc, TypePtr receiver_type, std::string_view method_name) {
  if (std::vector<FunctionPtr> other = lookup_methods_with_name(method_name); !other.empty()) {
    fire(cur_f, loc, "method `" + to_string(method_name) + "` not found for type " + to_string(receiver_type) + "\n(but it exists for type " + to_string(other.front()->receiver_type) + ")");
  }
  if (const Symbol* sym = lookup_global_symbol(method_name); sym && sym->try_as<FunctionPtr>()) {
    fire(cur_f, loc, "method `" + to_string(method_name) + "` not found, but there is a global function named `" + to_string(method_name) + "`\n(a function should be called `foo(arg)`, not `arg.foo()`)");
  }
  fire(cur_f, loc, "method `" + to_string(method_name) + "` not found");
}

// safe version of std::stoi that does not crash on long numbers
static bool try_parse_string_to_int(std::string_view str, int& out) {
  auto result = std::from_chars(str.data(), str.data() + str.size(), out);
  return result.ec == std::errc() && result.ptr == str.data() + str.size();
}

// helper function: given hint = `Ok<int> | Err<slice>` and struct `Ok`, return `Ok<int>`
// example: `match (...) { Ok => ... }` we need to deduce `Ok<T>` based on subject
static TypePtr try_pick_instantiated_generic_from_hint(TypePtr hint, StructPtr lookup_ref) {
  // example: `var w: Ok<int> = Ok { ... }`, hint is `Ok<int>`, lookup is `Ok`
  if (const TypeDataStruct* h_struct = hint->unwrap_alias()->try_as<TypeDataStruct>()) {
    if (lookup_ref == h_struct->struct_ref->base_struct_ref) {
      return h_struct;
    }
  }
  // example: `fun f(): Response<int, slice> { return Err { ... } }`, hint is `Ok<int> | Err<slice>`, lookup is `Err`
  if (const TypeDataUnion* h_union = hint->unwrap_alias()->try_as<TypeDataUnion>()) {
    TypePtr only_variant = nullptr;   // hint `Ok<int8> | Ok<int16>` is ambiguous
    for (TypePtr h_variant : h_union->variants) {
      if (const TypeDataStruct* variant_struct = h_variant->unwrap_alias()->try_as<TypeDataStruct>()) {
        if (lookup_ref == variant_struct->struct_ref->base_struct_ref) {
          if (only_variant) {
            return nullptr;
          }
          only_variant = variant_struct;
        }
      }
    }
    return only_variant;
  }
  return nullptr;
}

// helper function, similar to the above, but for generic type aliases
// example: `v is OkAlias`, need to deduce `OkAlias<T>` based on type of v
static TypePtr try_pick_instantiated_generic_from_hint(TypePtr hint, AliasDefPtr lookup_ref) {
  // when a generic type alias points to a generic struct actually: `type WrapperAlias<T> = Wrapper<T>`
  if (const TypeDataGenericTypeWithTs* as_instT = lookup_ref->underlying_type->try_as<TypeDataGenericTypeWithTs>()) {
    return as_instT->struct_ref
         ? try_pick_instantiated_generic_from_hint(hint, as_instT->struct_ref)
         : try_pick_instantiated_generic_from_hint(hint, as_instT->alias_ref);
  }
  // it's something weird, when a generic alias refs non-generic type
  // example: `type StrangeInt<T> = int`, hint is `StrangeInt<builder>`, lookup `StrangeInt`
  if (const TypeDataAlias* h_alias = hint->try_as<TypeDataAlias>()) {
    if (lookup_ref == h_alias->alias_ref->base_alias_ref) {
      return h_alias;
    }
  }
  return nullptr;
}

// given `p.create` (called_receiver = Point, called_name = "create")
// look up a corresponding method (it may be `Point.create` / `Point?.create` / `T.create`)
static MethodCallCandidate choose_only_method_to_call(FunctionPtr cur_f, SrcLocation loc, TypePtr called_receiver, std::string_view called_name) {
  // most practical cases: `builder.storeInt` etc., when a direct method for receiver exists
  if (FunctionPtr exact_method = match_exact_method_for_call_not_generic(called_receiver, called_name)) {
    return {exact_method, GenericsSubstitutions(exact_method->genericTs)};
  }

  // else, try to match, for example `10.copy` with `int?.copy`, with `T.copy`, etc.
  std::vector<MethodCallCandidate> candidates = match_methods_for_call_including_generic(called_receiver, called_name);
  if (candidates.size() == 1) {
    return candidates[0];
  }
  if (candidates.empty()) {   // return nullptr, the caller side decides how to react on this
    return {nullptr, GenericsSubstitutions(nullptr)};
  }

  std::ostringstream msg;
  msg << "call to method `" << called_name << "` for type `" << called_receiver << "` is ambiguous\n";
  for (const auto& [method_ref, substitutedTs] : candidates) {
    msg << "candidate function: `" << method_ref->as_human_readable() << "`";
    if (method_ref->is_generic_function()) {
      msg << " with " << substitutedTs.as_human_readable(false);
    }
    if (method_ref->loc.is_defined()) {
      msg << " (declared at " << method_ref->loc << ")\n";
    } else if (method_ref->is_builtin_function()) {
      msg << " (builtin)\n";
    }
  }
  fire(cur_f, loc, msg.str());
}

// given fun `f` and a call `f(a,b,c)`, check that argument count is expected;
// (parameters may have default values, so it's not as trivial as to compare params and args size)
void check_arguments_count_at_fun_call(FunctionPtr cur_f, V<ast_function_call> v, FunctionPtr called_f, AnyExprV self_obj) {
  int delta_self = self_obj != nullptr;
  int n_arguments = v->get_num_args() + delta_self;
  int n_max_params = called_f->get_num_params();
  int n_min_params = n_max_params;
  while (n_min_params && called_f->get_param(n_min_params - 1).has_default_value()) {
    n_min_params--;
  }

  if (!called_f->does_accept_self() && self_obj) {   // static method `Point.create(...)` called as `p.create()`
    fire(cur_f, v->loc, "method " + to_string(called_f) + " can not be called via dot\n(it's a static method, it does not accept `self`)");
  }
  if (n_max_params < n_arguments) {
    fire(cur_f, v->loc, "too many arguments in call to " + to_string(called_f) + ", expected " + std::to_string(n_max_params - delta_self) + ", have " + std::to_string(n_arguments - delta_self));
  }
  if (n_arguments < n_min_params) {
    fire(cur_f, v->loc, "too few arguments in call to " + to_string(called_f) + ", expected " + std::to_string(n_min_params - delta_self) + ", have " + std::to_string(n_arguments - delta_self));
  }
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
    tolk_assert(src->inferred_type != nullptr && !src->inferred_type->has_genericT_inside());
#endif
    dst->mutate()->assign_inferred_type(src->inferred_type);
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE
  static void assign_inferred_type(AnyExprV dst, TypePtr inferred_type) {
#ifdef TOLK_DEBUG
    tolk_assert(inferred_type != nullptr && !inferred_type->has_genericT_inside());
#endif
    dst->mutate()->assign_inferred_type(inferred_type);
  }

  static void assign_inferred_type(LocalVarPtr local_var_or_param, TypePtr inferred_type) {
#ifdef TOLK_DEBUG
    tolk_assert(inferred_type != nullptr && !inferred_type->has_genericT_inside());
#endif
    local_var_or_param->mutate()->assign_inferred_type(inferred_type);
  }

  static void assign_inferred_type(FunctionPtr fun_ref, TypePtr inferred_return_type, TypePtr inferred_full_type) {
#ifdef TOLK_DEBUG
    tolk_assert(inferred_return_type != nullptr && !inferred_return_type->has_genericT_inside());
#endif
    fun_ref->mutate()->assign_inferred_type(inferred_return_type, inferred_full_type);
  }

  // traverse children in any statement
  FlowContext process_any_statement(AnyV v, FlowContext&& flow) {
    switch (v->kind) {
      case ast_block_statement:
        return process_block_statement(v->as<ast_block_statement>(), std::move(flow));
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
    switch (v->kind) {
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
      case ast_is_type_operator:
        return infer_is_type_operator(v->as<ast_is_type_operator>(), std::move(flow), used_as_condition);
      case ast_not_null_operator:
        return infer_not_null_operator(v->as<ast_not_null_operator>(), std::move(flow), used_as_condition);
      case ast_parenthesized_expression:
        return infer_parenthesized(v->as<ast_parenthesized_expression>(), std::move(flow), used_as_condition, hint);
      case ast_braced_expression:
        return infer_braced_expression(v->as<ast_braced_expression>(), std::move(flow), used_as_condition);
      case ast_reference:
        return infer_reference(v->as<ast_reference>(), std::move(flow), used_as_condition, hint);
      case ast_dot_access:
        return infer_dot_access(v->as<ast_dot_access>(), std::move(flow), used_as_condition, hint);
      case ast_function_call:
        return infer_function_call(v->as<ast_function_call>(), std::move(flow), used_as_condition, hint);
      case ast_tensor:
        return infer_tensor(v->as<ast_tensor>(), std::move(flow), used_as_condition, hint);
      case ast_bracket_tuple:
        return infer_typed_tuple(v->as<ast_bracket_tuple>(), std::move(flow), used_as_condition, hint);
      case ast_null_keyword:
        return infer_null_keyword(v->as<ast_null_keyword>(), std::move(flow), used_as_condition);
      case ast_match_expression:
        return infer_match_expression(v->as<ast_match_expression>(), std::move(flow), used_as_condition, hint);
      case ast_object_literal:
        return infer_object_literal(v->as<ast_object_literal>(), std::move(flow), used_as_condition, hint);
      case ast_underscore:
        return infer_underscore(v->as<ast_underscore>(), std::move(flow), used_as_condition, hint);
      case ast_empty_expression:
        return infer_empty_expression(v->as<ast_empty_expression>(), std::move(flow), used_as_condition);
      default:
        throw UnexpectedASTNodeKind(v, "infer_any_expr");
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
      assign_inferred_type(v, v->type_node ? v->type_node->resolved_type : TypeDataUnknown::create());
      flow.register_known_type(SinkExpression(v->var_ref), TypeDataUnknown::create());    // it's unknown before assigned
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

    } else if (auto lhs_tuple = lhs->try_as<ast_bracket_tuple>()) {
      std::vector<TypePtr> types_list;
      types_list.reserve(lhs_tuple->size());
      for (int i = 0; i < lhs_tuple->size(); ++i) {
        flow = infer_left_side_of_assignment(lhs_tuple->get_item(i), std::move(flow));
        types_list.push_back(lhs_tuple->get_item(i)->inferred_type);
      }
      assign_inferred_type(lhs, TypeDataBrackets::create(std::move(types_list)));

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
      TypePtr declared_type = lhs_var->marked_as_redef ? lhs_var->var_ref->declared_type : lhs_var->type_node ? lhs_var->type_node->resolved_type : nullptr;
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
      const TypeDataTensor* rhs_type_tensor = rhs_type->unwrap_alias()->try_as<TypeDataTensor>();
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
    if (auto lhs_tuple = lhs->try_as<ast_bracket_tuple>()) {
      const TypeDataBrackets* rhs_type_tuple = rhs_type->unwrap_alias()->try_as<TypeDataBrackets>();
      std::vector<TypePtr> types_list;
      types_list.reserve(lhs_tuple->size());
      for (int i = 0; i < lhs_tuple->size(); ++i) {
        TypePtr ith_rhs_type = rhs_type_tuple && i < rhs_type_tuple->size() ? rhs_type_tuple->items[i] : TypeDataUnknown::create();
        process_assignment_lhs_after_infer_rhs(lhs_tuple->get_item(i), ith_rhs_type, out_flow);
        types_list.push_back(lhs_tuple->get_item(i)->inferred_type);
      }
      assign_inferred_type(lhs, TypeDataBrackets::create(std::move(types_list)));
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

    FunctionPtr builtin_sym = lookup_function("_" + static_cast<std::string>(builtin_func) + "_");
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
        if (rhs->inferred_type->unwrap_alias() == TypeDataBool::create()) {
          builtin_func = "!b";  // "overloaded" for bool
        }
        assign_inferred_type(v, TypeDataBool::create());
        std::swap(after_rhs.false_flow, after_rhs.true_flow);
        break;
      default:
        tolk_assert(false);
    }

    FunctionPtr builtin_sym = lookup_function(static_cast<std::string>(builtin_func) + "_");
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
        if (lhs->inferred_type->unwrap_alias() == TypeDataBool::create() && rhs->inferred_type->unwrap_alias() == TypeDataBool::create()) {
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
      FunctionPtr builtin_sym = lookup_function("_" + static_cast<std::string>(builtin_func) + "_");
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

    TypeInferringUnifyStrategy branches_unifier;
    branches_unifier.unify_with(v->get_when_true()->inferred_type, hint);
    branches_unifier.unify_with(v->get_when_false()->inferred_type, hint);
    if (branches_unifier.is_union_of_different_types()) {
      // `... ? intVar : sliceVar` results in `int | slice`, probably it's not what the user expected
      // example: `var v = ternary`, show an inference error
      // do NOT show an error for `var v: T = ternary` (T is hint); it will be checked by type checker later
      if (hint == nullptr || hint == TypeDataUnknown::create() || hint->has_genericT_inside()) {
        fire(cur_f, v->loc, "types of ternary branches are incompatible: " + to_string(v->get_when_true()) + " and " + to_string(v->get_when_false()));
      }
    }
    assign_inferred_type(v, branches_unifier.get_result());

    FlowContext out_flow = FlowContext::merge_flow(std::move(after_true.out_flow), std::move(after_false.out_flow));
    return ExprFlow(std::move(out_flow), std::move(after_true.true_flow), std::move(after_false.false_flow));
  }

  ExprFlow infer_cast_as_operator(V<ast_cast_as_operator> v, FlowContext&& flow, bool used_as_condition) {
    // for `expr as <type>`, use this type for hint, so that `t.tupleAt(0) as int` is ok
    ExprFlow after_expr = infer_any_expr(v->get_expr(), std::move(flow), false, v->type_node->resolved_type);
    assign_inferred_type(v, v->type_node->resolved_type);

    if (!used_as_condition) {
      return after_expr;
    }
    return ExprFlow(std::move(after_expr.out_flow), true);
  }

  ExprFlow infer_is_type_operator(V<ast_is_type_operator> v, FlowContext&& flow, bool used_as_condition) {
    ExprFlow after_expr = infer_any_expr(v->get_expr(), std::move(flow), false);
    assign_inferred_type(v, TypeDataBool::create());

    TypePtr rhs_type = v->type_node->resolved_type;

    if (const auto* t_struct = rhs_type->try_as<TypeDataStruct>(); t_struct && t_struct->struct_ref->is_generic_struct()) {
      // `v is Wrapper`, detect T based on type of v (`Wrapper<int> | int` => `Wrapper<int>`)
      if (TypePtr inst_rhs_type = try_pick_instantiated_generic_from_hint(v->get_expr()->inferred_type, t_struct->struct_ref)) {
        rhs_type = inst_rhs_type;
        v->type_node->mutate()->assign_resolved_type(rhs_type);
      } else {
        fire(cur_f, v->type_node->loc, "can not deduce type arguments for " + to_string(t_struct->struct_ref) + ", provide them manually");
      }
    }
    if (const auto* t_alias = rhs_type->try_as<TypeDataAlias>(); t_alias && t_alias->alias_ref->is_generic_alias()) {
      // `v is WrapperAlias`, detect T similar to structures
      if (TypePtr inst_rhs_type = try_pick_instantiated_generic_from_hint(v->get_expr()->inferred_type, t_alias->alias_ref)) {
        rhs_type = inst_rhs_type;
        v->type_node->mutate()->assign_resolved_type(rhs_type);
      } else {
        fire(cur_f, v->type_node->loc, "can not deduce type arguments for " + to_string(t_alias->alias_ref) + ", provide them manually");
      }
    }

    rhs_type = rhs_type->unwrap_alias();
    TypePtr expr_type = v->get_expr()->inferred_type->unwrap_alias();
    TypePtr non_rhs_type = calculate_type_subtract_rhs_type(expr_type, rhs_type);
    if (expr_type->equal_to(rhs_type)) {                          // `expr is <type>` is always true
      v->mutate()->assign_always_true_or_false(v->is_negated ? 2 : 1);
    } else if (non_rhs_type == TypeDataNever::create()) {         // `expr is <type>` is always false
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
        true_flow.register_known_type(s_expr, rhs_type);
        false_flow.register_known_type(s_expr, non_rhs_type);
      } else {
        true_flow.register_known_type(s_expr, non_rhs_type);
        false_flow.register_known_type(s_expr, rhs_type);
      }
    }
    return ExprFlow(std::move(after_expr.out_flow), std::move(true_flow), std::move(false_flow));
  }

  ExprFlow infer_not_null_operator(V<ast_not_null_operator> v, FlowContext&& flow, bool used_as_condition) {
    ExprFlow after_expr = infer_any_expr(v->get_expr(), std::move(flow), false);
    TypePtr expr_type = v->get_expr()->inferred_type;
    TypePtr without_null_type = calculate_type_subtract_rhs_type(expr_type->unwrap_alias(), TypeDataNullLiteral::create());
    assign_inferred_type(v, without_null_type != TypeDataNever::create() ? without_null_type : expr_type);

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

  ExprFlow infer_braced_expression(V<ast_braced_expression> v, FlowContext&& flow, bool used_as_condition) {
    // `{ ... }` used as an expression can not return a value currently (there is no syntax in a language)
    flow = process_any_statement(v->get_block_statement(), std::move(flow));
    assign_inferred_type(v, flow.is_unreachable() ? TypeDataNever::create() : TypeDataVoid::create());
    return ExprFlow(std::move(flow), used_as_condition);
  }

  // given `genericF<int, slice>` / `t.tupleFirst<cell>` (the user manually specified instantiation Ts),
  // validate and collect them
  // returns: [int, slice] / [cell]
  std::vector<TypePtr> collect_type_arguments_for_fun(SrcLocation loc, const GenericsDeclaration* genericTs, V<ast_instantiationT_list> instantiationT_list) const {
    // for `genericF<T1,T2>` user should provide two Ts
    // for `Container<T>.wrap<U>` — one U (and one T is implicitly from receiver)
    if (instantiationT_list->size() != genericTs->size() - genericTs->n_from_receiver) {
      fire(cur_f, loc, "expected " + std::to_string(genericTs->size() - genericTs->n_from_receiver) + " type arguments, got " + std::to_string(instantiationT_list->size()));
    }

    std::vector<TypePtr> type_arguments;
    type_arguments.reserve(instantiationT_list->size());
    for (int i = 0; i < instantiationT_list->size(); ++i) {
      type_arguments.push_back(instantiationT_list->get_item(i)->type_node->resolved_type);
    }

    return type_arguments;
  }

  // when substitutedTs have been collected from `f<int>` or deduced from arguments, instantiate a generic function `f`
  // example: was `t.push(2)`, deduced <int>, instantiate `tuple.push<int>`
  // example: was `t.push<slice>(2)`, collected <slice>, instantiate `tuple.push<slice>` (will later fail type check)
  // example: was `var cb = t.first<int>;` (used as reference, as non-call), instantiate `tuple.first<int>`
  // returns fun_ref to instantiated function
  FunctionPtr check_and_instantiate_generic_function(SrcLocation loc, FunctionPtr fun_ref, GenericsSubstitutions&& substitutedTs) const {
    // T for asm function must be a TVM primitive (width 1), otherwise, asm would act incorrectly
    if (fun_ref->is_asm_function() || fun_ref->is_builtin_function()) {
      for (int i = 0; i < substitutedTs.size(); ++i) {
        if (substitutedTs.typeT_at(i)->get_width_on_stack() != 1 && !fun_ref->is_variadic_width_T_allowed()) {
          fire_error_calling_asm_function_with_non1_stack_width_arg(cur_f, loc, fun_ref, substitutedTs, i);
        }
      }
    }

    // make deep clone of `f<T>` with substitutedTs (or immediately return from symtable if already instantiated)
    return instantiate_generic_function(fun_ref, std::move(substitutedTs));
  }

  ExprFlow infer_reference(V<ast_reference> v, FlowContext&& flow, bool used_as_condition, TypePtr hint, FunctionPtr* out_f_called = nullptr) const {
    // at current point, v is a reference:
    // - either a standalone: `local_var` / `SOME_CONST` / `globalF` / `genericFn<int>`
    // - or inside a call: `globalF()` / `genericFn()` / `genericFn<int>()` / `local_var()`

    if (LocalVarPtr var_ref = v->sym->try_as<LocalVarPtr>()) {
      TypePtr declared_or_smart_casted = flow.smart_cast_if_exists(SinkExpression(var_ref));
      tolk_assert(declared_or_smart_casted != nullptr);   // all local vars are presented in flow
      assign_inferred_type(v, declared_or_smart_casted);
      if (var_ref->is_lateinit() && declared_or_smart_casted == TypeDataUnknown::create() && v->is_rvalue) {
        fire_error_using_lateinit_variable_uninitialized(cur_f, v->loc, v->get_name());
      }
      // it might be `local_var()` also, don't fill out_f_called, we have no fun_ref, it's a call of arbitrary expression

    } else if (GlobalConstPtr const_ref = v->sym->try_as<GlobalConstPtr>()) {
      if (!const_ref->inferred_type) {
        infer_and_save_type_of_constant(const_ref);
      }
      assign_inferred_type(v, const_ref->inferred_type);

    } else if (GlobalVarPtr glob_ref = v->sym->try_as<GlobalVarPtr>()) {
      // there are no smart casts for globals, it's a way of preventing reading one global multiple times, it costs gas
      assign_inferred_type(v, glob_ref->declared_type);

    } else if (FunctionPtr fun_ref = v->sym->try_as<FunctionPtr>()) {
      // it's `globalF` / `globalF<int>` / `globalF()` / `globalF<int>()`
      // if it's a call, then out_f_called is present, we should fill it
      V<ast_instantiationT_list> v_instantiationTs = v->get_instantiationTs();

      if (fun_ref->is_generic_function() && !v_instantiationTs && !out_f_called) {
        // `genericFn` is invalid as non-call, can't be used without <instantiation>
        fire(cur_f, v->loc, "can not use a generic function " + to_string(fun_ref) + " as non-call");

      } else if (fun_ref->is_generic_function() && v_instantiationTs) {
        // `genericFn<int>` is valid as non-call, it's a reference to instantiation
        // `genericFn<int>()` is also ok, we'll assign an instantiated fun_ref to out_f_called
        GenericsSubstitutions substitutedTs(fun_ref->genericTs);
        substitutedTs.provide_type_arguments(collect_type_arguments_for_fun(v->loc, fun_ref->genericTs, v_instantiationTs));
        fun_ref = check_and_instantiate_generic_function(v->loc, fun_ref, std::move(substitutedTs));
        v->mutate()->assign_sym(fun_ref);

      } else if (UNLIKELY(v_instantiationTs != nullptr)) {
        // non-generic function referenced like `return beginCell<builder>;`
        fire(cur_f, v_instantiationTs->loc, "type arguments not expected here");
      }

      if (out_f_called) {           // so, it's `globalF()` / `genericFn()` / `genericFn<int>()`
        *out_f_called = fun_ref;    // (it's still may be a generic one, then Ts will be deduced from arguments)
      } else {                      // so, it's `globalF` / `genericFn<int>` as a reference
        if (fun_ref->is_compile_time_const_val() || fun_ref->is_compile_time_special_gen()) {
          fire(cur_f, v->loc, "can not get reference to this function, it's compile-time only");
        }
        fun_ref->mutate()->assign_is_used_as_noncall();
        get_or_infer_return_type(fun_ref);
        assign_inferred_type(v, fun_ref->inferred_full_type);
      }
      return ExprFlow(std::move(flow), used_as_condition);

    } else {
      tolk_assert(false);
    }

    // for non-functions: `local_var<int>` and similar not allowed
    if (UNLIKELY(v->has_instantiationTs())) {
      fire(cur_f, v->get_instantiationTs()->loc, "type arguments not expected here");
    }
    return ExprFlow(std::move(flow), used_as_condition);
  }

  ExprFlow infer_dot_access(V<ast_dot_access> v, FlowContext&& flow, bool used_as_condition, TypePtr hint, FunctionPtr* out_f_called = nullptr, AnyExprV* out_dot_obj = nullptr) {
    // at current point, v is a dot access to a field / index / method:
    // - either a standalone: `user.id` / `getUser().id` / `var.0` / `t.size` / `Point.create` / `t.tupleAt<slice>`
    // - or inside a call: `user.getId()` / `<any_expr>.method()` / `Point.create()` / `t.tupleAt<slice>(1)`

    AnyExprV dot_obj = nullptr;     // to be filled for `<dot_obj>.(field/index/method)`, nullptr for `Point.create`
    FunctionPtr fun_ref = nullptr;  // to be filled for `<any_expr>.method` / `Point.create` (both standalone or in a call)
    GenericsSubstitutions substitutedTs(nullptr);
    V<ast_identifier> v_ident = v->get_identifier();    // field/method name vertex

    // handle `Point.create` / `Container<int>.wrap`: no dot_obj expression actually, lhs is a type, looking up a method
    if (auto obj_ref = v->get_obj()->try_as<ast_reference>()) {
      if (const auto* obj_as_type = obj_ref->sym->try_as<const TypeReferenceUsedAsSymbol*>()) {
        TypePtr receiver_type = obj_as_type->resolved_type;
        std::tie(fun_ref, substitutedTs) = choose_only_method_to_call(cur_f, v_ident->loc, receiver_type, v->get_field_name());
        if (!fun_ref) {
          fire(cur_f, v_ident->loc, "method `" + to_string(v->get_field_name()) + "` not found for type " + to_string(receiver_type));
        }
      }
    }
    // handle other (most, actually) cases: `<any_expr>.field` / `<any_expr>.method`
    if (!fun_ref) {
      dot_obj = v->get_obj();
      flow = infer_any_expr(dot_obj, std::move(flow), false).out_flow;
    }

    // our goal is to fill v->target (field/index/method) knowing type of obj
    TypePtr obj_type = dot_obj ? dot_obj->inferred_type->unwrap_alias() : TypeDataUnknown::create();
    V<ast_instantiationT_list> v_instantiationTs = v->get_instantiationTs();
    std::string_view field_name = v_ident->name;

    // check for field access (`user.id`), when obj is a struct
    if (const TypeDataStruct* obj_struct = obj_type->try_as<TypeDataStruct>()) {
      if (StructFieldPtr field_ref = obj_struct->struct_ref->find_field(field_name)) {
        v->mutate()->assign_target(field_ref);
        TypePtr inferred_type = field_ref->declared_type;
        if (SinkExpression s_expr = extract_sink_expression_from_vertex(v)) {
          if (TypePtr smart_casted = flow.smart_cast_if_exists(s_expr)) {
            inferred_type = smart_casted;
          }
        }
        assign_inferred_type(v, inferred_type);
        return ExprFlow(std::move(flow), used_as_condition);
      }
      // if field_name doesn't exist, don't fire an error now — maybe, it's `user.method()`
    }

    // check for indexed access (`tensorVar.0` / `tupleVar.1`)
    if (!fun_ref && field_name[0] >= '0' && field_name[0] <= '9') {
      int index_at;
      if (!try_parse_string_to_int(field_name, index_at)) {
        fire(cur_f, v_ident->loc, "invalid numeric index");
      }
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
      if (const auto* t_tuple = obj_type->try_as<TypeDataBrackets>()) {
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
          if (hint == nullptr || hint == TypeDataUnknown::create() || hint->has_genericT_inside()) {
            fire_error_cannot_deduce_untyped_tuple_access(cur_f, v->loc, index_at);
          }
          item_type = hint;
        }
        v->mutate()->assign_target(index_at);
        assign_inferred_type(v, item_type);
        return ExprFlow(std::move(flow), used_as_condition);
      }
    }

    // check for method (`t.size` / `user.getId`); even `i.0()` can be here if `fun int.0(self)` exists
    // for `T.copy` / `Container<T>.create`, substitution for T is also returned
    if (!fun_ref) {
      std::tie(fun_ref, substitutedTs) = choose_only_method_to_call(cur_f, dot_obj->loc, obj_type, field_name);
    }

    // not a field, not a method — fire an error
    if (!fun_ref) {
      // as a special case, handle accessing fields of nullable objects, to show a more precise error
      if (const TypeDataUnion* as_union = obj_type->try_as<TypeDataUnion>(); as_union && as_union->or_null) {
        if (const TypeDataStruct* n_struct = as_union->or_null->try_as<TypeDataStruct>(); n_struct && n_struct->struct_ref->find_field(field_name)) {
          fire(cur_f, v_ident->loc, "can not access field `" + static_cast<std::string>(field_name) + "` of a possibly nullable object " + to_string(dot_obj) + "\ncheck it via `obj != null` or use non-null assertion `obj!` operator");
        }
      }
      if (out_f_called) {
        fire_error_method_not_found(cur_f, v_ident->loc, dot_obj->inferred_type, v->get_field_name());
      } else {
        fire(cur_f, v_ident->loc, "field `" + static_cast<std::string>(field_name) + "` doesn't exist in type " + to_string(dot_obj));
      }
    }

    // if `fun T.copy(self)` and reference `int.copy` — all generic parameters are determined by the receiver, we know it
    if (fun_ref->is_generic_function() && fun_ref->genericTs->size() == fun_ref->genericTs->n_from_receiver) {
      tolk_assert(substitutedTs.typeT_at(0) != nullptr);
      fun_ref = check_and_instantiate_generic_function(v->loc, fun_ref, std::move(substitutedTs));
    }

    if (fun_ref->is_generic_function() && !v_instantiationTs && !out_f_called) {
      // `t.tupleAt` is invalid as non-call, can't be used without <instantiation>
      fire(cur_f, v->loc, "can not use a generic function " + to_string(fun_ref) + " as non-call");

    } else if (fun_ref->is_generic_function() && v_instantiationTs) {
      // `t.tupleAt<int>` is valid as non-call, it's a reference to instantiation
      // `t.tupleAt<int>()` is also ok, we'll assign an instantiated fun_ref to out_f_called
      substitutedTs.provide_type_arguments(collect_type_arguments_for_fun(v->loc, fun_ref->genericTs, v_instantiationTs));
      fun_ref = check_and_instantiate_generic_function(v->loc, fun_ref, std::move(substitutedTs));

    } else if (UNLIKELY(v_instantiationTs != nullptr)) {
      // non-generic method referenced like `var cb = c.hash<int>;`
      fire(cur_f, v_instantiationTs->loc, "type arguments not expected here");
    }

    if (out_f_called) {           // so, it's `user.method()` / `t.tupleAt()` / `t.tupleAt<int>()` / `Point.create`
      *out_f_called = fun_ref;    // (it's still may be a generic one, then Ts will be deduced from arguments)
      *out_dot_obj = dot_obj;
    } else {                      // so, it's `user.method` / `t.tupleAt<int>` as a reference
      if (fun_ref->is_compile_time_const_val() || fun_ref->is_compile_time_special_gen()) {
        fire(cur_f, v->get_identifier()->loc, "can not get reference to this method, it's compile-time only");
      }
      fun_ref->mutate()->assign_is_used_as_noncall();
      v->mutate()->assign_target(fun_ref);
      get_or_infer_return_type(fun_ref);
      assign_inferred_type(v, fun_ref->inferred_full_type);
    }
    return ExprFlow(std::move(flow), used_as_condition);
  }

  ExprFlow infer_function_call(V<ast_function_call> v, FlowContext&& flow, bool used_as_condition, TypePtr hint) {
    AnyExprV callee = v->get_callee();

    // v is `globalF(args)` / `globalF<int>(args)` / `obj.method(args)` / `local_var(args)` / `getF()(args)`
    AnyExprV self_obj = nullptr;      // for `obj.method()`, obj will be here (but for `Point.create()`, no obj exists)
    FunctionPtr fun_ref = nullptr;

    if (callee->kind == ast_reference) {
      flow = infer_reference(callee->as<ast_reference>(), std::move(flow), false, nullptr, &fun_ref).out_flow;
    } else if (callee->kind == ast_dot_access) {
      flow = infer_dot_access(callee->as<ast_dot_access>(), std::move(flow), false, nullptr, &fun_ref, &self_obj).out_flow;
    } else {
      flow = infer_any_expr(callee, std::move(flow), false).out_flow;
    }

    // handle `local_var()` / `getF()()` / `5()` / `SOME_CONST()` / `obj.method()()()` / `tensorVar.0()`
    if (!fun_ref) {
      // callee must have "callable" inferred type
      const TypeDataFunCallable* f_callable = callee->inferred_type->unwrap_alias()->try_as<TypeDataFunCallable>();
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
      assign_inferred_type(v, f_callable->return_type);
      return ExprFlow(std::move(flow), used_as_condition);
    }

    // so, we have a call `f(args)` or `obj.f(args)`, f is fun_ref (function / method) (code / asm / builtin)
    // we're going to iterate over passed arguments, and (if generic) infer substitutedTs
    // at first, check argument count
    int delta_self = self_obj != nullptr;
    check_arguments_count_at_fun_call(cur_f, v, fun_ref, self_obj);

    // for every passed argument, we need to infer its type
    // for generic functions, we need to infer type arguments (substitutedTs) on the fly
    // (if they are specified by a user like `f<int>(args)` / `t.tupleAt<slice>()`, fun_ref is already instantiated)
    GenericSubstitutionsDeducing deducingTs(fun_ref);

    // for `obj.method()` obj is the first argument (passed to `self` parameter)
    if (self_obj) {
      const LocalVarData& param_0 = fun_ref->parameters[0];
      TypePtr param_type = param_0.declared_type;
      if (param_type->has_genericT_inside()) {
        param_type = deducingTs.auto_deduce_from_argument(cur_f, self_obj->loc, param_type, self_obj->inferred_type);
      }
      if (param_0.is_mutate_parameter() && self_obj->inferred_type->unwrap_alias() != param_type->unwrap_alias()) {
        if (SinkExpression s_expr = extract_sink_expression_from_vertex(self_obj)) {
          assign_inferred_type(self_obj, calc_declared_type_before_smart_cast(self_obj));
          flow.register_known_type(s_expr, param_type);
        }
      }
    }
    // for static generic method call `Container<int>.create` of fun_ref = `Container<T>.create`, gather T=int
    if (!self_obj && fun_ref->receiver_type && fun_ref->receiver_type->has_genericT_inside() && callee->kind == ast_dot_access && callee->as<ast_dot_access>()->get_obj()->kind == ast_reference) {
      const auto* t_static = callee->as<ast_dot_access>()->get_obj()->as<ast_reference>()->sym->try_as<const TypeReferenceUsedAsSymbol*>();
      tolk_assert(t_static);
      deducingTs.auto_deduce_from_argument(cur_f, v->loc, fun_ref->receiver_type, t_static->resolved_type);
    }

    // loop over every argument, one by one, like control flow goes
    for (int i = 0; i < v->get_num_args(); ++i) {
      const LocalVarData& param_i = fun_ref->parameters[delta_self + i];
      AnyExprV arg_i = v->get_arg(i)->get_expr();
      TypePtr param_type = param_i.declared_type;
      if (param_type->has_genericT_inside()) {    // `fun f<T>(a:T, b:T)` T was fixated on `a`, use it as hint for `b`
        param_type = deducingTs.replace_Ts_with_currently_deduced(param_type);
      }
      flow = infer_any_expr(arg_i, std::move(flow), false, param_type).out_flow;
      if (param_type->has_genericT_inside()) {    // `f(a)` where f is generic: use `a` to infer param type
        param_type = deducingTs.auto_deduce_from_argument(cur_f, arg_i->loc, param_type, arg_i->inferred_type);
      }
      assign_inferred_type(v->get_arg(i), arg_i);  // arg itself is an expression
      if (param_i.is_mutate_parameter() && arg_i->inferred_type->unwrap_alias() != param_type->unwrap_alias()) {
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
      std::string_view nameT_unknown = deducingTs.get_first_not_deduced_nameT();
      if (!nameT_unknown.empty() && hint && !hint->has_genericT_inside() && fun_ref->declared_return_type) {
        // example: `t.tupleFirst()`, T doesn't depend on arguments, but is determined by return type
        // if used like `var x: int = t.tupleFirst()` / `t.tupleFirst() as int` / etc., use hint
        deducingTs.auto_deduce_from_argument(cur_f, v->loc, fun_ref->declared_return_type, hint);
        nameT_unknown = deducingTs.get_first_not_deduced_nameT();
      }
      if (!nameT_unknown.empty()) {
        deducingTs.apply_defaults_from_declaration();
        nameT_unknown = deducingTs.get_first_not_deduced_nameT();
      }
      if (!nameT_unknown.empty()) {
        deducingTs.fire_error_can_not_deduce(cur_f, v->get_arg_list()->loc, nameT_unknown);
      }
      fun_ref = check_and_instantiate_generic_function(v->loc, fun_ref, deducingTs.flush());
    }

    // get return type either from user-specified declaration or infer here on demand traversing its body
    v->mutate()->assign_fun_ref(fun_ref, self_obj != nullptr);
    get_or_infer_return_type(fun_ref);
    TypePtr inferred_type = fun_ref->does_return_self() ? self_obj->inferred_type : fun_ref->inferred_return_type;
    assign_inferred_type(v, inferred_type);
    assign_inferred_type(callee, fun_ref->inferred_full_type);
    if (inferred_type == TypeDataNever::create()) {
      flow.mark_unreachable(UnreachableKind::CallNeverReturnFunction);
    }
    return ExprFlow(std::move(flow), used_as_condition);
  }

  ExprFlow infer_tensor(V<ast_tensor> v, FlowContext&& flow, bool used_as_condition, TypePtr hint) {
    const TypeDataTensor* tensor_hint = hint ? hint->unwrap_alias()->try_as<TypeDataTensor>() : nullptr;
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

  ExprFlow infer_typed_tuple(V<ast_bracket_tuple> v, FlowContext&& flow, bool used_as_condition, TypePtr hint) {
    const TypeDataBrackets* tuple_hint = hint ? hint->unwrap_alias()->try_as<TypeDataBrackets>() : nullptr;
    std::vector<TypePtr> types_list;
    types_list.reserve(v->get_items().size());
    for (int i = 0; i < v->size(); ++i) {
      AnyExprV item = v->get_item(i);
      flow = infer_any_expr(item, std::move(flow), false, tuple_hint && i < tuple_hint->size() ? tuple_hint->items[i] : nullptr).out_flow;
      types_list.emplace_back(item->inferred_type);
    }
    assign_inferred_type(v, TypeDataBrackets::create(std::move(types_list)));
    return ExprFlow(std::move(flow), used_as_condition);
  }

  static ExprFlow infer_null_keyword(V<ast_null_keyword> v, FlowContext&& flow, bool used_as_condition) {
    assign_inferred_type(v, TypeDataNullLiteral::create());

    return ExprFlow(std::move(flow), used_as_condition);
  }

  ExprFlow infer_match_expression(V<ast_match_expression> v, FlowContext&& flow, bool used_as_condition, TypePtr hint) {
    flow = infer_any_expr(v->get_subject(), std::move(flow), false).out_flow;
    SinkExpression s_expr = extract_sink_expression_from_vertex(v->get_subject());
    TypeInferringUnifyStrategy branches_unifier;
    FlowContext arms_entry_facts = flow.clone();
    FlowContext match_out_flow;
    bool has_expr_arm = false;
    bool has_else_arm = false;

    for (int i = 0; i < v->get_arms_count(); ++i) {
      auto v_arm = v->get_arm(i);
      FlowContext arm_flow = infer_any_expr(v_arm->get_pattern_expr(), arms_entry_facts.clone(), false, nullptr).out_flow;
      if (s_expr && v_arm->pattern_kind == MatchArmKind::exact_type) {
        TypePtr exact_type = v_arm->pattern_type_node->resolved_type;
        if (const auto* t_struct = exact_type->try_as<TypeDataStruct>(); t_struct && t_struct->struct_ref->is_generic_struct()) {
          // `Wrapper => ...`, detect T based on type of subject (`Wrapper<int> | int` => `Wrapper<int>`)
          if (TypePtr inst_exact_type = try_pick_instantiated_generic_from_hint(v->get_subject()->inferred_type, t_struct->struct_ref)) {
            exact_type = inst_exact_type;
            v_arm->pattern_type_node->mutate()->assign_resolved_type(exact_type);
          } else {
            fire(cur_f, v_arm->loc, "can not deduce type arguments for " + to_string(t_struct->struct_ref) + ", provide them manually");
          }
        }
        if (const auto* t_alias = exact_type->try_as<TypeDataAlias>(); t_alias && t_alias->alias_ref->is_generic_alias()) {
          // `WrapperAlias => ...`, detect T similar to structures
          if (TypePtr inst_exact_type = try_pick_instantiated_generic_from_hint(v->get_subject()->inferred_type, t_alias->alias_ref)) {
            exact_type = inst_exact_type;
            v_arm->pattern_type_node->mutate()->assign_resolved_type(exact_type);
          } else {
            fire(cur_f, v_arm->loc, "can not deduce type arguments for " + to_string(t_alias->alias_ref) + ", provide them manually");
          }
        }
        arm_flow.register_known_type(s_expr, exact_type);

      } else if (v_arm->pattern_kind == MatchArmKind::const_expression) {
        has_expr_arm = true;
      } else if (v_arm->pattern_kind == MatchArmKind::else_branch) {
        has_else_arm = true;
      }

      arm_flow = infer_any_expr(v_arm->get_body(), std::move(arm_flow), false, hint).out_flow;
      match_out_flow = i == 0 ? std::move(arm_flow) : FlowContext::merge_flow(std::move(match_out_flow), std::move(arm_flow));
      branches_unifier.unify_with(v_arm->get_body()->inferred_type, hint);
    }
    if (v->get_arms_count() == 0) {
      match_out_flow = std::move(arms_entry_facts);
    }
    if (has_expr_arm && !has_else_arm) {
      FlowContext else_flow = process_any_statement(createV<ast_empty_expression>(v->loc), std::move(arms_entry_facts));
      match_out_flow = FlowContext::merge_flow(std::move(match_out_flow), std::move(else_flow));
    }

    if (v->is_statement()) {
      assign_inferred_type(v, TypeDataVoid::create());
    } else {
      if (v->get_arms_count() == 0) {     // still allow empty `match` statements, for probable codegen
        fire(cur_f, v->loc, "empty `match` can't be used as expression");
      }
      if (branches_unifier.is_union_of_different_types()) {
        // same as in ternary: `match (...) { t1 => someSlice, t2 => someInt }` is `int|slice`, probably unexpected
        if (hint == nullptr || hint == TypeDataUnknown::create() || hint->has_genericT_inside()) {
          fire(cur_f, v->loc, "type of `match` was inferred as " + to_string(branches_unifier.get_result()) + "; probably, it's not what you expected\nassign it to a variable `var v: <type> = match (...) { ... }` manually");
        }
      }
      assign_inferred_type(v, branches_unifier.get_result());
    }

    return ExprFlow(std::move(match_out_flow), used_as_condition);
  }

  ExprFlow infer_object_literal(V<ast_object_literal> v, FlowContext&& flow, bool used_as_condition, TypePtr hint) {
    // the goal is to detect struct_ref
    // either by lhs hint `var u: User = { ... }, or by explicitly provided ref `User { ... }`
    StructPtr struct_ref = nullptr;

    // `User { ... }` / `UserAlias { ... }` / `Wrapper { ... }` / `Wrapper<int> { ... }`
    if (v->type_node) {
      TypePtr provided_type = v->type_node->resolved_type->unwrap_alias();
      if (const TypeDataStruct* hint_struct = provided_type->try_as<TypeDataStruct>()) {
        struct_ref = hint_struct->struct_ref;     // `Wrapper` / `Wrapper<int>`
      } else if (const TypeDataGenericTypeWithTs* hint_instTs = provided_type->try_as<TypeDataGenericTypeWithTs>()) {
        struct_ref = hint_instTs->struct_ref;     // if `type WAlias<T> = Wrapper<T>`, here `Wrapper` (generic struct)
      }
      if (!struct_ref) {
        fire(cur_f, v->type_node->loc, to_string(v->type_node->resolved_type) + " does not name a struct");
      }
      // example: `var v: Ok<int> = Ok { ... }`, now struct_ref is "Ok<T>", take "Ok<int>" from hint
      if (struct_ref->is_generic_struct() && hint) {
        if (TypePtr inst_explicit_ref = try_pick_instantiated_generic_from_hint(hint, struct_ref)) {
          struct_ref = inst_explicit_ref->try_as<TypeDataStruct>()->struct_ref;
        }
      }
    }
    if (!struct_ref && hint) {
      if (const TypeDataStruct* hint_struct = hint->unwrap_alias()->try_as<TypeDataStruct>()) {
        struct_ref = hint_struct->struct_ref;
      }
      if (const TypeDataUnion* hint_union = hint->unwrap_alias()->try_as<TypeDataUnion>()) {
        StructPtr last_struct = nullptr;
        int n_structs = 0;
        for (TypePtr variant : hint_union->variants) {
          if (const TypeDataStruct* variant_struct = variant->unwrap_alias()->try_as<TypeDataStruct>()) {
            last_struct = variant_struct->struct_ref;
            n_structs++;
          }
          if (const TypeDataGenericTypeWithTs* hint_withTs = variant->unwrap_alias()->try_as<TypeDataGenericTypeWithTs>()) {
            last_struct = hint_withTs->struct_ref;
            n_structs++;
          }
        }
        if (n_structs == 1) {
          struct_ref = last_struct;
        }
      }
      if (const TypeDataGenericTypeWithTs* hint_withTs = hint->unwrap_alias()->try_as<TypeDataGenericTypeWithTs>()) {
        struct_ref = hint_withTs->struct_ref;
      }
    }
    if (!struct_ref) {
      fire(cur_f, v->loc, "can not detect struct name\nuse either `var v: StructName = { ... }` or `var v = StructName { ... }`");
    }

    // so, we have struct_ref, so we can check field names and infer values
    // if it's a generic struct, we need to deduce Ts by field values, like for a function call
    GenericSubstitutionsDeducing deducingTs(struct_ref);

    uint64_t occurred_mask = 0;
    for (int i = 0; i < v->get_body()->get_num_fields(); ++i) {
      auto field_i = v->get_body()->get_field(i);
      std::string_view field_name = field_i->get_field_name();
      StructFieldPtr field_ref = struct_ref->find_field(field_name);
      if (!field_ref) {
        fire(cur_f, field_i->loc, "field `" + to_string(field_name) + "` not found in struct " + to_string(struct_ref));
      }
      field_i->mutate()->assign_field_ref(field_ref);

      if (occurred_mask & (1ULL << field_ref->field_idx)) {
        fire(cur_f, field_i->loc, "duplicate field initialization");
      }
      occurred_mask |= 1ULL << field_ref->field_idx;

      AnyExprV val_i = field_i->get_init_val();
      TypePtr field_type = field_ref->declared_type;
      if (field_type->has_genericT_inside()) {
        field_type = deducingTs.replace_Ts_with_currently_deduced(field_type);
      }
      flow = infer_any_expr(val_i, std::move(flow), false, field_type).out_flow;
      if (field_type->has_genericT_inside()) {
        deducingTs.auto_deduce_from_argument(cur_f, field_i->loc, field_type, val_i->inferred_type);
      }
      assign_inferred_type(field_i, val_i);
    }

    // if it's a generic struct `Wrapper<T>`, we need to instantiate it, like `Wrapper<int>`
    if (struct_ref->is_generic_struct()) {
      std::string_view nameT_unknown = deducingTs.get_first_not_deduced_nameT();
      if (!nameT_unknown.empty()) {
        deducingTs.apply_defaults_from_declaration();
        nameT_unknown = deducingTs.get_first_not_deduced_nameT();
      }
      if (!nameT_unknown.empty()) {
        deducingTs.fire_error_can_not_deduce(cur_f, v->loc, nameT_unknown);
      }
      struct_ref = instantiate_generic_struct(struct_ref, deducingTs.flush());
      // re-assign field_ref (it was earlier assigned into a field of a generic struct)
      for (int i = 0; i < v->get_body()->get_num_fields(); ++i) {
        auto field_i = v->get_body()->get_field(i);
        field_i->mutate()->assign_field_ref(struct_ref->find_field(field_i->get_field_name()));
      }
    }

    // check that all fields are present (do it after potential generic instantiation, when types are known)
    for (StructFieldPtr field_ref : struct_ref->fields) {
      if (!(occurred_mask & (1ULL << field_ref->field_idx))) {
        bool allow_missing = field_ref->has_default_value() || field_ref->declared_type == TypeDataNever::create();
        if (!allow_missing) {
          fire(cur_f, v->get_body()->loc, "field `" + field_ref->name + "` missed in initialization of struct " + to_string(struct_ref));
        }
      }
    }

    v->mutate()->assign_struct_ref(struct_ref);
    assign_inferred_type(v, TypeDataStruct::create(struct_ref));
    assign_inferred_type(v->get_body(), v);
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

  FlowContext process_block_statement(V<ast_block_statement> v, FlowContext&& flow) {
    // we'll print a warning if after some statement, control flow became unreachable
    // (but don't print a warning if it's already unreachable, for example we're inside always-false if)
    bool initially_unreachable = flow.is_unreachable();
    for (AnyV item : v->get_items()) {
      if (flow.is_unreachable() && !initially_unreachable && !v->first_unreachable && item->kind != ast_empty_statement) {
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
          fire(fun_ref, v_function->get_body()->as<ast_block_statement>()->loc_end, "a function returning `never` can not have a reachable endpoint");
        }
      }

      if (!fun_ref->declared_return_type) {
        TypeInferringUnifyStrategy return_unifier;
        if (fun_ref->does_return_self()) {
          return_unifier.unify_with(fun_ref->parameters[0].declared_type);
        }
        bool has_void_returns = false;
        bool has_non_void_returns = false;
        for (AnyExprV return_expr : return_statements) {
          TypePtr cur_type = return_expr->inferred_type;     // `return expr` - type of expr; `return` - void
          return_unifier.unify_with(cur_type);
          has_void_returns |= cur_type == TypeDataVoid::create();
          has_non_void_returns |= cur_type != TypeDataVoid::create();
        }
        inferred_return_type = return_unifier.get_result();
        if (inferred_return_type == nullptr) {    // if no return statements at all
          inferred_return_type = TypeDataVoid::create();
        }

        if (!body_end.is_unreachable() && inferred_return_type != TypeDataVoid::create()) {
          fire(fun_ref, v_function->get_body()->as<ast_block_statement>()->loc_end, "missing return");
        }
        if (has_void_returns && has_non_void_returns) {
          for (AnyExprV return_expr : return_statements) {
            if (return_expr->inferred_type == TypeDataVoid::create()) {
              fire(fun_ref, return_expr->loc, "mixing void and non-void returns in function " + to_string(fun_ref));
            }
          }
        }
        if (return_unifier.is_union_of_different_types()) {
          // `return intVar` + `return sliceVar` results in `int | slice`, probably unexpected
          fire(fun_ref, v_function->get_body()->loc, "function " + to_string(fun_ref) + " calculated return type is " + to_string(inferred_return_type) + "; probably, it's not what you expected\ndeclare `fun (...): <return_type>` manually");
        }
      }

    } else {
      // asm functions should be strictly typed, this was checked earlier
      tolk_assert(fun_ref->declared_return_type);
    }

    // visit default values of parameters; to correctly track symbols in `fun f(a: int, b: int = a)`, use flow context
    FlowContext params_flow;
    for (int i = 0; i < fun_ref->get_num_params(); ++i) {
      LocalVarPtr param_ref = &fun_ref->get_param(i);
      if (param_ref->has_default_value()) {
        params_flow = infer_any_expr(param_ref->default_value, std::move(params_flow), false, param_ref->declared_type).out_flow;
      }
      params_flow.register_known_type(SinkExpression(param_ref), param_ref->declared_type);
    }

    assign_fun_full_type(fun_ref, inferred_return_type);
    fun_ref->mutate()->assign_is_type_inferring_done();
  }

  // given `const a = 2 + 3` infer that it's int
  // for `const a: int = ...` still infer all sub expressions (to be checked in a later pipe)
  void start_visiting_constant(GlobalConstPtr const_ref) {
    infer_any_expr(const_ref->init_value, FlowContext(), false, const_ref->declared_type);
    const_ref->mutate()->assign_inferred_type(const_ref->declared_type == nullptr ? const_ref->init_value->inferred_type : const_ref->declared_type);
  }

  // given struct field `a: int = 2 + 3` infer that default value is int, assign inferred_type to all nodes
  void start_visiting_struct_fields(StructPtr struct_ref) {
    for (StructFieldPtr field_ref : struct_ref->fields) {
      if (field_ref->has_default_value()) {
        infer_any_expr(field_ref->default_value, FlowContext(), false, field_ref->declared_type);
      }
    }
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
    fire(fun_ref, fun_ref->loc, "could not infer return type of " + to_string(fun_ref) + ", because it appears in a recursive call chain\ndeclare `fun (...): <return_type>` manually");
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

  // infer types for default values in structs
  for (StructPtr struct_ref : get_all_declared_structs()) {
    if (!struct_ref->is_generic_struct()) {
      visitor.start_visiting_struct_fields(struct_ref);
    }
  }
}

void pipeline_infer_types_and_calls_and_fields(FunctionPtr fun_ref) {
  InferTypesAndCallsAndFieldsVisitor visitor;
  visitor.start_visiting_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
}

} // namespace tolk
