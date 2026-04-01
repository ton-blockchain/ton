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
#include "ast-aux-data.h"
#include "ast-visitor.h"
#include "compilation-errors.h"
#include "constant-evaluator.h"
#include "type-system.h"
#include "common/refint.h"
#include "smart-casts-cfg.h"
#include "pack-unpack-api.h"
#include "gen-entrypoints.h"

/*
 *   This pipe is the last one operating AST: it transforms AST to IR.
 *   IR is described as "Op" struct. So, here AST is transformed to Ops, and then all the rest "legacy"
 * kernel (initially forked from FunC) comes into play.
 *   Up to this point, all types have been inferred, all validity checks have been passed, etc.
 * All properties in AST nodes are assigned and can be safely used (fun_ref, etc.).
 * So, if execution reaches this pass, the input is (almost) correct, and code generation should succeed.
 *   (previously, there was a check for one variable modified twice like `(t.0, t.0) = rhs`, but after changing
 * execution order of assignment to "first lhs, then lhs", it was removed for several reasons)
*
 *   A noticeable property for IR generation is "target_type" used to extend/shrink stack.
 *   Example: `var a: (int,int)? = null`. This `null` has inferred_type "null literal", but target_type "nullable tensor",
 *            and when it's assigned, it's "expanded" from 1 stack slot to 3 (int + int + null flag).
 *   Example: `fun analyze(t: (int,int)?)` and a call `analyze((1,2))`. `(1,2)` is `(int,int)` (2 stack slots),
 *            and when passed to target (3 slots, one for null flag), this null flag is implicitly added (zero value).
 *   Example: `nullableInt!`; for `nullableInt` inferred_type is `int?`, and target_type is `int`
 *            (this doesn't lead to stack reorganization, but in case `nullableTensor!` does)
 *            (inferred_type of `nullableInt!` is `int`, and its target_type depends on its usage).
 *   Example: `var a: int|slice = 5`. This `5` should be extended as "5 1" (5 for value, 1 for type_id of `int`).
 */

namespace tolk {

class LValContext;
std::vector<var_idx_t> pre_compile_expr(AnyExprV v, CodeBlob& code, TypePtr target_type = nullptr, LValContext* lval_ctx = nullptr);
std::vector<var_idx_t> pre_compile_symbol(const Symbol* sym, CodeBlob& code, AnyV origin, LValContext* lval_ctx);
void process_any_statement(AnyV v, CodeBlob& code);
std::vector<var_idx_t> transition_rvect_to_runtime_type(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr from_type, TypePtr dest_type, AnyV origin);

static thread_local AnyV stmt_before_immediate_return = nullptr;

// The goal of VarsModificationWatcher is to detect such cases: `return (x, x += y, x)`.
// Without any changes, ops will be { _Call '2 = +('0_x, '1_y); _Return '0_x, '2, '0_x } - incorrect.
// Correct will be to introduce tmp var: { _Let '3 = '0_x; _Call '2 = ...; _Return '3, '2, '0_x }.
// This class is a global registry for callbacks, it does not insert tmp values. See `pre_compile_tensor()`.
class VarsModificationWatcher {
  struct WatchedVar {
    var_idx_t ir_idx;
    std::function<void(AnyV, var_idx_t)> on_modification_callback;
  };

  std::vector<WatchedVar> all_callbacks;

public:

  bool empty() const { return all_callbacks.empty(); }

  void push_callback(var_idx_t ir_idx, std::function<void(AnyV, var_idx_t)> callback) {
    all_callbacks.emplace_back(WatchedVar{ir_idx, std::move(callback)});
  }

  void pop_callback(var_idx_t ir_idx) {
    for (auto it = all_callbacks.rbegin(); it != all_callbacks.rend(); ++it) {
      if (it->ir_idx == ir_idx) {
        all_callbacks.erase((it + 1).base());
        return;
      }
    }
    tolk_assert(false);
  }

  void trigger_callbacks(const std::vector<var_idx_t>& left_lval_indices, AnyV origin) const {
    for (const WatchedVar& w : all_callbacks) {
      for (var_idx_t changed_var : left_lval_indices) {
        if (w.ir_idx == changed_var) {
          w.on_modification_callback(origin, w.ir_idx);
        }
      }
    }
  }
};

static thread_local VarsModificationWatcher vars_modification_watcher;


static int calc_offset_on_stack(const TypeDataTensor* t_tensor, int index_at) {
  int stack_offset = 0;
  for (int i = 0; i < index_at; ++i) {
    stack_offset += t_tensor->items[i]->get_width_on_stack();
  }
  return stack_offset;
}

static int calc_offset_on_stack(StructPtr struct_ref, int field_idx) {
  int stack_offset = 0;
  for (int i = 0; i < field_idx; ++i) {
    stack_offset += struct_ref->get_field(i)->declared_type->get_width_on_stack();
  }
  return stack_offset;
}


// The main goal of LValContext is to handle non-primitive lvalues. At IR level, a usual local variable
// exists, but on its change, something non-trivial should happen.
// Example: `globVar = 9` actually does `Const '5 = 9` + `Let 'g = '5` + `SetGlob "globVar" = 'g`.
// Example: `tupleShaped.0 = 9` actually does `Const '5 = 9` + `Let '6 = '5` + `Const '7 = 0` + `Call array.set('4, '6, '7)`.
// For functions that mutate `self`, like `g.inc().inc()`, the inner call is evaluated as lval, returning `g`.
class LValContext {
  // every global variable used as lvalue is registered here
  // example: `globalInt = 9`, does `GetGlob '8 "globalInt"`, and when '8 is modified, "SetGlob" is inserted
  struct ModifiedGlobal {
    GlobalVarPtr glob_ref;
    std::vector<var_idx_t> ir_global;    // typically 1, generally get_width_on_stack() of global tensors

    // for 1-slot globals int/cell/slice, assigning to them is just SETGLOB
    // same for tensors, if they are fully rewritten in an expression: `gTensor = (5,6)`
    void apply_fully_rewrite(CodeBlob& code, AnyV origin) const {
      code.add_set_glob_var(origin, ir_global, glob_ref);
    }

    // for N-slot globals tensor/struct/union, assigning to their parts, like `gTensor.1 = 6`
    // we need to read gTensor as a whole (0-th and 1-th component), rewrite 1-th component, and SETGLOB a whole back
    void apply_partially_rewrite(CodeBlob& code, AnyV origin, std::vector<bool>&& was_modified_by_let) const {
      std::vector local_ir_idx = pre_compile_symbol(glob_ref, code, origin, nullptr);
      for (size_t i = 0; i < local_ir_idx.size(); ++i) {
        if (was_modified_by_let[i]) {
          code.add_let(origin, {local_ir_idx[i]}, {ir_global[i]});
        }
      }

      code.add_set_glob_var(origin, std::move(local_ir_idx), glob_ref);
    }
  };

  // every shaped tuple index used as lvalue is registered here
  // example: `t.0 = 9`, reads `t.0` into 'N, and when 'N is modified, inserts "array.set"
  // for instance, modifying `t.0.1`, will read t.0, modify 1-st element, and modify that t.0 is also changed
  struct ModifiedShapedTupleIndex {
    AnyExprV shaped_obj;  // it's a sink expression: `t.0`, not `getT().0` and similar
    int index_at;         // 0 for `t.0`
    std::vector<var_idx_t> ir_field;

    void apply_partially_rewrite(CodeBlob& code, AnyV origin, std::vector<bool>&& was_modified_by_let) const {
      // for safety, especially when modifying structs inside shapes like `sh.0.x = 10`,
      // re-read a field by index_at, apply changes from LET, and write back: the same as for globals
      const TypeDataShapedTuple* t_shaped = shaped_obj->inferred_type->unwrap_alias()->try_as<TypeDataShapedTuple>();
      LValContext local_lval;
      std::vector tuple_ir_idx = pre_compile_expr(shaped_obj, code, nullptr, &local_lval);
      std::vector index_ir_idx = code.create_tmp_var(TypeDataInt::create(), origin, "(tuple-idx)");
      code.add_int_const(origin, index_ir_idx, td::make_refint(index_at));
      std::vector field_ir_idx = code.create_tmp_var(t_shaped->items[index_at], origin, "(tuple-field)");
      code.add_call(origin, field_ir_idx, {tuple_ir_idx[0], index_ir_idx[0]}, lookup_function("array<T>.get"));
      for (size_t i = 0; i < field_ir_idx.size(); ++i) {
        if (was_modified_by_let[i]) {
          code.add_let(origin, {field_ir_idx[i]}, {ir_field[i]});
        }
      }

      vars_modification_watcher.trigger_callbacks(tuple_ir_idx, origin);
      std::vector<var_idx_t> ir_args;   // array.set(tuple, new_field, index_at)
      ir_args.reserve(tuple_ir_idx.size() + field_ir_idx.size() + 1);
      ir_args.insert(ir_args.end(), tuple_ir_idx.begin(), tuple_ir_idx.end());
      ir_args.insert(ir_args.end(), field_ir_idx.begin(), field_ir_idx.end());
      ir_args.insert(ir_args.end(), index_ir_idx.begin(), index_ir_idx.end());
      code.add_call(origin, tuple_ir_idx, std::move(ir_args), lookup_function("array<T>.set"));
      local_lval.after_let(std::move(tuple_ir_idx), code, origin);
    }
  };

  // every lval index of a tensor/struct inside a shaped tuple is registered here
  // example: `var t: [Point]` and `t.0.x = 10` (we need to update the 0-th element)
  // note, that `globalPoint.x = 10` is not registered here: only inner-shape modifications
  struct ModifiedTensorIndex {
    AnyExprV tensor_obj;      // it's a sink expression: `t.0.x`, not `getT().0.x`
    int stack_offset;
    std::vector<var_idx_t> ir_field;

    void apply(CodeBlob& code, AnyV origin) const {
      LValContext local_lval;
      std::vector obj_ir_idx = pre_compile_expr(tensor_obj, code, nullptr, &local_lval);
      std::vector field_ir_idx(obj_ir_idx.begin() + stack_offset, obj_ir_idx.begin() + stack_offset + static_cast<int>(ir_field.size()));

      vars_modification_watcher.trigger_callbacks(field_ir_idx, origin);
      code.add_let(origin, field_ir_idx, ir_field);
      local_lval.after_let(std::move(field_ir_idx), code, origin);
    }
  };

  AnyExprV mutated_self_obj = nullptr;    // for `g.inc().inc()` it's `g`, returned from the inner call to the outer
  std::vector<std::variant<ModifiedGlobal, ModifiedShapedTupleIndex, ModifiedTensorIndex>> modifications;

  static bool vector_contains(const std::vector<var_idx_t>& ir_vars, var_idx_t ir_idx) {
    for (var_idx_t var_in_vector : ir_vars) {
      if (var_in_vector == ir_idx) {
        return true;
      }
    }
    return false;
  }

  static bool vector_contains(const std::vector<var_idx_t>& ir_vars, const std::vector<var_idx_t>& ir_idx_arr) {
    for (var_idx_t ir_idx : ir_idx_arr) {
      if (vector_contains(ir_vars, ir_idx)) {
        return true;
      }
    }
    return false;
  }

  static int vector_contains(const std::vector<var_idx_t>& ir_vars, const std::vector<var_idx_t>& ir_idx_arr, std::vector<bool>& was_modified_by_let) {
    int n_modified_by_let = 0;
    was_modified_by_let.resize(ir_idx_arr.size());
    for (size_t i = 0; i < ir_idx_arr.size(); ++i) {
      if (vector_contains(ir_vars, ir_idx_arr[i])) {
        was_modified_by_let[i] = true;
        n_modified_by_let++;
      }
    }
    return n_modified_by_let;
  }

public:
  AnyExprV get_mutated_self_obj() const { return mutated_self_obj; }
  void set_mutated_self_obj(AnyExprV v) { mutated_self_obj = v; }

  void capture_global_modification(GlobalVarPtr glob_ref, std::vector<var_idx_t> ir_global) {
    modifications.emplace_back(ModifiedGlobal{glob_ref, std::move(ir_global)});
  }

  void capture_shaped_tuple_index_modification(AnyExprV shaped_obj, int index_at, std::vector<var_idx_t> ir_field) {
    modifications.emplace_back(ModifiedShapedTupleIndex{shaped_obj, index_at, std::move(ir_field)});
  }

  void capture_tensor_index_modification(AnyExprV tensor_obj, int stack_offset, std::vector<var_idx_t> ir_field) {
    modifications.emplace_back(ModifiedTensorIndex{tensor_obj, stack_offset, std::move(ir_field)});
  }

  void after_let(std::vector<var_idx_t>&& let_left_vars, CodeBlob& code, AnyV origin) const {
    for (const auto& modification : modifications) {
      if (const auto* m_glob = std::get_if<ModifiedGlobal>(&modification)) {
        std::vector<bool> was_modified_by_let;
        int n_modified_by_let = vector_contains(let_left_vars, m_glob->ir_global, was_modified_by_let);
        if (n_modified_by_let == static_cast<int>(m_glob->ir_global.size())) {
          m_glob->apply_fully_rewrite(code, origin);
        } else if (n_modified_by_let > 0) {
          m_glob->apply_partially_rewrite(code, origin, std::move(was_modified_by_let));
        }
      } else if (const auto* m_tup = std::get_if<ModifiedShapedTupleIndex>(&modification)) {
        std::vector<bool> was_modified_by_let;
        if (vector_contains(let_left_vars, m_tup->ir_field, was_modified_by_let)) {
          m_tup->apply_partially_rewrite(code, origin, std::move(was_modified_by_let));
        }
      } else if (const auto* m_tens = std::get_if<ModifiedTensorIndex>(&modification)) {
        if (vector_contains(let_left_vars, m_tens->ir_field)) {
          m_tens->apply(code, origin);
        }
      }
    }
  }
};

// the purpose of this class is having a call `f(a1,a2,...)` when f has asm arg_order, to check
// whether it's safe to rearrange arguments (to evaluate them in arg_order right here for fewer stack manipulations)
// or it's unsafe, and we should evaluate them left-to-right;
// example: `f(1,2,3)` / `b.storeUint(2,32)` is safe;
// example: `f(x,x+=5,x)` / `f(impureF1(), global_var)` / `f(s.loadInt(), s.loadInt())` is unsafe;
// the same rules are used to check an object literal: is it safe to convert `{y:expr, x:expr}` to declaration order {x,y}
class CheckReorderingForAsmArgOrderIsSafeVisitor final : public ASTVisitorFunctionBody {
  bool has_side_effects = false;

  void visit(V<ast_function_call> v) override {
    has_side_effects |= v->fun_maybe == nullptr || !v->fun_maybe->is_marked_as_pure() || v->fun_maybe->has_mutate_params();
    parent::visit(v);
  }

  void visit(V<ast_assign> v) override {
    has_side_effects = true;
    parent::visit(v);
  }

  void visit(V<ast_set_assign> v) override {
    has_side_effects = true;
    parent::visit(v);
  }

  void visit(V<ast_braced_expression> v) override {
    has_side_effects = true;    // e.g., `f(arg1, match(v) { 7 => return 0 })`
    parent::visit(v);           // (treat any "statement-expression" argument as unsafe to reorder)
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    tolk_assert(false);
  }

  static bool is_safe_to_reorder(V<ast_function_call> v) {
    for (const LocalVarData& param : v->fun_maybe->parameters) {
      if (param.declared_type->get_width_on_stack() != 1) {
        return false;
      }
    }

    CheckReorderingForAsmArgOrderIsSafeVisitor visitor;
    for (int i = 0; i < v->get_num_args(); ++i) {
      visitor.ASTVisitorFunctionBody::visit(v->get_arg(i)->get_expr());
    }
    if (v->dot_obj_is_self) {
      visitor.ASTVisitorFunctionBody::visit(v->get_self_obj());
    }
    return !visitor.has_side_effects;
  }

  static bool is_safe_to_reorder(V<ast_object_body> v) {
    CheckReorderingForAsmArgOrderIsSafeVisitor visitor;
    for (int i = 0; i < v->get_num_fields(); ++i) {
      visitor.ASTVisitorFunctionBody::visit(v->get_field(i)->get_init_val());
    }
    return !visitor.has_side_effects;
  }
};

// when a call to `f()` was inlined, f's body was processed, leaving some state
// that should be cleared upon next inlining;
// for instance, ir_idx of local variables point to caller (where f was inlined)
class ClearStateAfterInlineInPlace final : public ASTVisitorFunctionBody {

  void visit(V<ast_local_var_lhs> v) override {
    v->var_ref->mutate()->assign_ir_idx({});
  }

  void visit(V<ast_try_catch_statement> v) override {
    for (AnyExprV v_catch_var : v->get_catch_expr()->get_items()) {
      if (auto v_ref = v_catch_var->try_as<ast_reference>(); v_ref && v_ref->sym) {
        v_ref->sym->try_as<LocalVarPtr>()->mutate()->assign_ir_idx({});
      }
    }
    parent::visit(v);
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    tolk_assert(false);
  }

  void on_enter_function(V<ast_function_declaration> v_function) override {
    tolk_assert(cur_f->is_inlined_in_place());

    for (int i = 0; i < cur_f->get_num_params(); ++i) {
      cur_f->get_param(i).mutate()->assign_ir_idx({});
    }
  }
};


// CodeBlob has a mapping [st => ptr]
const LazyVariableLoadedState* CodeBlob::get_lazy_variable(LocalVarPtr var_ref) const {
  for (const LazyVarRefAtCodegen& stored : lazy_variables) {
    if (stored.var_ref == var_ref) {
      return stored.var_state;
    }
  }
  return nullptr;
}

// detect `st` by vertex "st"
const LazyVariableLoadedState* CodeBlob::get_lazy_variable(AnyExprV v) const {
  if (auto as_ref = v->try_as<ast_reference>()) {
    if (LocalVarPtr var_ref = as_ref->sym->try_as<LocalVarPtr>()) {
      return get_lazy_variable(var_ref);
    }
  }
  return nullptr;
}

// ternary `x ? y : z` can be optimized to asm `CONDSEL` (not IF/ELSE), if y and z don't require evaluation;
// example when can: `cond ? 2 : null`, `x == null ? some_var : obj.field`;
// example when not: `cond ? f() : g()` and other non-trivial arguments
static bool is_ternary_arg_trivial_for_condsel(AnyExprV v, bool require_1slot = true) {
  if (require_1slot && v->inferred_type->get_width_on_stack() != 1) {
    return false;
  }
  if (v->kind == ast_int_const || v->kind == ast_string_const || v->kind == ast_bool_const ||
      v->kind == ast_null_keyword || v->kind == ast_reference) {
    return true;
  }
  if (auto v_dot = v->try_as<ast_dot_access>()) {
    TypePtr obj_type = v_dot->get_obj()->inferred_type;
    if (obj_type && !obj_type->unwrap_alias()->try_as<TypeDataShapedTuple>()) {   // `t.0` for tuples is a runtime call, not trivial
      return is_ternary_arg_trivial_for_condsel(v_dot->get_obj(), false);
    }
    return false;
  }
  if (auto v_cast = v->try_as<ast_not_null_operator>()) {
    return is_ternary_arg_trivial_for_condsel(v_cast->get_expr(), require_1slot);
  }
  if (auto v_call = v->try_as<ast_function_call>()) {
    return v_call->fun_maybe && v_call->fun_maybe->is_compile_time_const_val();
  }
  return false;
}

// invoke the type transition of IR slots only if potentially needed to
// (if an expression is targeted to another type)
GNU_ATTRIBUTE_ALWAYS_INLINE
static std::vector<var_idx_t> transition_to_target_type(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr target_type, AnyExprV v) {
  if (target_type != nullptr && target_type != v->inferred_type) {
    rvect = transition_rvect_to_runtime_type(std::move(rvect), code, v->inferred_type, target_type, v);
  }
  return rvect;
}

// the second overload of the same function, invoke impl only when original and target differ
GNU_ATTRIBUTE_ALWAYS_INLINE
std::vector<var_idx_t> transition_to_target_type(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr original_type, TypePtr target_type, AnyV origin) {
  if (target_type != original_type) {
    rvect = transition_rvect_to_runtime_type(std::move(rvect), code, original_type, target_type, origin);
  }
  return rvect;
}


static std::vector<std::vector<var_idx_t>> pre_compile_tensor_inner(CodeBlob& code, const std::vector<AnyExprV>& args,
                                          const std::vector<TypePtr>& target_types_for_arg,
                                          const std::vector<LValContext*>& lval_ctx_for_arg) {
  tolk_assert(target_types_for_arg.size() == args.size());
  tolk_assert(lval_ctx_for_arg.empty() || lval_ctx_for_arg.size() == args.size());
  const int n = static_cast<int>(args.size());
  if (n == 0) {  // just `()`
    return {};
  }
  if (n == 1) {  // just `(x)`: even if x is modified (e.g. `f(x=x+2)`), there are no next arguments
    LValContext* lval_ctx = lval_ctx_for_arg.empty() ? nullptr : lval_ctx_for_arg[0];
    return {pre_compile_expr(args[0], code, target_types_for_arg[0], lval_ctx)};
  }

  // the purpose is to handle such cases: `return (x, x += y, x)`
  // without this, ops will be { _Call '2 = +('0_x, '1_y); _Return '0_x, '2, '0_x } - invalid
  // with this, ops will be { _Let '3 = '0_x; _Call '2 = ...; _Return '3, '2, '0_x } - valid, tmp var for x
  // how it works: for every arg, after transforming to ops, start tracking ir_idx inside it
  // on modification attempt, create Op::_Let to a tmp var and replace old ir_idx with tmp_idx in result
  struct WatchingVarList {
    struct WatchedVarInfo {
      var_idx_t ir_idx;
      OpList* saved_list;       // the OpList being built at the time variable was added
      size_t saved_index;       // index in that OpList (position where Op::_Let will be inserted)
      size_t saved_stack_depth; // cur_ops_stack.size() at that moment
    };

    std::vector<WatchedVarInfo> watched_vars;
    std::vector<std::vector<var_idx_t>> res_lists;

    explicit WatchingVarList(int n_args) {
      res_lists.reserve(n_args);
    }

    WatchedVarInfo* find_watched(var_idx_t ir_idx) {
      for (auto& w : watched_vars) {
        if (w.ir_idx == ir_idx) {
          return &w;
        }
      }
      return nullptr;
    }

    void add_and_watch_modifications(std::vector<var_idx_t>&& vars_of_ith_arg, CodeBlob& code) {
      for (var_idx_t ir_idx : vars_of_ith_arg) {
        if (!code.vars[ir_idx].name.empty()) {
          WatchedVarInfo* existing = find_watched(ir_idx);
          if (existing) {
            // Variable is already watched. Update saved position if we're at the same nesting level,
            // so that subsequent modifications insert Op::_Let after this argument, not before.
            if (code.cur_ops_stack.size() == existing->saved_stack_depth) {
              existing->saved_list = code.cur_ops;
              existing->saved_index = code.cur_ops->size();
            }
          } else {
            // Variable is not watched yet. Start watching it: remember current insertion point
            // and nesting level, and register a callback for when this variable is modified.
            size_t watch_idx = watched_vars.size();
            watched_vars.push_back({ir_idx, code.cur_ops, code.cur_ops->size(), code.cur_ops_stack.size()});
            vars_modification_watcher.push_callback(ir_idx, [this, &code, watch_idx](AnyV origin, var_idx_t ir_idx) {
              on_var_modified(ir_idx, origin, code, watch_idx);
            });
          }
        }
      }
      res_lists.emplace_back(std::move(vars_of_ith_arg));
    }

    void on_var_modified(var_idx_t ir_idx, AnyV origin, CodeBlob& code, size_t watch_idx) {
      WatchedVarInfo& info = watched_vars[watch_idx];
      tolk_assert(info.ir_idx == ir_idx);
      std::vector tmp_idx_arr = code.create_tmp_var(code.vars[ir_idx].v_type, origin, "(pre-modified)");
      tolk_assert(tmp_idx_arr.size() == 1);
      var_idx_t tmp_idx = tmp_idx_arr[0];

      // if we are in a nested block (inside a ternary), insert Op::_Let at saved position;
      // example: `return (x, condition ? x = 9 : x = 10, x)`, we are in "x=9", insert outside ternary;
      // otherwise, insert at current position (standard behavior)
      if (code.cur_ops_stack.size() > info.saved_stack_depth) {
        info.saved_list->insert(info.saved_list->begin() + static_cast<long>(info.saved_index),
                                Op::make_let(origin, {tmp_idx}, {ir_idx}));
        // adjust indices of all watchers pointing to the same list at or after the insertion point
        for (auto& w : watched_vars) {
          if (w.saved_list == info.saved_list && w.saved_index >= info.saved_index) {
            w.saved_index++;
          }
        }
      } else {
        code.add_let(origin, {tmp_idx}, {ir_idx});
      }

      for (std::vector<var_idx_t>& prev_vars : res_lists) {
        std::replace(prev_vars.begin(), prev_vars.end(), ir_idx, tmp_idx);
      }
    }

    std::vector<std::vector<var_idx_t>> clear_and_stop_watching() {
      for (const WatchedVarInfo& w : watched_vars) {
        vars_modification_watcher.pop_callback(w.ir_idx);
      }
      watched_vars.clear();
      return std::move(res_lists);
    }
  };

  WatchingVarList watched_vars(n);
  for (int arg_idx = 0; arg_idx < n; ++arg_idx) {
    LValContext* lval_ctx = lval_ctx_for_arg.empty() ? nullptr : lval_ctx_for_arg[arg_idx];
    std::vector vars_of_ith_arg = pre_compile_expr(args[arg_idx], code, target_types_for_arg[arg_idx], lval_ctx);
    watched_vars.add_and_watch_modifications(std::move(vars_of_ith_arg), code);
  }
  return watched_vars.clear_and_stop_watching();
}

static std::vector<var_idx_t> pre_compile_tensor(CodeBlob& code, const std::vector<AnyExprV>& args,
                                          LValContext* lval_ctx = nullptr, std::vector<TypePtr> target_types = {}) {
  if (target_types.empty()) {
    target_types.reserve(args.size());
    for (AnyExprV ith_arg : args) {
      target_types.push_back(ith_arg->inferred_type);
    }
  }
  std::vector<LValContext*> lval_ctx_for_arg(args.size(), lval_ctx);
  std::vector<std::vector<var_idx_t>> res_lists = pre_compile_tensor_inner(code, args, target_types, lval_ctx_for_arg);
  std::vector<var_idx_t> res;
  for (const std::vector<var_idx_t>& list : res_lists) {
    res.insert(res.end(), list.cbegin(), list.cend());
  }

  return res;
}

static std::vector<var_idx_t> pre_compile_let(CodeBlob& code, AnyExprV lhs, AnyExprV rhs) {
  // [lhs] = rhs; it's un-tuple to N left vars
  if (lhs->kind == ast_square_brackets) {
    const TypeDataShapedTuple* l_shaped = lhs->inferred_type->unwrap_alias()->try_as<TypeDataShapedTuple>();
    const TypeDataShapedTuple* r_shaped = rhs->inferred_type->unwrap_alias()->try_as<TypeDataShapedTuple>();
    tolk_assert(l_shaped && r_shaped && l_shaped->size() == r_shaped->size());
    std::vector ir_right = pre_compile_expr(rhs, code);
    std::vector rvect = code.create_tmp_var(TypeDataTensor::create(std::vector(r_shaped->size(), TypeDataUnknown::create())), rhs, "(unpack-tuple)");
    code.add_un_tuple(lhs, rvect, ir_right);
    LValContext local_lval;
    std::vector ir_left = pre_compile_tensor(code, lhs->as<ast_square_brackets>()->get_items(), &local_lval);
    vars_modification_watcher.trigger_callbacks(ir_left, lhs);
    int stack_offset = 0;
    for (int i = 0; i < l_shaped->size(); ++i) {
      int ith_w = l_shaped->items[i]->get_width_on_stack();
      std::vector ith_lvect(ir_left.begin() + stack_offset, ir_left.begin() + stack_offset + ith_w);
      std::vector ith_rvect = transition_to_target_type({rvect[i]}, code, TypeDataUnknown::create(), r_shaped->items[i], rhs);
      ith_rvect = transition_to_target_type(std::move(ith_rvect), code, r_shaped->items[i], l_shaped->items[i], rhs);
      code.add_let(lhs, ith_lvect, ith_rvect);
      stack_offset += ith_w;
    }
    local_lval.after_let(std::move(ir_left), code, lhs);
    return ir_right;
  }
  // lhs = rhs (resulting IR vars is rhs)
  // note, that we calculate RHS at first; earlier lhs was calculated first, to support "someF().field = calc()",
  // but since lvalues are restricted to strict paths (v.0.nested, etc.), function calls not allowed in lhs
  std::vector ir_right = pre_compile_expr(rhs, code, nullptr);
  std::vector ir_assignable = transition_to_target_type(std::vector(ir_right), code, rhs->inferred_type, lhs->inferred_type, rhs);
  LValContext local_lval;
  std::vector ir_left = pre_compile_expr(lhs, code, nullptr, &local_lval);
  vars_modification_watcher.trigger_callbacks(ir_left, lhs);
  code.add_let(lhs, ir_left, std::move(ir_assignable));
  local_lval.after_let(std::move(ir_left), code, lhs);
  return ir_right;
}

std::vector<var_idx_t> pre_compile_is_type(CodeBlob& code, TypePtr expr_type, TypePtr cmp_type, const std::vector<var_idx_t>& expr_ir_idx, AnyV origin, const char* debug_desc) {
  FunctionPtr eq_sym = lookup_function("_==_");
  FunctionPtr isnull_sym = lookup_function("__isNull");
  FunctionPtr not_sym = lookup_function("!b_");
  std::vector ir_result = code.create_tmp_var(TypeDataBool::create(), origin, debug_desc);

  const TypeDataUnion* lhs_union = expr_type->unwrap_alias()->try_as<TypeDataUnion>();
  if (!lhs_union && expr_type == TypeDataUnknown::create() && cmp_type == TypeDataNullLiteral::create()) {
    // `unknown == null`, it works even though `unknown` is not a union
    tolk_assert(expr_ir_idx.size() == 1);
    code.add_call(origin, ir_result, expr_ir_idx, isnull_sym);
  } else if (!lhs_union) {
    // `int` is `int` / `int` is `builder`, it's compile-time, either 0, or -1
    bool types_eq = expr_type->equal_to(cmp_type);
    code.add_int_const(origin, ir_result, td::make_refint(types_eq ? -1 : 0));
  } else if (lhs_union->is_primitive_nullable() && cmp_type == TypeDataNullLiteral::create()) {
    // `int?` is `null` for primitive 1-slot nullables, they hold either value of TVM NULL, no extra union tag slot
    code.add_call(origin, ir_result, expr_ir_idx, isnull_sym);
  } else if (lhs_union->is_primitive_nullable()) {
    // `int?` is `int` (check for null actually) / `int?` is `builder` (compile-time false actually)
    bool cant_happen = lhs_union->or_null->get_type_id() != cmp_type->get_type_id();
    if (cant_happen) {
      code.add_int_const(origin, ir_result, td::make_refint(0));
    } else {
      code.add_call(origin, ir_result, expr_ir_idx, isnull_sym);
      code.add_call(origin, ir_result, ir_result, not_sym);
    }
  } else {
    // `int | slice` is `int`, check type id
    std::vector ir_typeid = code.create_tmp_var(TypeDataInt::create(), origin, "(type-id)");
    code.add_int_const(origin, ir_typeid, td::make_refint(cmp_type->get_type_id()));
    code.add_call(origin, ir_result, {ir_typeid[0], expr_ir_idx.back()}, eq_sym);
  }

  return ir_result;
}


static std::vector<var_idx_t> gen_compile_time_code_instead_of_fun_call(CodeBlob& code, V<ast_function_call> v_call, const std::vector<std::vector<var_idx_t>>& vars_per_arg) {
  FunctionPtr called_f = v_call->fun_maybe;

  if (called_f->is_method() && called_f->is_instantiation_of_generic_function()) {
    std::string_view f_name = called_f->base_fun_ref->name;
    const LazyVariableLoadedState* lazy_variable = v_call->dot_obj_is_self ? code.get_lazy_variable(v_call->get_self_obj()) : nullptr;

    if (f_name == "T.toCell" && lazy_variable && lazy_variable->is_struct()) {
      // in: object Lazy<T> (partially loaded), out: Cell<T>
      std::vector ir_obj = vars_per_arg[0];   // = lazy_var_ref->ir_idx
      return generate_lazy_struct_to_cell(code, v_call, &lazy_variable->loaded_state, std::move(ir_obj), vars_per_arg[1]);
    }
    if (f_name == "T.forceLoadLazyObject") {
      // in: object T, out: slice (same slice that a lazy variable holds, after loading/skipping all its fields)
      if (!lazy_variable) {
        err("this method is applicable to lazy variables only").fire(v_call->get_callee(), code.fun_ref);
      }
      std::vector ir_obj = vars_per_arg[0];
      return generate_lazy_object_finish_loading(code, v_call, lazy_variable, std::move(ir_obj));
    }
  }

  auto gen = std::get_if<FunctionBodyBuiltinGenerateOps*>(&called_f->body);
  tolk_assert(gen);
  return (*gen)->generate_ops(called_f, code, v_call, vars_per_arg);
}

std::vector<var_idx_t> gen_inline_fun_call_in_place(CodeBlob& code, TypePtr ret_type, AnyV origin, FunctionPtr f_inlined, AnyExprV self_obj, bool is_before_immediate_return, const std::vector<std::vector<var_idx_t>>& vars_per_arg) {
  tolk_assert(vars_per_arg.size() == f_inlined->parameters.size());
  for (int i = 0; i < f_inlined->get_num_params(); ++i) {
    const LocalVarData& param_i = f_inlined->get_param(i);
    if (!param_i.is_used_as_lval() && !param_i.is_mutate_parameter()) {
      // if param used for reading only, pass the same ir_idx as for an argument
      // it decreases number of tmp variables and leads to better optimizations
      // (being honest, it's quite strange that copy+LET may lead to more stack permutations)
      param_i.mutate()->assign_ir_idx(std::vector(vars_per_arg[i]));
    } else {
      std::vector ir_ith_param = code.create_var(param_i.declared_type, origin, param_i.name);
      code.add_let(origin, ir_ith_param, vars_per_arg[i]);
      param_i.mutate()->assign_ir_idx(std::move(ir_ith_param));
    }
  }

  std::vector rvect_call = code.create_tmp_var(ret_type, origin, "(inlined-return)");
  std::vector<var_idx_t>* backup_outer_inline = code.inline_rvect_out;
  FunctionPtr backup_cur_fun = code.fun_ref;
  bool backup_inline_before_return = code.inlining_before_immediate_return;
  auto backup_lazy_variables = code.lazy_variables;
  code.inline_rvect_out = &rvect_call;
  code.inlining_before_immediate_return = is_before_immediate_return;
  code.fun_ref = f_inlined;
  // specially handle `point.getX()` if point is a lazy var: to make `self.toCell()` work and `self.x` asserted;
  // (only methods preserve lazy, `getXOf(point)` does not, though theoretically can be done)
  const LazyVariableLoadedState* lazy_receiver = self_obj ? code.get_lazy_variable(self_obj) : nullptr;
  if (lazy_receiver) {
    LocalVarPtr self_var_ref = &f_inlined->parameters[0];             // `self` becomes lazy while inlining
    code.lazy_variables.emplace_back(self_var_ref, lazy_receiver);  // (points to the same slice, immutable tail, etc.)
  }

  auto v_ast_root = f_inlined->ast_root->as<ast_function_declaration>();
  auto v_block = v_ast_root->get_body()->as<ast_block_statement>();
  process_any_statement(v_block, code);

  if (f_inlined->has_mutate_params() && f_inlined->inferred_return_type == TypeDataVoid::create()) {
    std::vector<var_idx_t> mutated_vars;
    for (const LocalVarData& p_sym: f_inlined->parameters) {
      if (p_sym.is_mutate_parameter()) {
        mutated_vars.insert(mutated_vars.end(), p_sym.ir_idx.begin(), p_sym.ir_idx.end());
      }
    }
    code.add_let(origin, rvect_call, std::move(mutated_vars));
  }

  ClearStateAfterInlineInPlace visitor;
  visitor.start_visiting_function(f_inlined, v_ast_root);

  code.fun_ref = backup_cur_fun;
  code.inline_rvect_out = backup_outer_inline;
  code.inlining_before_immediate_return = backup_inline_before_return;
  code.lazy_variables = std::move(backup_lazy_variables);
  return rvect_call;
}

// convert a constant value (calculated by a "constant-evaluator") to IR vars;
// every init_val of `const XXX = ...` is calculated once (into ConstValExpression) and cached
// besides rvect, return the type this rvect has, for correct transitions
static std::pair<TypePtr, std::vector<var_idx_t>> pre_compile_constant_expression(const ConstValExpression& value, CodeBlob& code, AnyV origin) {
  if (const ConstValInt* val = std::get_if<ConstValInt>(&value)) {
    std::vector rvect = code.create_tmp_var(TypeDataInt::create(), origin, "(int-const)");
    code.add_int_const(origin, rvect, val->int_val);
    return {TypeDataInt::create(), rvect};
  }
  if (const ConstValBool* val = std::get_if<ConstValBool>(&value)) {
    std::vector rvect = code.create_tmp_var(TypeDataBool::create(), origin, "(bool-const)");
    code.add_call(origin, rvect, {}, lookup_function(val->bool_val ? "__true" : "__false"));
    return {TypeDataBool::create(), rvect};
  }
  if (const ConstValSlice* val = std::get_if<ConstValSlice>(&value)) {
    std::vector rvect = code.create_tmp_var(TypeDataSlice::create(), origin, "(slice-const)");
    code.add_slice_const(origin, rvect, val->str_hex);
    return {TypeDataSlice::create(), rvect};
  }
  if (const ConstValString* val = std::get_if<ConstValString>(&value)) {
    std::vector rvect = code.create_tmp_var(TypeDataString::create(), origin, "(str-const)");
    code.add_string_const(origin, rvect, val->str_val);
    return {TypeDataString::create(), rvect};
  }
  if (const ConstValAddress* val = std::get_if<ConstValAddress>(&value)) {
    std::vector rvect = code.create_tmp_var(TypeDataAddress::internal(), origin, "(addr-const)");
    code.add_slice_const(origin, rvect, val->std_addr_hex);
    return {TypeDataAddress::internal(), rvect};
  }
  if (std::get_if<ConstValNullLiteral>(&value)) {
    std::vector rvect = code.create_tmp_var(TypeDataNullLiteral::create(), origin, "(null-literal)");
    code.add_call(origin, rvect, {}, lookup_function("__null"));
    return {TypeDataNullLiteral::create(), rvect};
  }

  if (const ConstValTensor* val = std::get_if<ConstValTensor>(&value)) {
    std::vector<TypePtr> items_types;
    std::vector<var_idx_t> rvect;
    for (const ConstValExpression& c_item : val->items) {
      auto [item_type, item_rvect] = pre_compile_constant_expression(c_item, code, origin);
      items_types.push_back(item_type);
      rvect.insert(rvect.end(), item_rvect.begin(), item_rvect.end());
    }
    return {TypeDataTensor::create(std::move(items_types)), rvect};
  }
  if (const ConstValShapedTuple* val = std::get_if<ConstValShapedTuple>(&value)) {
    std::vector<TypePtr> items_types;
    std::vector<var_idx_t> rvect;
    for (const ConstValExpression& c_item : val->items) {
      auto [item_type, item_rvect] = pre_compile_constant_expression(c_item, code, origin);
      items_types.push_back(item_type);
      item_rvect = transition_to_target_type(std::move(item_rvect), code, item_type, TypeDataUnknown::create(), origin);
      rvect.insert(rvect.end(), item_rvect.begin(), item_rvect.end());
    }
    TypePtr t_shaped = TypeDataShapedTuple::create(std::move(items_types));
    std::vector ir_shaped = code.create_tmp_var(t_shaped, origin, "(shaped-const)");
    code.add_to_tuple(origin, ir_shaped, std::move(rvect));
    return {t_shaped, ir_shaped};
  }
  if (const ConstValObject* val = std::get_if<ConstValObject>(&value)) {
    std::vector<var_idx_t> rvect;
    for (int i = 0; i < val->struct_ref->get_num_fields(); ++i) {
      auto [field_type, field_rvect] = pre_compile_constant_expression(val->fields[i], code, origin);
      field_rvect = transition_to_target_type(std::move(field_rvect), code, field_type, val->struct_ref->get_field(i)->declared_type, origin);
      rvect.insert(rvect.end(), field_rvect.begin(), field_rvect.end());
    }
    return {TypeDataStruct::create(val->struct_ref), rvect};
  }
  if (const ConstValCastToType* val = std::get_if<ConstValCastToType>(&value)) {
    auto [original_type, rvect] = pre_compile_constant_expression(val->inner.front(), code, origin);
    rvect = transition_to_target_type(std::move(rvect), code, original_type, val->cast_to, origin);
    return {val->cast_to, rvect};
  }
  tolk_assert(false);
}


std::vector<var_idx_t> pre_compile_symbol(const Symbol* sym, CodeBlob& code, AnyV origin, LValContext* lval_ctx) {
  // referencing a local variable (not its declaration, but its usage)
  if (LocalVarPtr var_ref = sym->try_as<LocalVarPtr>()) {
#ifdef TOLK_DEBUG
    tolk_assert(static_cast<int>(var_ref->ir_idx.size()) == var_ref->declared_type->get_width_on_stack());
#endif
    return var_ref->ir_idx;
  }

  // referencing a global constant, embed its init_value directly
  if (GlobalConstPtr const_ref = sym->try_as<GlobalConstPtr>()) {
    tolk_assert(lval_ctx == nullptr);
    ConstValExpression value = eval_and_cache_const_init_val(const_ref);
    auto [original_type, rvect] = pre_compile_constant_expression(value, code, origin);
    // handle `const a: int|slice = 1`, ir_init is int(1), transition to union
    return transition_to_target_type(std::move(rvect), code, original_type, const_ref->inferred_type, origin);
  }

  // referencing a global variable, copy it to a local tmp var
  if (GlobalVarPtr glob_ref = sym->try_as<GlobalVarPtr>()) {
    // `globalVar` is used for reading, just create local IR var to represent its value, Op::_GlobVar will fill it
    // note, that global tensors are stored as a tuple an unpacked to N vars on read, N determined by declared_type
    std::vector local_ir_idx = code.create_var(glob_ref->declared_type, origin, "g_" + glob_ref->name);
    code.add_read_glob_var(origin, local_ir_idx, glob_ref);
    if (lval_ctx) {   // `g = rhs` or `g.0 = rhs`
      // for just an assignment `g = rhs`, Op::_GlobVar will be disabled, GETGLOB won't be generated:
      // we use ir vars to generate SETGLOB whenever they are modified
      lval_ctx->capture_global_modification(glob_ref, local_ir_idx);
    }
    return local_ir_idx;
  }

  // referencing a function (not calling it! using as a callback, works similar to a global var)
  if (FunctionPtr fun_ref = sym->try_as<FunctionPtr>()) {
    std::vector rvect = code.create_tmp_var(fun_ref->inferred_full_type, origin, "(glob-var-fun)");
    code.add_read_glob_var(origin, rvect, fun_ref);
    return rvect;
  }

  throw Fatal("pre_compile_symbol");
}

static std::vector<var_idx_t> process_reference(V<ast_reference> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  std::vector rvect = pre_compile_symbol(v->sym, code, v, lval_ctx);

  // a local variable might be smart cast at this point, for example we're in `if (v != null)`
  // it means that we must drop the null flag (if it's a tensor), or maybe perform other stack transformations
  // (from original var_ref->ir_idx to fit smart cast)
  if (LocalVarPtr var_ref = v->sym->try_as<LocalVarPtr>()) {
    // note, inside `if (v != null)` when `v` is used for writing, v->inferred_type is an original (declared_type)
    // (smart casts apply only for rvalue, not for lvalue, we don't check it here, it's a property of inferring)
    rvect = transition_to_target_type(std::move(rvect), code, var_ref->declared_type, v->inferred_type, v);
  }

  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_assignment(V<ast_assign> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  AnyExprV lhs = v->get_lhs();
  AnyExprV rhs = v->get_rhs();

  if (auto lhs_decl = lhs->try_as<ast_local_vars_declaration>()) {
    lhs = lhs_decl->get_expr();
  }

  std::vector rvect = pre_compile_let(code, lhs, rhs);
  // rvect is IR of rhs: for example, `nullablePoint = null` then rhs is size=1 (just `null`),
  // but the assignment worked correctly, because `null` was transitioned to `Point?` inside;
  // now also transition rhs is the assignment is used as an expression: `f(lhs = rhs)` or `a = b = rhs`
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_set_assign(V<ast_set_assign> v, CodeBlob& code, TypePtr target_type) {
  // for "lhs += rhs", compile lhs once as lvalue, read its current value from the resulting IR vars,
  // compute (current_value OP rhs), and assign the result back
  LValContext local_lval;
  std::vector ir_lhs = pre_compile_expr(v->get_lhs(), code, nullptr, &local_lval);
  vars_modification_watcher.trigger_callbacks(ir_lhs, v->get_lhs());

  std::vector ir_rhs = pre_compile_expr(v->get_rhs(), code, nullptr);

  std::vector<var_idx_t> args_vars;
  args_vars.insert(args_vars.end(), ir_lhs.begin(), ir_lhs.end());
  args_vars.insert(args_vars.end(), ir_rhs.begin(), ir_rhs.end());
  std::vector ir_result = code.create_tmp_var(v->inferred_type, v, "(set-assign)");
  code.add_call(v, ir_result, std::move(args_vars), v->fun_ref);

  code.add_let(v, ir_lhs, ir_result);   // += and others for math only, transition not required
  local_lval.after_let(std::move(ir_lhs), code, v);

  return transition_to_target_type(std::move(ir_result), code, target_type, v);
}

static std::vector<var_idx_t> process_binary_operator(V<ast_binary_operator> v, CodeBlob& code, TypePtr target_type) {
  TokenType t = v->tok;

  if (v->fun_ref) {   // almost all operators, fun_ref was assigned at type inferring
    std::vector args_vars = pre_compile_tensor(code, {v->get_lhs(), v->get_rhs()});
    std::vector rvect = code.create_tmp_var(v->inferred_type, v, "(binary-op)");
    code.add_call(v, rvect, std::move(args_vars), v->fun_ref);
    return transition_to_target_type(std::move(rvect), code, target_type, v);
  }
  if (t == tok_logical_and || t == tok_logical_or) {
    // do the following transformations:
    // a && b  ->  a ? (b != 0) : 0
    // a || b  ->  a ? 1 : (b != 0)
    AnyExprV v_0 = createV<ast_int_const>(v->range, td::make_refint(0), "0");
    v_0->mutate()->assign_inferred_type(TypeDataInt::create());
    AnyExprV v_1 = createV<ast_int_const>(v->range, td::make_refint(-1), "-1");
    v_1->mutate()->assign_inferred_type(TypeDataInt::create());
    auto v_b_ne_0 = createV<ast_binary_operator>(v->range, v->operator_range, "!=", tok_neq, v->get_rhs(), v_0);
    v_b_ne_0->mutate()->assign_inferred_type(TypeDataInt::create());
    v_b_ne_0->mutate()->assign_fun_ref(lookup_function("_!=_"));
    std::vector ir_cond = pre_compile_expr(v->get_lhs(), code, nullptr);
    tolk_assert(ir_cond.size() == 1);
    std::vector rvect = code.create_tmp_var(v->inferred_type, v, "(ternary)");
    Op& if_op = code.add_if_else(v, ir_cond);
    code.push_set_cur(if_op.block0);
    code.add_let(v, rvect, pre_compile_expr(t == tok_logical_and ? v_b_ne_0 : v_1, code, nullptr));
    code.close_pop_cur(v);
    code.push_set_cur(if_op.block1);
    code.add_let(v, rvect, pre_compile_expr(t == tok_logical_and ? v_0 : v_b_ne_0, code, nullptr));
    code.close_pop_cur(v);
    return transition_to_target_type(std::move(rvect), code, target_type, v);
  }
  if (t == tok_eq || t == tok_neq) {
    bool both_addr = v->get_lhs()->inferred_type->unwrap_alias()->try_as<TypeDataAddress>() && v->get_rhs()->inferred_type->unwrap_alias()->try_as<TypeDataAddress>();
    bool both_cell = v->get_lhs()->inferred_type->unwrap_alias()->try_as<TypeDataCell>() && v->get_rhs()->inferred_type->unwrap_alias()->try_as<TypeDataCell>();
    if (both_addr || both_cell) {
      FunctionPtr f_eq = both_addr ? lookup_function("slice.bitsEqual") : lookup_function("cell.hashEqual");
      std::vector args_vars = pre_compile_tensor(code, {v->get_lhs(), v->get_rhs()});
      std::vector rvect = code.create_tmp_var(TypeDataBool::create(), v, "(eq-operator)");
      code.add_call(v, rvect, {args_vars[0], args_vars[1]}, f_eq);
      if (t == tok_neq) {
        FunctionPtr not_sym = lookup_function("!b_");
        code.add_call(v, rvect, rvect, not_sym);
      }
      return transition_to_target_type(std::move(rvect), code, target_type, v);
    }
  }

  throw UnexpectedASTNodeKind(v, "process_binary_operator");
}

static std::vector<var_idx_t> process_unary_operator(V<ast_unary_operator> v, CodeBlob& code, TypePtr target_type) {
  std::vector rhs_vars = pre_compile_expr(v->get_rhs(), code, nullptr);
  std::vector rvect = code.create_tmp_var(v->inferred_type, v, "(unary-op)");
  code.add_call(v, rvect, std::move(rhs_vars), v->fun_ref);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_ternary_operator(V<ast_ternary_operator> v, CodeBlob& code, TypePtr target_type) {
  std::vector ir_cond = pre_compile_expr(v->get_cond(), code, nullptr);
  tolk_assert(ir_cond.size() == 1);
  std::vector rvect = code.create_tmp_var(v->inferred_type, v, "(ternary)");

  if (v->get_cond()->is_always_true) {
    code.add_let(v->get_when_true(), rvect, pre_compile_expr(v->get_when_true(), code, v->inferred_type));
  } else if (v->get_cond()->is_always_false) {
    code.add_let(v->get_when_false(), rvect, pre_compile_expr(v->get_when_false(), code, v->inferred_type));
  } else if (v->inferred_type->get_width_on_stack() == 1 && is_ternary_arg_trivial_for_condsel(v->get_when_true()) && is_ternary_arg_trivial_for_condsel(v->get_when_false())) {
    std::vector ir_true = pre_compile_expr(v->get_when_true(), code, v->inferred_type);
    std::vector ir_false = pre_compile_expr(v->get_when_false(), code, v->inferred_type);
    std::vector condsel_args = { ir_cond[0], ir_true[0], ir_false[0] };
    code.add_call(v, rvect, std::move(condsel_args), lookup_function("__condsel"));
  } else {
    Op& if_op = code.add_if_else(v, ir_cond);
    code.push_set_cur(if_op.block0);
    code.add_let(v->get_when_true(), rvect, pre_compile_expr(v->get_when_true(), code, v->inferred_type));
    code.close_pop_cur(v->get_when_true());
    code.push_set_cur(if_op.block1);
    code.add_let(v->get_when_false(), rvect, pre_compile_expr(v->get_when_false(), code, v->inferred_type));
    code.close_pop_cur(v->get_when_false());
  }

  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_null_coalesce_operator(V<ast_null_coalesce_operator> v, CodeBlob& code, TypePtr target_type) {
  std::vector rvect = code.create_tmp_var(v->inferred_type, v, "(null-coalesce)");
  std::vector ir_lhs = pre_compile_expr(v->get_lhs(), code);
  TypePtr lhs_type = v->get_lhs()->inferred_type;
  TypePtr without_null_type = calculate_type_subtract_rhs_type(lhs_type, TypeDataNullLiteral::create());

  if (lhs_type == TypeDataNullLiteral::create()) {
    // `null ?? rhs` — lhs is always null, no IF ELSE, just return rhs
    std::vector ir_rhs = pre_compile_expr(v->get_rhs(), code, v->inferred_type);
    code.add_let(v, rvect, std::move(ir_rhs));
  } else if (without_null_type == TypeDataNever::create()) {
    // `1 ?? rhs` — lhs can never be null, don't even call rhs, just return lhs
    ir_lhs = transition_to_target_type(std::move(ir_lhs), code, lhs_type, v->inferred_type, v);
    code.add_let(v, rvect, std::move(ir_lhs));
  } else {
    // regular situation: `lhs ?? rhs`, need a runtime branch
    std::vector ir_is_null = pre_compile_is_type(code, lhs_type, TypeDataNullLiteral::create(), ir_lhs, v, "(lhs-is-null)");
    Op& if_op = code.add_if_else(v, ir_is_null);
    {
      code.push_set_cur(if_op.block0);
      std::vector ir_rhs = pre_compile_expr(v->get_rhs(), code, v->inferred_type);
      code.add_let(v, rvect, std::move(ir_rhs));
      code.close_pop_cur(v);
    }
    {
      code.push_set_cur(if_op.block1);
      ir_lhs = transition_to_target_type(std::move(ir_lhs), code, lhs_type, without_null_type, v);
      ir_lhs = transition_to_target_type(std::move(ir_lhs), code, without_null_type, v->inferred_type, v);
      code.add_let(v, rvect, std::move(ir_lhs));
      code.close_pop_cur(v);
    }
  }

  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_cast_as_operator(V<ast_cast_as_operator> v, CodeBlob& code, TypePtr target_type) {
  TypePtr child_target_type = v->type_node->resolved_type;
  std::vector rvect = pre_compile_expr(v->get_expr(), code, child_target_type);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_is_type_operator(V<ast_is_type_operator> v, CodeBlob& code, TypePtr target_type) {
  TypePtr lhs_type = v->get_expr()->inferred_type;
  TypePtr cmp_type = v->type_node->resolved_type;
  bool is_null_check = cmp_type == TypeDataNullLiteral::create();    // `v == null`, not `v is T`
  tolk_assert(!cmp_type->try_as<TypeDataUnion>());  // `v is int|slice` is a type checker error

  std::vector ir_expr = pre_compile_expr(v->get_expr(), code, nullptr);
  std::vector ir_result = pre_compile_is_type(code, lhs_type, cmp_type, ir_expr, v, is_null_check ? "(is-null)" : "(is-type)");

  if (v->is_negated) {
    FunctionPtr not_sym = lookup_function("!b_");
    code.add_call(v, ir_result, ir_result, not_sym);
  }
  return transition_to_target_type(std::move(ir_result), code, target_type, v);
}

static std::vector<var_idx_t> process_not_null_operator(V<ast_not_null_operator> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  TypePtr expr_type = v->get_expr()->inferred_type;
  TypePtr without_null_type = calculate_type_subtract_rhs_type(expr_type, TypeDataNullLiteral::create());
  TypePtr child_target_type = without_null_type != TypeDataNever::create() ? without_null_type : expr_type;

  std::vector rvect = pre_compile_expr(v->get_expr(), code, child_target_type, lval_ctx);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_lazy_operator(V<ast_lazy_operator> v, CodeBlob& code, TypePtr target_type) {
  // `lazy Storage.fromSlice(s)` does not load anything here, it only saves a slice for future loads;
  // "future loads" are special auxiliary AST vertices "load x" that were inserted in pipe-lazy-load-insertions.cpp
  auto v_call = v->get_expr()->try_as<ast_function_call>();
  tolk_assert(v_call && v_call->fun_maybe);

  FunctionPtr called_f = v_call->fun_maybe;
  if (called_f->is_code_function()) {     // `lazy loadStorage()` is allowed, it contains just `return ...`, inline it here
    auto f_body = called_f->ast_root->as<ast_function_declaration>()->get_body()->as<ast_block_statement>();
    tolk_assert(f_body->size() == 1 && f_body->get_item(0)->kind == ast_return_statement);
    auto f_returns = f_body->get_item(0)->as<ast_return_statement>();
    v_call = f_returns->get_return_value()->try_as<ast_function_call>();
    tolk_assert(v_call && v_call->fun_maybe && v_call->fun_maybe->is_builtin());
    called_f = v_call->fun_maybe;
  }

  // only predefined built-in functions are allowed for lazy loading
  tolk_assert(called_f->is_builtin() && called_f->is_instantiation_of_generic_function());
  std::string_view f_name = called_f->base_fun_ref->name;
  std::vector ir_slice = code.create_var(TypeDataSlice::create(), v, "lazyS");
  bool has_passed_options = false;
  if (f_name == "T.fromSlice") {
    std::vector passed_slice = pre_compile_expr(v_call->get_arg(0)->get_expr(), code);
    code.add_let(v, ir_slice, std::move(passed_slice));
    has_passed_options = v_call->get_num_args() == 2;
  } else if (f_name == "T.fromCell") {
    std::vector ir_cell = pre_compile_expr(v_call->get_arg(0)->get_expr(), code);
    code.add_call(v, ir_slice, ir_cell, lookup_function("cell.beginParse"));
    has_passed_options = v_call->get_num_args() == 2;
  } else if (f_name == "Cell<T>.load") {
    std::vector ir_cell = pre_compile_expr(v_call->get_callee()->try_as<ast_dot_access>()->get_obj(), code);
    code.add_call(v, ir_slice, ir_cell, lookup_function("cell.beginParse"));
    has_passed_options = v_call->get_num_args() == 1;
  } else {
    tolk_assert(false);
  }

  // on `var p = lazy Point.fromSlice(s, options)`, save s and options (lazy_variable)
  AnyExprV v_options = has_passed_options ? v_call->get_arg(v_call->get_num_args() - 1)->get_expr() : called_f->parameters.back().default_value;
  std::vector ir_options = pre_compile_expr(v_options, code, called_f->parameters[1].declared_type);
  const LazyVariableLoadedState* lazy_variable = new LazyVariableLoadedState(v->dest_var_ref->declared_type, std::move(ir_slice), std::move(ir_options));
  code.lazy_variables.emplace_back(v->dest_var_ref, lazy_variable);

  // initially, all contents of `p` is filled by nulls, but before `p.x` or any other field usages,
  // they will be loaded by separate AST aux vertices;
  // same for unions: `val msg = lazy MyMsgUnion`, msg is N+1 nulls, but next lazy `match` will transition slots,
  // which will be filled by loads
  std::vector ir_null = code.create_tmp_var(TypeDataNullLiteral::create(), v, "(init-null)");
  code.add_call(v, ir_null, {}, lookup_function("__null"));
  std::vector ir_initial_nulls(v->dest_var_ref->declared_type->get_width_on_stack(), ir_null[0]);
  return transition_to_target_type(std::move(ir_initial_nulls), code, target_type, v);
}

static std::vector<var_idx_t> process_match_expression(V<ast_match_expression> v, CodeBlob& code, TypePtr target_type) {
  TypePtr subject_type = v->get_subject()->inferred_type;

  int n_arms = v->get_arms_count();
  std::vector ir_subj = pre_compile_expr(v->get_subject(), code, nullptr);
  std::vector ir_result = code.create_tmp_var(v->inferred_type, v, "(match-expression)");

  if (!n_arms) {    // `match (subject) {}`
    tolk_assert(v->is_statement());
    return {};
  }

  bool has_type_arm = false;    // it's either `match` by type (all arms are types covering all cases)
  bool has_expr_arm = false;    // or `match` by expression, patterns can't be mixed, checked earlier
  bool has_else_arm = false;    // if `else` exists, it's the last
  for (int i = 0; i < n_arms; ++i) {
    auto v_arm = v->get_arm(i);
    has_type_arm |= v_arm->pattern_kind == MatchArmKind::exact_type;
    has_expr_arm |= v_arm->pattern_kind == MatchArmKind::const_expression;
    has_else_arm |= v_arm->pattern_kind == MatchArmKind::else_branch;
  }

  // `else` is not allowed in `match` by type; this was not fired at type checking,
  // because it might have turned out to be a lazy match, where `else` is allowed;
  // if we are here, it's not a lazy match, it's a regular one (the lazy one is handled specially, in aux vertex)
  if (has_type_arm && has_else_arm) {
    err("`else` is not allowed in `match` by type; you should cover all possible types").fire(v->get_arm(n_arms - 1)->get_pattern_expr());
  }

  // in some cases, if `else` does not exist, we implicitly add it and "throw unreachable" there;
  int implicit_else_unreachable_throw = 0;
  // even though `match (enum)` covers all cases, if a stack is malformed (holds a wrong integer), we throw
  if (v->is_exhaustive && has_expr_arm && !has_else_arm && subject_type->unwrap_alias()->try_as<TypeDataEnum>()) {
    implicit_else_unreachable_throw = 5;    // "integer is out of range"
  }

  // how to compare subject and branches for `match` expression, similar to operator `==` which can handle non-integers
  FunctionPtr eq_fn = lookup_function("_==_");      // for int/bool/enum                                                                   
  if (subject_type->unwrap_alias()->try_as<TypeDataAddress>()) {
    eq_fn = lookup_function("slice.bitsEqual");
  }

  // example 1 (exhaustive): `match (v) { int => ... slice => ... builder => ... }`
  // IF is int { ... } ELSE { IF is slice { ... } ELSE { ... } }
  // example 2 (exhaustive): `match (v) { -1 => ... 0 => ... else => ... }`
  // IF == -1 { ... } ELSE { IF == 0 { ... } ELSE { ... } }
  // example 3 (not exhaustive): `match (v) { -1 => ... 0 => ... 1 => ... }`
  // IF == -1 { ... } ELSE { IF == 0 { ... } ELSE { IF == 1 { ... } } }
  // example 4 (with implicit else): `match (role) { User => ... Admin => ... }`
  // IF == 0 { ... } ELSE { IF == 1 { ... } ELSE { 5 THROW } }
  for (int i = 0; i < n_arms; ++i) {
    auto v_ith_arm = v->get_arm(i);
    // if we're inside `else` of inside `builder` (example 1), no more checks, we're inside last else
    Op* if_op = nullptr;
    bool inside_last_branch = i == n_arms - 1 && v->is_exhaustive && implicit_else_unreachable_throw == 0;
    if (!inside_last_branch) {
      // construct "IF enter_ith_branch"
      std::vector<var_idx_t> eq_ith_ir_idx;
      if (has_type_arm) {     // `v is int`, `v is slice`, etc. (type before =>)
        TypePtr cmp_type = v_ith_arm->pattern_type_node->resolved_type;
        tolk_assert(!cmp_type->unwrap_alias()->try_as<TypeDataUnion>());  // `match` over `int|slice` is a type checker error
        eq_ith_ir_idx = pre_compile_is_type(code, subject_type, cmp_type, ir_subj, v_ith_arm, "(arm-cond-eq)");
      } else {                // `v == 0`, `v == Role.User`, etc. (expr before =>)
        std::vector ith_ir_idx = pre_compile_expr(v_ith_arm->get_pattern_expr(), code);
        tolk_assert(ir_subj.size() == 1 && ith_ir_idx.size() == 1);
        eq_ith_ir_idx = code.create_tmp_var(TypeDataBool::create(), v_ith_arm, "(arm-cond-eq)");
        code.add_call(v_ith_arm, eq_ith_ir_idx, {ir_subj[0], ith_ir_idx[0]}, eq_fn);
      }
      if_op = &code.add_if_else(v_ith_arm, std::move(eq_ith_ir_idx));
      code.push_set_cur(if_op->block0);
    }

    if (v->is_statement()) {
      pre_compile_expr(v_ith_arm->get_body(), code);
      if (v == stmt_before_immediate_return) {
        code.add_return(v_ith_arm);
      }
    } else {
      std::vector arm_ir_idx = pre_compile_expr(v_ith_arm->get_body(), code, v->inferred_type);
      code.add_let(v, ir_result, std::move(arm_ir_idx));
    }

    if (!inside_last_branch) {
      code.close_pop_cur(v);
      code.push_set_cur(if_op->block1);    // open ELSE
    }
  }

  // we are inside last ELSE
  // if it was user-defined, we've inserted its body already
  // if it's auto-generated "unreachable", insert "N THROW"
  if (implicit_else_unreachable_throw) {
    code.add_call(v, {}, {code.create_int(v, implicit_else_unreachable_throw, "(throw-else)")}, lookup_function("__throw"));
  }

  // close all outer IFs
  int depth = n_arms - v->is_exhaustive + (implicit_else_unreachable_throw != 0);
  for (int i = 0; i < depth; ++i) {
    code.close_pop_cur(v);
  }

  return transition_to_target_type(std::move(ir_result), code, target_type, v);
}

static std::vector<var_idx_t> process_dot_access(V<ast_dot_access> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  // it's NOT a method call `t.tupleSize()` (since such cases are handled by process_function_call)
  // it's `t.0`, `getUser().id`, and `t.tupleSize` (as a reference, not as a call)
  if (v->is_target_indexed_access() || v->is_target_struct_field()) {
    TypePtr obj_type = v->get_obj()->inferred_type->unwrap_alias();
    // `user.id`; internally, a struct (an object) is a tensor
    if (const TypeDataStruct* t_struct = obj_type->try_as<TypeDataStruct>()) {
      StructFieldPtr field_ref = std::get<StructFieldPtr>(v->target);
      // handle `lazyPoint.x`, assert that slot for "x" is loaded (ensure lazy-loading correctness);
      // same for `val msg = lazy MyMsgUnion; match(...) msg.field` inside a specific variant (struct_ref)
      if (const LazyVariableLoadedState* lazy_variable = code.get_lazy_variable(v->get_obj())) {
        lazy_variable->assert_field_loaded(t_struct->struct_ref, field_ref);
      }
      std::vector lhs_vars = pre_compile_expr(v->get_obj(), code, nullptr, lval_ctx);
      int stack_width = field_ref->declared_type->get_width_on_stack();
      int stack_offset = calc_offset_on_stack(t_struct->struct_ref, field_ref->field_idx);
      std::vector rvect(lhs_vars.begin() + stack_offset, lhs_vars.begin() + stack_offset + stack_width);
      if (lval_ctx && v->get_obj()->inferred_type->unwrap_alias()->try_as<TypeDataShapedTuple>()) {
        tolk_assert(is_valid_lvalue_path(v));
        lval_ctx->capture_tensor_index_modification(v->get_obj(), stack_offset, rvect);
      }
      // an object field might be smart cast at this point, for example we're in `if (user.t != null)`
      // it means that we must drop the null flag (if `user.t` is a tensor), or maybe perform other stack transformations
      // (from original rvect = (vars of user.t) to fit smart cast)
      rvect = transition_to_target_type(std::move(rvect), code, field_ref->declared_type, v->inferred_type, v->get_obj());
      return transition_to_target_type(std::move(rvect), code, target_type, v);
    }
    // `tensorVar.0`
    if (const TypeDataTensor* t_tensor = obj_type->try_as<TypeDataTensor>()) {
      int index_at = std::get<int>(v->target);
      std::vector lhs_vars = pre_compile_expr(v->get_obj(), code, nullptr, lval_ctx);
      // since a tensor of N elems are N vars on a stack actually, calculate offset
      int stack_width = t_tensor->items[index_at]->get_width_on_stack();
      int stack_offset = calc_offset_on_stack(t_tensor, index_at);
      std::vector rvect(lhs_vars.begin() + stack_offset, lhs_vars.begin() + stack_offset + stack_width);
      if (lval_ctx && v->get_obj()->inferred_type->unwrap_alias()->try_as<TypeDataShapedTuple>()) {
        tolk_assert(is_valid_lvalue_path(v));
        lval_ctx->capture_tensor_index_modification(v->get_obj(), stack_offset, rvect);
      }
      // a tensor index might be smart cast at this point, for example we're in `if (t.1 != null)`
      rvect = transition_to_target_type(std::move(rvect), code, t_tensor->items[index_at], v->inferred_type, v->get_obj());
      return transition_to_target_type(std::move(rvect), code, target_type, v);
    }
    // `tupleShaped.0`
    if (const TypeDataShapedTuple* t_shaped = obj_type->try_as<TypeDataShapedTuple>()) {
      int index_at = std::get<int>(v->target);
      std::vector tuple_ir_idx = pre_compile_expr(v->get_obj(), code, nullptr, lval_ctx);
      std::vector index_ir_idx = code.create_tmp_var(TypeDataInt::create(), v->get_identifier(), "(tuple-idx)");
      code.add_int_const(v, index_ir_idx, td::make_refint(index_at));
      std::vector rvect = code.create_tmp_var(t_shaped->items[index_at], v, "(tuple-field)");
      code.add_call(v, rvect, {tuple_ir_idx[0], index_ir_idx[0]}, lookup_function("array<T>.get"));
      if (lval_ctx) {
        tolk_assert(is_valid_lvalue_path(v));
        lval_ctx->capture_shaped_tuple_index_modification(v->get_obj(), index_at, rvect);
      }
      // a tuple index might be smart cast at this point, for example we're in `if (shapeOfPoints.1 != null)`
      rvect = transition_to_target_type(std::move(rvect), code, t_shaped->items[index_at], v->inferred_type, v->get_obj());
      return transition_to_target_type(std::move(rvect), code, target_type, v);
    }
    tolk_assert(false);
  }
  // `Color.Red`
  if (v->is_target_enum_member()) {
    // all enums are integers, and their integer values have already been assigned or auto-calculated
    EnumMemberPtr member_ref = std::get<EnumMemberPtr>(v->target);
    tolk_assert(!member_ref->computed_value.is_null());
    std::vector enum_ir_idx = code.create_tmp_var(TypeDataInt::create(), v->get_identifier(), "(enum-member)");
    code.add_int_const(v, enum_ir_idx, member_ref->computed_value);
    return transition_to_target_type(std::move(enum_ir_idx), code, target_type, v);
  }

  // okay, v->target refs a function, like `obj.method`, filled at type inferring
  // (currently, nothing except a global function can be referenced, no object-scope methods exist)
  FunctionPtr fun_ref = std::get<FunctionPtr>(v->target);
  tolk_assert(fun_ref);
  std::vector rvect = pre_compile_symbol(fun_ref, code, v, lval_ctx);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_function_call(V<ast_function_call> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  // v is `globalF(args)` / `globalF<int>(args)` / `obj.method(args)` / `local_var(args)` / `getF()(args)`
  FunctionPtr fun_ref = v->fun_maybe;
  if (!fun_ref) {
    // it's `local_var(args)`, treat args like a tensor:
    // 1) when variables are modified like `local_var(x, x += 2, x)`, regular mechanism of watching automatically works
    // 2) when `null` is passed to `(int, int)?`, or any other type transitions, it automatically works
    std::vector tfunc = pre_compile_expr(v->get_callee(), code, nullptr);
    tolk_assert(tfunc.size() == 1);
    std::vector<AnyExprV> args;
    args.reserve(v->get_num_args());
    for (int i = 0; i < v->get_num_args(); ++i) {
      args.push_back(v->get_arg(i)->get_expr());
    }
    std::vector<TypePtr> params_types = v->get_callee()->inferred_type->unwrap_alias()->try_as<TypeDataFunCallable>()->params_types;
    std::vector args_vars = pre_compile_tensor(code, args, nullptr, params_types);
    args_vars.push_back(tfunc[0]);
    std::vector rvect = code.create_tmp_var(v->inferred_type, v, "(call-ind)");
    if (args_vars.size() >= 254 || rvect.size() >= 254) {
      err("too many arguments on a stack for an indirect call").fire(v->get_callee());
    }
    code.add_indirect_invoke(v, rvect, std::move(args_vars));
    return transition_to_target_type(std::move(rvect), code, target_type, v);
  }
  // `ton("0.05")` and others, we even don't need to calculate ir_idx for arguments, just replace with constexpr
  if (fun_ref->is_compile_time_const_val()) {
    ConstValExpression value = eval_expression_if_const_or_fire(v);
    auto [type, rvect] = pre_compile_constant_expression(value, code, v);
    return transition_to_target_type(std::move(rvect), code, target_type, v);
  }

  // fill args for evaluation: dot object + passed arguments + parameters defaults if not all passed
  int delta_self = v->get_self_obj() != nullptr;
  std::vector<AnyExprV> args;
  args.reserve(fun_ref->get_num_params());
  if (delta_self) {
    args.push_back(v->get_self_obj());
  }
  for (int i = 0; i < v->get_num_args(); ++i) {
    args.push_back(v->get_arg(i)->get_expr());
  }
  // default values
  for (int i = delta_self + v->get_num_args(); i < fun_ref->get_num_params(); ++i) {
    LocalVarPtr param_ref = &fun_ref->get_param(i);
    tolk_assert(param_ref->has_default_value());
    AnyExprV dv = param_ref->default_value;
    if (auto dv_call = dv->try_as<ast_function_call>()) {
      // reflect.sourceLocation() as default — create a new AST vertex with range = call site
      if (dv_call->fun_maybe->name == "reflect.sourceLocation" || dv_call->fun_maybe->name == "reflect.sourceLocationAsString") {
        auto dv_new = createV<ast_function_call>(v->range, dv_call->get_callee(), dv_call->get_arg_list());
        dv_new->mutate()->assign_fun_ref(dv_call->fun_maybe, dv_call->dot_obj_is_self);
        dv_new->mutate()->assign_inferred_type(dv_call->inferred_type);
        dv = dv_new;
      }
    }
    args.push_back(dv);
  }

  // if fun_ref has asm arg_order, maybe it's safe to swap arguments here (to put them onto a stack in the right way);
  // (if it's not safe, arguments are evaluated left-to-right, involving stack transformations later)
  std::vector<TypePtr> params_types = fun_ref->inferred_full_type->try_as<TypeDataFunCallable>()->params_types;
  bool arg_order_already_equals_asm = false;
  std::vector<int> rev_arg_order;     // either empty or a reversed mapping filled below
  if (!fun_ref->arg_order.empty() && CheckReorderingForAsmArgOrderIsSafeVisitor::is_safe_to_reorder(v)) {
    std::vector<AnyExprV> new_args(fun_ref->get_num_params());
    std::vector<TypePtr> new_params_types(fun_ref->get_num_params());
    rev_arg_order.resize(fun_ref->get_num_params());
    for (int i = 0; i < fun_ref->get_num_params(); ++i) {
      int orig_i = fun_ref->arg_order[i];
      new_args[i] = args[orig_i];
      new_params_types[i] = params_types[orig_i];
      rev_arg_order[orig_i] = i;    // having arg_order=(c,a,b)=[2,0,1], calc rev_arg_order=[1,2,0]
    }
    args = std::move(new_args);
    params_types = std::move(new_params_types);
    arg_order_already_equals_asm = true;
  }

  // every `mutate` parameter (`mutate self` also) is evaluated in an LVal context;
  // for instance, `f(mutate tupleShaped.0)` will trigger SETINDEX 0 into a tuple after f() returns
  LValContext local_lval;
  // use a separate LValContext for `self`
  // e.g. `a.mix(mutate b.inc().v)` — inner `b.inc()` sets mutated_self_obj to `b`,
  // but we want `obj_leftmost` below to be `a` for `a.mix()`
  LValContext self_lval;
  bool has_mutate_self = delta_self && fun_ref->does_mutate_self();
  std::vector<LValContext*> lval_ctx_for_arg(fun_ref->get_num_params(), nullptr);
  for (int i = 0; i < fun_ref->get_num_params(); ++i) {
    if (fun_ref->parameters[i].is_mutate_parameter()) {
      int orig_i = rev_arg_order.empty() ? i : rev_arg_order[i];
      lval_ctx_for_arg[orig_i] = has_mutate_self && i == 0 ? &self_lval : &local_lval;
    }
  }

  // function arguments are like a tensor
  // for instance, `f(x, x += 2, x)` requires inserting a tmp variable
  std::vector vars_per_arg = pre_compile_tensor_inner(code, args, params_types, lval_ctx_for_arg);

  // detect what a function really returns from the stack perspective;
  // for instance, `fun f(mutate x: int): slice` puts `(int, slice)` onto a stack
  TypePtr op_call_type = v->inferred_type;
  if (v->get_self_obj() && fun_ref->does_return_self() && !fun_ref->does_mutate_self()) {
    op_call_type = TypeDataVoid::create();    // `return self` actually puts nothing onto a stack
  }
  if (fun_ref->has_mutate_params()) {
    std::vector<TypePtr> types_list;
    for (int i = 0; i < delta_self + v->get_num_args(); ++i) {
      if (fun_ref->parameters[i].is_mutate_parameter()) {
        types_list.push_back(fun_ref->parameters[i].declared_type);
      }
    }
    bool self_already_added = v->get_self_obj() && fun_ref->does_return_self();
    if (!self_already_added) {
      types_list.push_back(v->inferred_type);
    }
    op_call_type = TypeDataTensor::create(std::move(types_list));
  }

  std::vector<var_idx_t> args_vars;
  for (const std::vector<var_idx_t>& list : vars_per_arg) {
    args_vars.insert(args_vars.end(), list.cbegin(), list.cend());
  }
  std::vector<var_idx_t> rvect;
  if (fun_ref->is_compile_time_special_gen()) {
    rvect = gen_compile_time_code_instead_of_fun_call(code, v, vars_per_arg);
  } else if (fun_ref->is_inlined_in_place() && fun_ref->is_code_function()) {
    rvect = gen_inline_fun_call_in_place(code, op_call_type, v, v->fun_maybe, v->get_self_obj(), v == stmt_before_immediate_return, vars_per_arg);
  } else {
    rvect = code.create_tmp_var(op_call_type, v, "(fun-call)");
    code.add_call(v, rvect, std::move(args_vars), fun_ref, arg_order_already_equals_asm);
  }

  // `x.inc().inc()` — mutating chaining was called in lval context, here we get `x` expression in an outer call;
  // if it's a global variable or a tuple index, re-evaluate it to bind modifications capturing
  AnyExprV obj_leftmost = self_lval.get_mutated_self_obj();   // nullptr if none
  if (fun_ref->does_mutate_self() && obj_leftmost) {
    tolk_assert(is_valid_lvalue_path(obj_leftmost));
    int orig_self_i = rev_arg_order.empty() ? 0 : rev_arg_order[0];
    vars_per_arg[orig_self_i] = pre_compile_expr(obj_leftmost, code, fun_ref->parameters[0].declared_type, &self_lval);
  }

  // `f(mutate x: int): slice` had put `(int, slice)` onto a stack, leave only `slice` (an actual return)
  if (fun_ref->has_mutate_params()) {
    std::vector<var_idx_t> left;
    for (int i = 0; i < delta_self + v->get_num_args(); ++i) {
      if (fun_ref->parameters[i].is_mutate_parameter()) {
        int orig_i = rev_arg_order.empty() ? i : rev_arg_order[i];
        left.insert(left.end(), vars_per_arg[orig_i].begin(), vars_per_arg[orig_i].end());
      }
    }
    int n_left_size = static_cast<int>(left.size());
    vars_modification_watcher.trigger_callbacks(left, v);
    code.add_let(v, left, std::vector(rvect.begin(), rvect.begin() + n_left_size));
    local_lval.after_let(std::vector(left), code, v);
    self_lval.after_let(std::move(left), code, v);
    rvect = std::vector(rvect.begin() + n_left_size, rvect.end());
  }

  // `beginCell().storeUint()` / `sb.append()` / etc. — dot call for methods returning `self`
  TypePtr rvect_type = v->inferred_type;
  if (v->get_self_obj() && fun_ref->does_return_self()) {
    int orig_self_i = rev_arg_order.empty() ? 0 : rev_arg_order[0];
    rvect = vars_per_arg[orig_self_i];
    if (fun_ref->does_mutate_self()) {
      if (obj_leftmost == nullptr && is_valid_lvalue_path(v->get_self_obj())) {
        obj_leftmost = v->get_self_obj();
      }
      if (lval_ctx && obj_leftmost) {
        lval_ctx->set_mutated_self_obj(obj_leftmost);
      }
    }
    rvect_type = fun_ref->parameters[0].declared_type;
  }

  TypePtr dest_type = target_type ? target_type : v->inferred_type;
  return transition_rvect_to_runtime_type(std::move(rvect), code, rvect_type, dest_type, v);
}

static std::vector<var_idx_t> process_braced_expression(V<ast_braced_expression> v, CodeBlob& code, TypePtr target_type) {
  // generally, `{ ... }` is a block statement not returning a value; it's used to represent `match` braced arms;
  // unless it's a special vertex "braced expression" (currently, only `match` arms)
  std::vector<var_idx_t> implicit_rvect;
  for (AnyV item : v->get_block_statement()->get_items()) {
    if (auto v_return = item->try_as<ast_braced_yield_result>()) {
      tolk_assert(implicit_rvect.empty());
      implicit_rvect = pre_compile_expr(v_return->get_expr(), code);
    } else {
      process_any_statement(item, code);
    }
  }
  return transition_to_target_type(std::move(implicit_rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_tensor(V<ast_tensor> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  // tensor is compiled "as is", for example `(1, null)` occupies 2 slots
  // and if assigned/passed to something other, like `(int, (int,int)?)`, a whole tensor is transitioned, it works
  std::vector rvect = pre_compile_tensor(code, v->get_items(), lval_ctx);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_square_brackets(V<ast_square_brackets> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  if (v->is_lvalue) {       // todo some time, make "var (a, [b,c]) = (1, [2,3])" work
    err("[...] can not be used as lvalue here").fire(v);
  }
  std::vector rvect = code.create_tmp_var(v->inferred_type, v, "(pack-tuple)");

  // note that for every constructor of `[...]` (array, lisp_list, etc.) we still make a shape at low-level,
  // then this shape is transitioned to a constructor type;
  // for example, having `lisp_list<int> [1,2]`, make a shape (TVM tuple) [1,2] at first;
  // we need this to handle nested modifications, like `[x, x+=2, x]` via "watched vars" with IR copying
  std::vector<TypePtr> items_types;
  items_types.reserve(v->size());
  for (int i = 0; i < v->size(); ++i) {
    items_types.push_back(v->get_item(i)->inferred_type);
  }
  TypePtr t_shaped = TypeDataShapedTuple::create(std::move(items_types));

  std::vector<TypePtr> target_types(v->size(), TypeDataUnknown::create());
  std::vector res = pre_compile_tensor(code, v->get_items(), nullptr, target_types);
  code.add_to_tuple(v, rvect, std::move(res));
  rvect = transition_to_target_type(std::move(rvect), code, t_shaped, v->inferred_type, v);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_object_literal_shuffled(V<ast_object_literal> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  // creating an object like `Point { y: getY(), x: getX() }`, where fields order doesn't match declaration;
  // as opposed to a non-shuffled version `{x:..., y:...}`, we should at first evaluate fields as they created,
  // and then to place them in a correct order
  std::vector<AnyExprV> tensor_items;   // create a tensor of literal fields values
  std::vector<TypePtr> target_types;
  tensor_items.reserve(v->get_body()->get_num_fields());
  target_types.reserve(v->get_body()->get_num_fields());
  for (int i = 0; i < v->get_body()->get_num_fields(); ++i) {
    auto v_field = v->get_body()->get_field(i);
    StructFieldPtr field_ref = v->struct_ref->find_field(v_field->get_field_name());
    tensor_items.push_back(v_field->get_init_val());
    target_types.push_back(field_ref->declared_type);
  }
  std::vector literal_rvect = pre_compile_tensor(code, tensor_items, lval_ctx, target_types);

  std::vector rvect = code.create_tmp_var(TypeDataStruct::create(v->struct_ref), v, "(object)");
  int stack_offset = 0;
  for (StructFieldPtr field_ref : v->struct_ref->fields) {
    int stack_width = field_ref->declared_type->get_width_on_stack();
    std::vector field_rvect(rvect.begin() + stack_offset, rvect.begin() + stack_offset + stack_width);
    stack_offset += stack_width;

    int tensor_offset = 0;
    bool exists_in_literal = false;
    for (int i = 0; i < v->get_body()->get_num_fields(); ++i) {
      auto v_field = v->get_body()->get_field(i);
      int tensor_item_width = v_field->field_ref->declared_type->get_width_on_stack();
      if (v_field->get_field_name() == field_ref->name) {
        exists_in_literal = true;
        std::vector literal_field_rvect(literal_rvect.begin() + tensor_offset, literal_rvect.begin() + tensor_offset + tensor_item_width);
        code.add_let(v, std::move(field_rvect), std::move(literal_field_rvect));
        break;
      }
      tensor_offset += tensor_item_width;
    }
    if (exists_in_literal || field_ref->declared_type == TypeDataVoid::create()) {
      continue;
    }

    tolk_assert(field_ref->has_default_value());
    std::vector def_rvect = pre_compile_expr(field_ref->default_value, code, field_ref->declared_type);
    code.add_let(v, std::move(field_rvect), std::move(def_rvect));
  }

  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_object_literal(V<ast_object_literal> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  // an object (an instance of a struct) is actually a tensor at low-level
  // for example, `struct User { id: int; name: slice; }` occupies 2 slots
  // fields of a tensor are placed in order of declaration (in a literal they might be shuffled)
  bool are_fields_shuffled = false;
  for (int i = 1; i < v->get_body()->get_num_fields(); ++i) {
    StructFieldPtr field_ref = v->struct_ref->find_field(v->get_body()->get_field(i)->get_field_name());
    StructFieldPtr prev_field_ref = v->struct_ref->find_field(v->get_body()->get_field(i - 1)->get_field_name());
    are_fields_shuffled |= prev_field_ref->field_idx > field_ref->field_idx;
  }

  // if fields are created {y,x} (not {x,y}), maybe, it's nevertheless safe to evaluate them as {x,y};
  // for example, if they are just constants, calls to pure non-mutating functions, etc.;
  // generally, rules of "can we evaluate {x,y} instead of {y,x}" follows the same logic
  // as passing of calling `f(x,y)` with asm arg_order, is it safe to avoid SWAP
  if (are_fields_shuffled && !CheckReorderingForAsmArgOrderIsSafeVisitor::is_safe_to_reorder(v->get_body())) {
    // okay, we have `{y: getY(), x: getX()}` / `{y: v += 1, x: v}`, evaluate them in created order
    return process_object_literal_shuffled(v, code, target_type, lval_ctx);
  }

  std::vector<AnyExprV> tensor_items;
  std::vector<TypePtr> target_types;
  tensor_items.reserve(v->struct_ref->get_num_fields());
  target_types.reserve(v->struct_ref->get_num_fields());
  for (StructFieldPtr field_ref : v->struct_ref->fields) {
    AnyExprV v_init_val = nullptr;
    for (int i = 0; i < v->get_body()->get_num_fields(); ++i) {
      auto v_field = v->get_body()->get_field(i);
      if (v_field->get_field_name() == field_ref->name) {
        v_init_val = v_field->get_init_val();
        break;
      }
    }
    if (!v_init_val) {
      if (field_ref->declared_type == TypeDataVoid::create()) {
        continue;   // field of `void` type can be missed out of object literal (useful in generics defaults)
      }             // (it occupies 0 slots, nothing is assignable to it — like this field is missing from a struct)
      tolk_assert(field_ref->has_default_value());
      v_init_val = field_ref->default_value;
    }
    tensor_items.push_back(v_init_val);
    target_types.push_back(field_ref->declared_type);
  }
  std::vector rvect = pre_compile_tensor(code, tensor_items, lval_ctx, target_types);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_lambda_fun(V<ast_lambda_fun> v, CodeBlob& code, TypePtr target_type) {
  tolk_assert(v->lambda_ref);
  std::vector rvect = code.create_tmp_var(v->lambda_ref->inferred_full_type, v, "(glob-var-lambda)");
  code.add_read_glob_var(v, rvect, v->lambda_ref);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_int_const(V<ast_int_const> v, CodeBlob& code, TypePtr target_type) {
  std::vector rvect = code.create_tmp_var(v->inferred_type, v, "(int-const)");
  code.add_int_const(v, rvect, v->intval);
  // here, like everywhere, even for just `int`, there might be a potential transition due to union types
  // example: passing `1` to `int | slice` puts actually "1 5" on a stack (1 for value, 5 for UTag = type_id of `int`)
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_string_const(V<ast_string_const> v, CodeBlob& code, TypePtr target_type) {
  std::vector rvect = code.create_tmp_var(v->inferred_type, v, "(str-const)");
  code.add_string_const(v, rvect, v->str_val);   // if long, will be transfomed to snake
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_bool_const(V<ast_bool_const> v, CodeBlob& code, TypePtr target_type) {
  std::vector rvect = code.create_tmp_var(v->inferred_type, v, "(bool-const)");
  code.add_call(v, rvect, {}, lookup_function(v->bool_val ? "__true" : "__false"));
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_null_keyword(V<ast_null_keyword> v, CodeBlob& code, TypePtr target_type) {
  std::vector rvect = code.create_tmp_var(v->inferred_type, v, "(null-literal)");
  code.add_call(v, rvect, {}, lookup_function("__null"));
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_local_var(V<ast_local_var_lhs> v, CodeBlob& code, TypePtr target_type) {
  tolk_assert(v->var_ref->ir_idx.empty());
  v->var_ref->mutate()->assign_ir_idx(code.create_var(v->inferred_type, v, v->var_ref->name));
  std::vector rvect = v->var_ref->ir_idx;
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_local_vars_declaration(V<ast_local_vars_declaration>, CodeBlob&) {
  // it can not appear as a standalone expression
  // `var ... = rhs` is handled by ast_assign
  // `var rhs: int lateinit` is ast_local_var_lhs
  tolk_assert(false);
}

static std::vector<var_idx_t> process_underscore(V<ast_underscore> v, CodeBlob& code) {
  // when _ is used as left side of assignment, like `(cs, _) = cs.loadAndReturn()`
  return code.create_tmp_var(v->inferred_type, v, "(underscore)");
}

static std::vector<var_idx_t> process_empty_expression(V<ast_empty_expression> v, CodeBlob& code, TypePtr target_type) {
  std::vector<var_idx_t> empty_rvect;
  return transition_to_target_type(std::move(empty_rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_artificial_aux_vertex(V<ast_artificial_aux_vertex> v, CodeBlob& code, TypePtr target_type) {
  AnyExprV wrapped = v->get_wrapped_expr();

  // aux "load x"; example: `var p = lazy Point.fromSlice(s); aux "load x"; return p.x`
  if (const auto* data = dynamic_cast<const AuxData_LazyObjectLoadFields*>(v->aux_data)) {
    const LazyVariableLoadedState* lazy_variable = code.get_lazy_variable(data->var_ref);
    tolk_assert(lazy_variable);

    std::vector ir_obj = data->var_ref->ir_idx;   // loading will update stack slots of `p`
    TypePtr t_orig = data->var_ref->declared_type;

    if (data->field_ref) {        // extract a field from a whole lazy variable
      tolk_assert(lazy_variable->is_struct());
      int stack_offset = calc_offset_on_stack(lazy_variable->loaded_state.original_struct, data->field_ref->field_idx);
      int stack_width = data->field_ref->declared_type->get_width_on_stack();
      ir_obj = std::vector(ir_obj.begin() + stack_offset, ir_obj.begin() + stack_offset + stack_width);
      t_orig = data->field_ref->declared_type;
    }

    if (data->union_variant) {    // extract a variant from a union (a union variable or a union field of a struct)
      ir_obj = transition_to_target_type(std::move(ir_obj), code, t_orig, data->union_variant, wrapped);
    }

    // `load_info` contains instructions to skip, load, save tail, etc.;
    // it generates LETs to ir_obj, so stack slots of lazy_variable will contain loaded data
    generate_lazy_struct_from_slice(code, wrapped, lazy_variable, data->load_info, ir_obj);
    return transition_to_target_type({}, code, target_type, wrapped);
  }

  // aux "match(lazyUnion)" / aux "match(obj.lastUnionField)"
  if (const auto* data = dynamic_cast<const AuxData_LazyMatchForUnion*>(v->aux_data)) {
    V<ast_match_expression> v_match = wrapped->as<ast_match_expression>();
    pre_compile_expr(v_match->get_subject(), code, nullptr);

    const LazyVariableLoadedState* lazy_variable = code.get_lazy_variable(data->var_ref);
    tolk_assert(lazy_variable);
    TypePtr t_union = data->field_ref ? data->field_ref->declared_type : data->var_ref->declared_type;

    std::vector<LazyMatchOptions::MatchBlock> match_blocks;
    match_blocks.reserve(v_match->get_arms_count());
    for (int i = 0; i < v_match->get_arms_count(); ++i) {
      auto v_arm = v_match->get_arm(i);
      TypePtr arm_variant = nullptr;
      if (v_arm->pattern_kind == MatchArmKind::exact_type) {
        arm_variant = v_arm->pattern_type_node->resolved_type;
      } else {
        tolk_assert(v_arm->pattern_kind == MatchArmKind::else_branch);   // `else` allowed in a lazy match
      }
      match_blocks.emplace_back(LazyMatchOptions::MatchBlock{arm_variant, v_arm->get_body(), v_arm->get_body()->inferred_type});
    }

    LazyMatchOptions options = {
      .match_expr_type = v->inferred_type,
      .is_statement = v_match->is_statement(),
      .add_return_to_all_arms = v == stmt_before_immediate_return,
      .match_blocks = std::move(match_blocks),
    };

    // it will generate match by a slice prefix, and for each `match` arm, invoke pre_compile_expr(),
    // which contains "aux load" particularly
    std::vector ir_match = generate_lazy_match_for_union(code, v_match, t_union, lazy_variable, options);
    return transition_to_target_type(std::move(ir_match), code, target_type, wrapped);
  }

  if (const auto* data = dynamic_cast<const AuxData_OnInternalMessage_getField*>(v->aux_data)) {
    std::vector rvect = data->generate_get_InMessage_field(code, wrapped);
    return transition_to_target_type(std::move(rvect), code, target_type, wrapped);
  }

  tolk_assert(false);
}

std::vector<var_idx_t> pre_compile_expr(AnyExprV v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  switch (v->kind) {
    case ast_reference:
      return process_reference(v->as<ast_reference>(), code, target_type, lval_ctx);
    case ast_assign:
      return process_assignment(v->as<ast_assign>(), code, target_type, lval_ctx);
    case ast_set_assign:
      return process_set_assign(v->as<ast_set_assign>(), code, target_type);
    case ast_binary_operator:
      return process_binary_operator(v->as<ast_binary_operator>(), code, target_type);
    case ast_unary_operator:
      return process_unary_operator(v->as<ast_unary_operator>(), code, target_type);
    case ast_ternary_operator:
      return process_ternary_operator(v->as<ast_ternary_operator>(), code, target_type);
    case ast_null_coalesce_operator:
      return process_null_coalesce_operator(v->as<ast_null_coalesce_operator>(), code, target_type);
    case ast_cast_as_operator:
      return process_cast_as_operator(v->as<ast_cast_as_operator>(), code, target_type);
    case ast_is_type_operator:
      return process_is_type_operator(v->as<ast_is_type_operator>(), code, target_type);
    case ast_not_null_operator:
      return process_not_null_operator(v->as<ast_not_null_operator>(), code, target_type, lval_ctx);
    case ast_lazy_operator:
      return process_lazy_operator(v->as<ast_lazy_operator>(), code, target_type);
    case ast_match_expression:
      return process_match_expression(v->as<ast_match_expression>(), code, target_type);
    case ast_dot_access:
      return process_dot_access(v->as<ast_dot_access>(), code, target_type, lval_ctx);
    case ast_function_call:
      return process_function_call(v->as<ast_function_call>(), code, target_type, lval_ctx);
    case ast_braced_expression:
      return process_braced_expression(v->as<ast_braced_expression>(), code, target_type);
    case ast_tensor:
      return process_tensor(v->as<ast_tensor>(), code, target_type, lval_ctx);
    case ast_square_brackets:
      return process_square_brackets(v->as<ast_square_brackets>(), code, target_type, lval_ctx);
    case ast_object_literal:
      return process_object_literal(v->as<ast_object_literal>(), code, target_type, lval_ctx);
    case ast_lambda_fun:
      return process_lambda_fun(v->as<ast_lambda_fun>(), code, target_type);
    case ast_int_const:
      return process_int_const(v->as<ast_int_const>(), code, target_type);
    case ast_string_const:
      return process_string_const(v->as<ast_string_const>(), code, target_type);
    case ast_bool_const:
      return process_bool_const(v->as<ast_bool_const>(), code, target_type);
    case ast_null_keyword:
      return process_null_keyword(v->as<ast_null_keyword>(), code, target_type);
    case ast_local_var_lhs:
      return process_local_var(v->as<ast_local_var_lhs>(), code, target_type);
    case ast_local_vars_declaration:
      return process_local_vars_declaration(v->as<ast_local_vars_declaration>(), code);
    case ast_underscore:
      return process_underscore(v->as<ast_underscore>(), code);
    case ast_empty_expression:
      return process_empty_expression(v->as<ast_empty_expression>(), code, target_type);
    case ast_artificial_aux_vertex:
      return process_artificial_aux_vertex(v->as<ast_artificial_aux_vertex>(), code, target_type);
    default:
      throw UnexpectedASTNodeKind(v, "pre_compile_expr");
  }
}


static void process_block_statement(V<ast_block_statement> v, CodeBlob& code) {
  if (v->empty()) {
    return;
  }

  FunctionPtr cur_f = code.fun_ref;
  bool does_f_return_nothing = cur_f->inferred_return_type == TypeDataVoid::create() && !cur_f->does_return_self() && !cur_f->has_mutate_params();
  bool is_toplevel_block = v == cur_f->ast_root->as<ast_function_declaration>()->get_body();
  bool inlining_doesnt_prevent = code.inline_rvect_out == nullptr || code.inlining_before_immediate_return;

  // we want to optimize `match` and `if/else`: if it's the last statement, implicitly add "return" to every branch
  // (to generate IFJMP instead of nested IF ELSE);
  // a competent way is to do it at the IR level (building CST, etc.), it's impossible to tweak Ops for now;
  // so, for every `f() { here }` of `... here; return;`, save it into a global, and handle within match/if
  AnyV backup = stmt_before_immediate_return;
  for (int i = 0; i < v->size() - 1; ++i) {
    AnyV stmt = v->get_item(i);
    AnyV next_stmt = v->get_item(i + 1);
    bool next_is_empty_return = next_stmt->kind == ast_return_statement && !next_stmt->as<ast_return_statement>()->has_return_value();
    stmt_before_immediate_return = next_is_empty_return && does_f_return_nothing && inlining_doesnt_prevent ? stmt : nullptr;
    process_any_statement(stmt, code);
  }
  AnyV last_stmt = v->get_item(v->size() - 1);
  stmt_before_immediate_return = is_toplevel_block && does_f_return_nothing && inlining_doesnt_prevent ? last_stmt : nullptr;
  process_any_statement(last_stmt, code);
  stmt_before_immediate_return = backup;
}

static void process_assert_statement(V<ast_assert_statement> v, CodeBlob& code) {
  bool excno_is_const = true;
  try { eval_expression_if_const_or_fire(v->get_thrown_code()); }
  catch (...) { excno_is_const = false; }

  if (excno_is_const) {
    // all practical cases: `assert(cond) throw SOME_ERR_CODE`, it's safe to put it on a stack
    std::vector ir_thrown_code = pre_compile_expr(v->get_thrown_code(), code);
    std::vector ir_cond = pre_compile_expr(v->get_cond(), code);
    tolk_assert(ir_cond.size() == 1 && ir_thrown_code.size() == 1);

    std::vector args_throwifnot = { ir_thrown_code[0], ir_cond[0] };
    code.add_call(v, {}, std::move(args_throwifnot), lookup_function("__throw_ifnot"));

  } else {
    // weird case: `assert(cond) throw fn()`, fn may throw or produce side effects, call it if `!cond`
    std::vector ir_cond = pre_compile_expr(v->get_cond(), code);
    Op& if_op = code.add_if_else(v, ir_cond);
    code.push_set_cur(if_op.block0);
    code.close_pop_cur(v);
    code.push_set_cur(if_op.block1);
    std::vector ir_thrown_code = pre_compile_expr(v->get_thrown_code(), code);
    code.add_call(v, {}, ir_thrown_code, lookup_function("__throw"));
    code.close_pop_cur(v);
  }
}

static void process_catch_variable(AnyExprV v_catch_var, CodeBlob& code) {
  if (auto v_ref = v_catch_var->try_as<ast_reference>(); v_ref && v_ref->sym) { // not underscore
    LocalVarPtr var_ref = v_ref->sym->try_as<LocalVarPtr>();
    tolk_assert(var_ref->ir_idx.empty());
    var_ref->mutate()->assign_ir_idx(code.create_var(v_catch_var->inferred_type, v_catch_var, var_ref->name));
  }
}

static void process_try_catch_statement(V<ast_try_catch_statement> v, CodeBlob& code) {
  code.require_callxargs = true;
  Op& try_catch_op = code.add_try_catch(v);
  code.push_set_cur(try_catch_op.block0);
  process_any_statement(v->get_try_body(), code);
  code.close_pop_cur(v->get_try_body());
  code.push_set_cur(try_catch_op.block1);

  // transform catch (excNo, arg) into TVM-catch (arg, excNo), where arg is untyped and thus almost useless now
  const std::vector<AnyExprV>& catch_vars = v->get_catch_expr()->get_items();
  tolk_assert(catch_vars.size() == 2);
  process_catch_variable(catch_vars[0], code);
  process_catch_variable(catch_vars[1], code);
  try_catch_op.left = pre_compile_tensor(code, {catch_vars[1], catch_vars[0]});
  process_any_statement(v->get_catch_body(), code);
  code.close_pop_cur(v->get_catch_body());
}

static void process_repeat_statement(V<ast_repeat_statement> v, CodeBlob& code) {
  std::vector tmp_vars = pre_compile_expr(v->get_cond(), code, nullptr);
  Op& repeat_op = code.add_repeat_loop(v, tmp_vars);
  code.push_set_cur(repeat_op.block0);
  process_any_statement(v->get_body(), code);
  code.close_pop_cur(v->get_body());
}

static void process_if_statement(V<ast_if_statement> v, CodeBlob& code) {
  std::vector ir_cond = pre_compile_expr(v->get_cond(), code, nullptr);
  tolk_assert(ir_cond.size() == 1);

  if (v->get_cond()->is_always_true) {
    process_any_statement(v->get_if_body(), code);      // v->is_ifnot does not matter here
    return;
  }
  if (v->get_cond()->is_always_false) {
    process_any_statement(v->get_else_body(), code);
    return;
  }

  Op& if_op = code.add_if_else(v, std::move(ir_cond));
  code.push_set_cur(if_op.block0);
  process_any_statement(v->get_if_body(), code);
  if (v == stmt_before_immediate_return) {
    code.add_return(v->get_if_body());
  }
  code.close_pop_cur(v->get_if_body());
  code.push_set_cur(if_op.block1);
  process_any_statement(v->get_else_body(), code);
  if (v == stmt_before_immediate_return) {
    code.add_return(v->get_else_body());
  }
  code.close_pop_cur(v->get_else_body());
  if (v->is_ifnot) {      // pre-optimized to generate IFNOT instead of IF
    std::swap(if_op.block0, if_op.block1);
  }
}

static void process_do_while_statement(V<ast_do_while_statement> v, CodeBlob& code) {
  Op& until_op = code.add_until_loop(v);
  code.push_set_cur(until_op.block0);
  process_any_statement(v->get_body(), code);

  AnyExprV cond = v->get_cond();
  std::vector ir_cond = pre_compile_expr(cond, code, nullptr);
  tolk_assert(ir_cond.size() == 1);

  // in TVM, there is only "do until", but in Tolk, we want "do while"; so, negate the condition;
  // optimizations like `while (a > 0)` -> `until (!(a > 0))` -> `until (a < 1)` are implemented as peephole
  FunctionPtr f_not = cond->inferred_type->equal_to(TypeDataBool::create()) ? lookup_function("!b_") : lookup_function("!_");
  std::vector ir_until = code.create_tmp_var(TypeDataInt::create(), cond, "(until-cond)");
  code.add_call(cond, ir_until, std::move(ir_cond), f_not);
  until_op.left = std::move(ir_until);
  code.close_pop_cur(v->get_body());
}

static void process_while_statement(V<ast_while_statement> v, CodeBlob& code) {
  Op& while_op = code.add_while_loop(v);
  code.push_set_cur(while_op.block0);
  while_op.left = pre_compile_expr(v->get_cond(), code, nullptr);
  tolk_assert(while_op.left.size() == 1);
  code.close_pop_cur(v->get_body());
  code.push_set_cur(while_op.block1);
  process_any_statement(v->get_body(), code);
  code.close_pop_cur(v->get_body());
}

static void process_throw_statement(V<ast_throw_statement> v, CodeBlob& code) {
  if (v->has_thrown_arg()) {
    // evaluate `throw (code, arg)` in ltr, in case evaluation throws an exception
    std::vector args_vars = pre_compile_tensor(code, {v->get_thrown_code(), v->get_thrown_arg()}, nullptr, {TypeDataInt::create(), TypeDataUnknown::create()});
    args_vars = {args_vars[1], args_vars[0]};   // but reverse them on a stack to match TVM order
    code.add_call(v, {}, std::move(args_vars), lookup_function("__throw_arg"));
  } else {
    std::vector args_vars = pre_compile_tensor(code, {v->get_thrown_code()});
    code.add_call(v, {}, std::move(args_vars), lookup_function("__throw"));
  }
}

static void process_return_statement(V<ast_return_statement> v, CodeBlob& code) {
  // it's a function we're traversing AST of;
  // probably, it's called and inlined into another (outer) function, we handle this below
  FunctionPtr fun_ref = code.fun_ref;

  TypePtr child_target_type = fun_ref->inferred_return_type;
  if (fun_ref->does_return_self()) {
    child_target_type = fun_ref->parameters[0].declared_type;
  }
  std::vector return_vars = pre_compile_expr(v->get_return_value(), code, child_target_type);

  if (fun_ref->does_return_self()) {
    return_vars = {};
  }
  if (fun_ref->has_mutate_params()) {
    std::vector<var_idx_t> mutated_vars;
    for (const LocalVarData& p_sym: fun_ref->parameters) {
      if (p_sym.is_mutate_parameter()) {
        mutated_vars.insert(mutated_vars.end(), p_sym.ir_idx.begin(), p_sym.ir_idx.end());
      }
    }
    return_vars.insert(return_vars.begin(), mutated_vars.begin(), mutated_vars.end());
  }

  // if fun_ref is called and inlined into a parent, assign a result instead of generating a return statement
  if (code.inline_rvect_out) {
    code.add_let(v, *code.inline_rvect_out, std::move(return_vars));
  } else {
    code.add_return(v, std::move(return_vars));
  }
}

// append "return" (void) to the end of the function
// if it's not reachable, it will be dropped
// (IR cfg reachability may differ from FlowContext in case of "never" types, so there may be situations,
//  when IR will consider this "return" reachable and leave it, but actually execution will never reach it)
static void append_implicit_return_statement(CodeBlob& code) {
  std::vector<var_idx_t> mutated_vars;
  if (code.fun_ref->has_mutate_params()) {
    for (const LocalVarData& p_sym: code.fun_ref->parameters) {
      if (p_sym.is_mutate_parameter()) {
        mutated_vars.insert(mutated_vars.end(), p_sym.ir_idx.begin(), p_sym.ir_idx.end());
      }
    }
  }
  code.add_return(code.fun_ref->ident_anchor, std::move(mutated_vars));
}


void process_any_statement(AnyV v, CodeBlob& code) {
  switch (v->kind) {
    case ast_block_statement:
      return process_block_statement(v->as<ast_block_statement>(), code);
    case ast_return_statement:
      return process_return_statement(v->as<ast_return_statement>(), code);
    case ast_repeat_statement:
      return process_repeat_statement(v->as<ast_repeat_statement>(), code);
    case ast_if_statement:
      return process_if_statement(v->as<ast_if_statement>(), code);
    case ast_do_while_statement:
      return process_do_while_statement(v->as<ast_do_while_statement>(), code);
    case ast_while_statement:
      return process_while_statement(v->as<ast_while_statement>(), code);
    case ast_throw_statement:
      return process_throw_statement(v->as<ast_throw_statement>(), code);
    case ast_assert_statement:
      return process_assert_statement(v->as<ast_assert_statement>(), code);
    case ast_try_catch_statement:
      return process_try_catch_statement(v->as<ast_try_catch_statement>(), code);
    case ast_empty_statement:
      return;
    default:
      pre_compile_expr(reinterpret_cast<AnyExprV>(v), code, nullptr);
  }
}

static void convert_function_body_to_CodeBlob(FunctionPtr fun_ref, FunctionBodyCode* code_body) {
  auto v_body = fun_ref->ast_root->as<ast_function_declaration>()->get_body()->as<ast_block_statement>();
  CodeBlob* blob = new CodeBlob(fun_ref);

  std::vector<var_idx_t> rvect_import;
  int total_arg_width = 0;
  for (int i = 0; i < fun_ref->get_num_params(); ++i) {
    total_arg_width += fun_ref->parameters[i].declared_type->get_width_on_stack();
  }
  rvect_import.reserve(total_arg_width);

  for (int i = 0; i < fun_ref->get_num_params(); ++i) {
    const LocalVarData& param_i = fun_ref->parameters[i];
    std::vector ir_param = blob->create_var(param_i.declared_type, param_i.ident_anchor, param_i.name);
    rvect_import.insert(rvect_import.end(), ir_param.begin(), ir_param.end());
    param_i.mutate()->assign_ir_idx(std::move(ir_param));
  }
  blob->add_import_fun_params(fun_ref->ident_anchor, rvect_import);
  blob->in_var_cnt = blob->var_cnt;
  tolk_assert(blob->var_cnt == total_arg_width);

  if (fun_ref->name == "onInternalMessage") {
    handle_onInternalMessage_codegen_start(fun_ref, rvect_import, *blob, fun_ref->ident_anchor);
  }

  process_block_statement(v_body, *blob);
  append_implicit_return_statement(*blob);

  blob->close_blk(v_body);
  code_body->set_code(blob);
  tolk_assert(vars_modification_watcher.empty());
}

static void convert_asm_body_to_AsmOp(FunctionPtr fun_ref, FunctionBodyAsm* asm_body) {
  int cnt = fun_ref->get_num_params();
  int width = fun_ref->inferred_return_type->get_width_on_stack();
  std::vector<AsmOp> asm_ops;
  for (AnyV v_child : fun_ref->ast_root->as<ast_function_declaration>()->get_body()->as<ast_asm_body>()->get_asm_commands()) {
    std::string_view ops = v_child->as<ast_string_const>()->str_val; // <op>\n<op>\n...
    std::string op;
    for (char c : ops) {
      if (c == '\n' || c == '\r') {
        if (!op.empty()) {
          asm_ops.push_back(AsmOp::Parse(v_child, op, cnt, width));
          if (asm_ops.back().is_custom()) {
            cnt = width;
          }
          op.clear();
        }
      } else {
        op.push_back(c);
      }
    }
    if (!op.empty()) {
      asm_ops.push_back(AsmOp::Parse(v_child, op, cnt, width));
      if (asm_ops.back().is_custom()) {
        cnt = width;
      }
    }
  }

  asm_body->set_code(std::move(asm_ops));
}

class UpdateArgRetOrderConsideringStackWidth final {
public:
  static bool should_visit_function(FunctionPtr fun_ref) {
    return !fun_ref->is_generic_function() && (!fun_ref->ret_order.empty() || !fun_ref->arg_order.empty());
  }

  static void start_visiting_function(FunctionPtr fun_ref, V<ast_function_declaration> v_function) {
    int total_arg_mutate_width = 0;
    bool has_arg_width_not_1 = false;
    for (const LocalVarData& param : fun_ref->parameters) {
      int arg_width = param.declared_type->get_width_on_stack();
      has_arg_width_not_1 |= arg_width != 1;
      total_arg_mutate_width += param.is_mutate_parameter() * arg_width;
    }

    // do not allow `mutate` combined with non-primitives arg_order shuffling: impractical but hard to follow
    if (has_arg_width_not_1 && total_arg_mutate_width) {
      err("width not 1 and mutate").fire(fun_ref->ident_anchor, fun_ref);
    }

    // example: `fun f(a: int, b: (int, (int, int)), c: int)` with `asm (b a c)`
    // current arg_order is [1 0 2]
    // needs to be converted to [1 2 3 0 4] because b width is 3
    if (has_arg_width_not_1 && !fun_ref->arg_order.empty()) {
      int total_arg_width = 0;
      std::vector<int> cum_arg_width;
      cum_arg_width.reserve(1 + fun_ref->get_num_params());
      cum_arg_width.push_back(0);
      for (const LocalVarData& param : fun_ref->parameters) {
        cum_arg_width.push_back(total_arg_width += param.declared_type->get_width_on_stack());
      }
      std::vector<int> arg_order;
      for (int i = 0; i < fun_ref->get_num_params(); ++i) {
        int j = fun_ref->arg_order[i];
        int c1 = cum_arg_width[j], c2 = cum_arg_width[j + 1];
        while (c1 < c2) {
          arg_order.push_back(c1++);
        }
      }
      fun_ref->mutate()->assign_arg_order(std::move(arg_order));
    }

    // example: `fun f(mutate self: slice): slice` with `asm(-> 1 0)`
    // ret_order is a shuffled range 0...N
    // validate N: a function should return value and mutated arguments onto a stack
    if (!fun_ref->ret_order.empty()) {
      size_t expected_width = fun_ref->inferred_return_type->get_width_on_stack() + total_arg_mutate_width;
      if (expected_width != fun_ref->ret_order.size()) {
        err("ret_order (after ->) expected to contain {} numbers", expected_width).fire(v_function->get_body());
      }
    }
  }
};

class ConvertASTToLegacyOpVisitor final {
public:
  static bool should_visit_function(FunctionPtr fun_ref) {
    return !fun_ref->is_generic_function();
  }

  static void start_visiting_function(FunctionPtr fun_ref, V<ast_function_declaration>) {
    tolk_assert(fun_ref->is_type_inferring_done());
    if (fun_ref->is_code_function() && !fun_ref->is_inlined_in_place()) {
      convert_function_body_to_CodeBlob(fun_ref, std::get<FunctionBodyCode*>(fun_ref->body));
    } else if (fun_ref->is_asm_function()) {
      convert_asm_body_to_AsmOp(fun_ref, std::get<FunctionBodyAsm*>(fun_ref->body));
    }
  }
};

void pipeline_convert_ast_to_legacy_Expr_Op() {
  UpdateArgRetOrderConsideringStackWidth asm_updater;
  visit_ast_of_all_functions(asm_updater);

  ConvertASTToLegacyOpVisitor ir_converter;
  visit_ast_of_all_functions(ir_converter);
}

} // namespace tolk
