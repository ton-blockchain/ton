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
#include "type-system.h"
#include "common/refint.h"
#include "constant-evaluator.h"
#include <unordered_set>

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
 *   The same mechanism will work for union types in the future.
 */

namespace tolk {

class LValContext;
std::vector<var_idx_t> pre_compile_expr(AnyExprV v, CodeBlob& code, TypePtr target_type = nullptr, LValContext* lval_ctx = nullptr);
std::vector<var_idx_t> pre_compile_symbol(SrcLocation loc, const Symbol* sym, CodeBlob& code, LValContext* lval_ctx);
void process_any_statement(AnyV v, CodeBlob& code);


// The goal of VarsModificationWatcher is to detect such cases: `return (x, x += y, x)`.
// Without any changes, ops will be { _Call $2 = +($0_x, $1_y); _Return $0_x, $2, $0_x } - incorrect
// Correct will be to introduce tmp var: { _Let $3 = $0_x; _Call $2 = ...; _Return $3, $2, $0_x }
// This "introducing" is done when compiling tensors, whereas this class allows to watch vars for modification.
class VarsModificationWatcher {
  struct WatchedVar {
    var_idx_t ir_idx;
    std::function<void(SrcLocation, var_idx_t)> on_modification_callback;

    WatchedVar(var_idx_t ir_idx, std::function<void(SrcLocation, var_idx_t)> on_modification_callback)
      : ir_idx(ir_idx), on_modification_callback(std::move(on_modification_callback)) {}
  };

  std::vector<WatchedVar> all_callbacks;

public:

  bool empty() const { return all_callbacks.empty(); }

  void push_callback(var_idx_t ir_idx, std::function<void(SrcLocation, var_idx_t)> callback) {
    all_callbacks.emplace_back(ir_idx, std::move(callback));
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

  void trigger_callbacks(const std::vector<var_idx_t>& left_lval_indices, SrcLocation loc) const {
    for (const WatchedVar& w : all_callbacks) {
      for (var_idx_t changed_var : left_lval_indices) {
        if (w.ir_idx == changed_var) {
          w.on_modification_callback(loc, w.ir_idx);
        }
      }
    }
  }
};

static VarsModificationWatcher vars_modification_watcher;


// Main goal of LValContext is to handle non-primitive lvalues. At IR level, a usual local variable
// exists, but on its change, something non-trivial should happen.
// Example: `globalVar = 9` actually does `Const $5 = 9` + `Let $6 = $5` + `SetGlob "globVar" = $6`
// Example: `tupleVar.0 = 9` actually does `Const $5 = 9` + `Let $6 = $5` + `Const $7 = 0` + `Call tupleSetAt($4, $6, $7)`
// Of course, mixing globals with tuples should also be supported.
// To achieve this, treat tupleObj inside "tupleObj.i" like "rvalue inside lvalue".
// For instance, `globalTuple.0 = 9` reads global (like rvalue), assigns 9 to tmp var, modifies tuple, writes global.
// Note, that tensors (not tuples) `tensorVar.0 = 9` do not emit anything special (unless global).
class LValContext {
  // every global variable used as lvalue is registered here
  // example: `globalInt = 9`, implicit var is created `$tmp = 9`, and `SetGlob "globalInt" $tmp` is done after
  struct ModifiedGlobal {
    GlobalVarPtr glob_ref;
    std::vector<var_idx_t> lval_ir_idx;    // typically 1, generally get_width_on_stack() of global var (tensors)

    // for 1-slot globals int/cell/slice, assigning to them is just SETGLOB
    // same for tensors, if they are fully rewritten in an expression: `gTensor = (5,6)`
    void apply_fully_rewrite(CodeBlob& code, SrcLocation loc) const {
      Op& op = code.emplace_back(loc, Op::_SetGlob, std::vector<var_idx_t>{}, lval_ir_idx, glob_ref);
      op.set_impure_flag();
    }

    // for N-slot globals tensor/struct/union, assigning to their parts, like `gTensor.1 = 6`
    // we need to read gTensor as a whole (0-th and 1-th component), rewrite 1-th component, and SETGLOB a whole back
    void apply_partially_rewrite(CodeBlob& code, SrcLocation loc, std::vector<bool>&& was_modified_by_let) const {
      LValContext local_lval;
      local_lval.enter_rval_inside_lval();
      std::vector<var_idx_t> local_ir_idx = pre_compile_symbol(loc, glob_ref, code, &local_lval);
      for (size_t i = 0; i < local_ir_idx.size(); ++i) {
        if (was_modified_by_let[i]) {
          code.emplace_back(loc, Op::_Let, std::vector{local_ir_idx[i]}, std::vector{lval_ir_idx[i]});
        }
      }

      Op& op = code.emplace_back(loc, Op::_SetGlob, std::vector<var_idx_t>{}, local_ir_idx, glob_ref);
      op.set_impure_flag();
    }
  };

  // every tensor index, when a tensor is a global, is registered here (same for structs and fields)
  // example: `global v: (int, int); v.1 = 5`, implicit var is created `$tmp = 5`, and when it's modified,
  // we need to partially update w; essentially, apply_partially_rewrite() above will be called
  struct ModifiedFieldOfGlobal {
    AnyExprV tensor_obj;
    int index_at;
    std::vector<var_idx_t> lval_ir_idx;

    void apply(CodeBlob& code, SrcLocation loc) const {
      LValContext local_lval;
      local_lval.enter_rval_inside_lval();
      std::vector<var_idx_t> obj_ir_idx = pre_compile_expr(tensor_obj, code, nullptr, &local_lval);
      const TypeDataTensor* t_tensor = tensor_obj->inferred_type->try_as<TypeDataTensor>();
      tolk_assert(t_tensor);
      int stack_width = t_tensor->items[index_at]->get_width_on_stack();
      int stack_offset = 0;
      for (int i = 0; i < index_at; ++i) {
        stack_offset += t_tensor->items[i]->get_width_on_stack();
      }
      std::vector<var_idx_t> field_ir_idx = {obj_ir_idx.begin() + stack_offset, obj_ir_idx.begin() + stack_offset + stack_width};
      tolk_assert(field_ir_idx.size() == lval_ir_idx.size());

      vars_modification_watcher.trigger_callbacks(field_ir_idx, loc);
      code.emplace_back(loc, Op::_Let, field_ir_idx, lval_ir_idx);
      local_lval.after_let(std::move(field_ir_idx), code, loc);
    }
  };

  // every tuple index used as lvalue is registered here
  // example: `t.0 = 9`, implicit var is created `$tmp = 9`, as well as `$tmp_idx = 0` and `tupleSetAt()` is done after
  // for `t.0.0` if t is `[[int, ...]]`, `tupleAt()` for it is done since it's rvalue, and `tupleSetAt()` is done 2 times
  struct ModifiedTupleIndex {
    AnyExprV tuple_obj;
    int index_at;
    std::vector<var_idx_t> lval_ir_idx;

    void apply(CodeBlob& code, SrcLocation loc) const {
      LValContext local_lval;
      local_lval.enter_rval_inside_lval();
      std::vector<var_idx_t> tuple_ir_idx = pre_compile_expr(tuple_obj, code, nullptr, &local_lval);
      std::vector<var_idx_t> index_ir_idx = code.create_tmp_var(TypeDataInt::create(), loc, "(tuple-idx)");
      code.emplace_back(loc, Op::_IntConst, index_ir_idx, td::make_refint(index_at));

      vars_modification_watcher.trigger_callbacks(tuple_ir_idx, loc);
      FunctionPtr builtin_sym = lookup_global_symbol("tupleSetAt")->try_as<FunctionPtr>();
      code.emplace_back(loc, Op::_Call, std::vector{tuple_ir_idx}, std::vector{tuple_ir_idx[0], lval_ir_idx[0], index_ir_idx[0]}, builtin_sym);
      local_lval.after_let(std::move(tuple_ir_idx), code, loc);
    }
  };

  int level_rval_inside_lval = 0;
  std::vector<std::variant<ModifiedGlobal, ModifiedTupleIndex, ModifiedFieldOfGlobal>> modifications;

  static bool vector_contains(const std::vector<var_idx_t>& ir_vars, var_idx_t ir_idx) {
    for (var_idx_t var_in_vector : ir_vars) {
      if (var_in_vector == ir_idx) {
        return true;
      }
    }
    return false;
  }

public:
  void enter_rval_inside_lval() { level_rval_inside_lval++; }
  void exit_rval_inside_lval() { level_rval_inside_lval--; }
  bool is_rval_inside_lval() const { return level_rval_inside_lval > 0; }

  void capture_global_modification(GlobalVarPtr glob_ref, std::vector<var_idx_t> lval_ir_idx) {
    modifications.emplace_back(ModifiedGlobal{glob_ref, std::move(lval_ir_idx)});
  }

  void capture_field_of_global_modification(AnyExprV tensor_obj, int index_at, std::vector<var_idx_t> lval_ir_idx) {
    modifications.emplace_back(ModifiedFieldOfGlobal{tensor_obj, index_at, std::move(lval_ir_idx)});
  }

  void capture_tuple_index_modification(AnyExprV tuple_obj, int index_at, std::vector<var_idx_t> lval_ir_idx) {
    modifications.emplace_back(ModifiedTupleIndex{tuple_obj, index_at, std::move(lval_ir_idx)});
  }

  void after_let(std::vector<var_idx_t>&& let_left_vars, CodeBlob& code, SrcLocation loc) const {
    for (const auto& modification : modifications) {
      if (const auto* m_glob = std::get_if<ModifiedGlobal>(&modification)) {
        int n_modified_by_let = 0;
        std::vector<bool> was_modified_by_let;
        was_modified_by_let.resize(m_glob->lval_ir_idx.size());
        for (size_t i = 0; i < m_glob->lval_ir_idx.size(); ++i) {
          if (vector_contains(let_left_vars, m_glob->lval_ir_idx[i])) {
            was_modified_by_let[i] = true;
            n_modified_by_let++;
          }
        }
        if (n_modified_by_let == static_cast<int>(m_glob->lval_ir_idx.size())) {
          m_glob->apply_fully_rewrite(code, loc);
        } else if (n_modified_by_let > 0) {
          m_glob->apply_partially_rewrite(code, loc, std::move(was_modified_by_let));
        }
      } else if (const auto* m_tup = std::get_if<ModifiedTupleIndex>(&modification)) {
        bool was_tuple_index_modified = false;
        for (var_idx_t field_ir_idx : m_tup->lval_ir_idx) {
          was_tuple_index_modified |= vector_contains(let_left_vars, field_ir_idx);
        }
        if (was_tuple_index_modified) {
          m_tup->apply(code, loc);
        }
      } else if (const auto* m_tens = std::get_if<ModifiedFieldOfGlobal>(&modification)) {
        bool was_tensor_index_modified = false;
        for (var_idx_t field_ir_idx : m_tens->lval_ir_idx) {
          was_tensor_index_modified |= vector_contains(let_left_vars, field_ir_idx);
        }
        if (was_tensor_index_modified) {
          m_tens->apply(code, loc);
        }
      }
    }
  }
};

// given `{some_expr}!`, return some_expr
static AnyExprV unwrap_not_null_operator(AnyExprV v) {
  while (auto v_notnull = v->try_as<ast_not_null_operator>()) {
    v = v_notnull->get_expr();
  }
  return v;
}

// given `{some_expr}.{i}`, check it for pattern `some_var.0` / `some_var.0.1` / etc.
// return some_var if satisfies (it may be a local or a global var, a tensor or a tuple)
// return nullptr otherwise: `f().0` / `(v = rhs).0` / `some_var.method().0` / etc.
static V<ast_reference> calc_sink_leftmost_obj(V<ast_dot_access> v) {
  AnyExprV leftmost_obj = unwrap_not_null_operator(v->get_obj());
  while (auto v_dot = leftmost_obj->try_as<ast_dot_access>()) {
    if (!v_dot->is_target_indexed_access()) {
      break;
    }
    leftmost_obj = unwrap_not_null_operator(v_dot->get_obj());
  }
  return leftmost_obj->type == ast_reference ? leftmost_obj->as<ast_reference>() : nullptr;
}


static std::vector<std::vector<var_idx_t>> pre_compile_tensor_inner(CodeBlob& code, const std::vector<AnyExprV>& args,
                                          const TypeDataTensor* tensor_target_type, LValContext* lval_ctx) {
  const int n = static_cast<int>(args.size());
  if (n == 0) {  // just `()`
    return {};
  }
  tolk_assert(!tensor_target_type || tensor_target_type->size() == n);
  if (n == 1) {  // just `(x)`: even if x is modified (e.g. `f(x=x+2)`), there are no next arguments
    TypePtr child_target_type = tensor_target_type ? tensor_target_type->items[0] : nullptr;
    return {pre_compile_expr(args[0], code, child_target_type, lval_ctx)};
  }

  // the purpose is to handle such cases: `return (x, x += y, x)`
  // without this, ops will be { _Call $2 = +($0_x, $1_y); _Return $0_x, $2, $0_x } - invalid
  // with this, ops will be { _Let $3 = $0_x; _Call $2 = ...; _Return $3, $2, $0_x } - valid, tmp var for x
  // how it works: for every arg, after transforming to ops, start tracking ir_idx inside it
  // on modification attempt, create Op::_Let to a tmp var and replace old ir_idx with tmp_idx in result
  struct WatchingVarList {
    std::vector<var_idx_t> watched_vars;
    std::vector<std::vector<var_idx_t>> res_lists;

    explicit WatchingVarList(int n_args) {
      res_lists.reserve(n_args);
    }

    bool is_watched(var_idx_t ir_idx) const {
      return std::find(watched_vars.begin(), watched_vars.end(), ir_idx) != watched_vars.end();
    }

    void add_and_watch_modifications(std::vector<var_idx_t>&& vars_of_ith_arg, CodeBlob& code) {
      for (var_idx_t ir_idx : vars_of_ith_arg) {
        if (!code.vars[ir_idx].name.empty() && !is_watched(ir_idx)) {
          watched_vars.emplace_back(ir_idx);
          vars_modification_watcher.push_callback(ir_idx, [this, &code](SrcLocation loc, var_idx_t ir_idx) {
            on_var_modified(ir_idx, loc, code);
          });
        }
      }
      res_lists.emplace_back(std::move(vars_of_ith_arg));
    }

    void on_var_modified(var_idx_t ir_idx, SrcLocation loc, CodeBlob& code) {
      tolk_assert(is_watched(ir_idx));
      std::vector<var_idx_t> tmp_idx_arr = code.create_tmp_var(code.vars[ir_idx].v_type, loc, "(pre-modified)");
      tolk_assert(tmp_idx_arr.size() == 1);
      var_idx_t tmp_idx = tmp_idx_arr[0];
      code.emplace_back(loc, Op::_Let, std::vector{tmp_idx}, std::vector{ir_idx});
      for (std::vector<var_idx_t>& prev_vars : res_lists) {
        std::replace(prev_vars.begin(), prev_vars.end(), ir_idx, tmp_idx);
      }
    }

    std::vector<std::vector<var_idx_t>> clear_and_stop_watching() {
      for (var_idx_t ir_idx : watched_vars) {
        vars_modification_watcher.pop_callback(ir_idx);
      }
      watched_vars.clear();
      return std::move(res_lists);
    }
  };

  WatchingVarList watched_vars(n);
  for (int arg_idx = 0; arg_idx < n; ++arg_idx) {
    TypePtr child_target_type = tensor_target_type ? tensor_target_type->items[arg_idx] : nullptr;
    std::vector<var_idx_t> vars_of_ith_arg = pre_compile_expr(args[arg_idx], code, child_target_type, lval_ctx);
    watched_vars.add_and_watch_modifications(std::move(vars_of_ith_arg), code);
  }
  return watched_vars.clear_and_stop_watching();
}

static std::vector<var_idx_t> pre_compile_tensor(CodeBlob& code, const std::vector<AnyExprV>& args,
                                          LValContext* lval_ctx = nullptr) {
  std::vector<TypePtr> types_list;
  types_list.reserve(args.size());
  for (AnyExprV item : args) {
    types_list.push_back(item->inferred_type);
  }
  const TypeDataTensor* tensor_target_type = TypeDataTensor::create(std::move(types_list))->try_as<TypeDataTensor>();
  std::vector<std::vector<var_idx_t>> res_lists = pre_compile_tensor_inner(code, args, tensor_target_type, lval_ctx);
  std::vector<var_idx_t> res;
  for (const std::vector<var_idx_t>& list : res_lists) {
    res.insert(res.end(), list.cbegin(), list.cend());
  }
  return res;
}

static std::vector<var_idx_t> pre_compile_let(CodeBlob& code, AnyExprV lhs, AnyExprV rhs, SrcLocation loc) {
  // [lhs] = [rhs]; since type checking is ok, it's the same as "lhs = rhs"
  if (lhs->type == ast_typed_tuple && rhs->type == ast_typed_tuple) {
    // note: there are no type transitions (adding nullability flag, etc.), since only 1-slot elements allowed in tuples
    LValContext local_lval;
    std::vector<var_idx_t> left = pre_compile_tensor(code, lhs->as<ast_typed_tuple>()->get_items(), &local_lval);
    vars_modification_watcher.trigger_callbacks(left, loc);
    std::vector<var_idx_t> rvect = pre_compile_tensor(code, rhs->as<ast_typed_tuple>()->get_items());
    code.emplace_back(loc, Op::_Let, left, rvect);
    local_lval.after_let(std::move(left), code, loc);
    std::vector<var_idx_t> right = code.create_tmp_var(TypeDataTuple::create(), loc, "(tuple)");
    code.emplace_back(lhs->loc, Op::_Tuple, right, std::move(rvect));
    return right;
  }
  // [lhs] = rhs; it's un-tuple to N left vars
  if (lhs->type == ast_typed_tuple) {
    LValContext local_lval;
    std::vector<var_idx_t> left = pre_compile_tensor(code, lhs->as<ast_typed_tuple>()->get_items(), &local_lval);
    vars_modification_watcher.trigger_callbacks(left, loc);
    std::vector<var_idx_t> right = pre_compile_expr(rhs, code, nullptr);
    const TypeDataTypedTuple* inferred_tuple = rhs->inferred_type->try_as<TypeDataTypedTuple>();
    std::vector<TypePtr> types_list = inferred_tuple->items;
    std::vector<var_idx_t> rvect = code.create_tmp_var(TypeDataTensor::create(std::move(types_list)), rhs->loc, "(unpack-tuple)");
    code.emplace_back(lhs->loc, Op::_UnTuple, rvect, std::move(right));
    code.emplace_back(loc, Op::_Let, left, rvect);
    local_lval.after_let(std::move(left), code, loc);
    return right;
  }
  // small optimization: `var x = rhs` or `local_var = rhs` (90% cases), LValContext not needed actually
  if (lhs->type == ast_local_var_lhs || (lhs->type == ast_reference && lhs->as<ast_reference>()->sym->try_as<LocalVarPtr>())) {
    std::vector<var_idx_t> left = pre_compile_expr(lhs, code, nullptr);    // effectively, local_var->ir_idx
    vars_modification_watcher.trigger_callbacks(left, loc);
    std::vector<var_idx_t> right = pre_compile_expr(rhs, code, lhs->inferred_type);
    code.emplace_back(loc, Op::_Let, std::move(left), right);
    return right;
  }
  // lhs = rhs
  LValContext local_lval;
  std::vector<var_idx_t> left = pre_compile_expr(lhs, code, nullptr, &local_lval);
  vars_modification_watcher.trigger_callbacks(left, loc);
  std::vector<var_idx_t> right = pre_compile_expr(rhs, code, lhs->inferred_type);
  code.emplace_back(loc, Op::_Let, left, right);
  local_lval.after_let(std::move(left), code, loc);
  return right;
}

static std::vector<var_idx_t> gen_op_call(CodeBlob& code, TypePtr ret_type, SrcLocation loc,
                                          std::vector<var_idx_t>&& args_vars, FunctionPtr fun_ref, const char* debug_desc) {
  std::vector<var_idx_t> rvect = code.create_tmp_var(ret_type, loc, debug_desc);
  Op& op = code.emplace_back(loc, Op::_Call, rvect, std::move(args_vars), fun_ref);
  if (!fun_ref->is_marked_as_pure()) {
    op.set_impure_flag();
  }
  return rvect;
}

// "Transition to target (runtime) type" is the following process.
// Imagine `fun analyze(t: (int,int)?)` and a call `analyze((1,2))`.
// `(1,2)` (inferred_type) is 2 stack slots, but `t` (target_type) is 3 (one for null-flag).
// So, this null flag should be implicitly added (non-zero, since a variable is not null).
// Another example: `var t: (int, int)? = null`.
// `null` (inferred_type) is 1 stack slots, but target_type is 3, we should add 2 nulls.
// Another example: `var t1 = (1, null); var t2: (int, (int,int)?) = t1;`.
// Then t1's rvect is 2 vars (1 and null), but t1's `null` should be converted to 3 stack slots (resulting in 4 total).
// The same mechanism will work for union types in the future.
// Here rvect is a list of IR vars for inferred_type, probably patched due to target_type.
GNU_ATTRIBUTE_NOINLINE
static std::vector<var_idx_t> transition_expr_to_runtime_type_impl(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr original_type, TypePtr target_type, SrcLocation loc) {
  // pass `T` to `T`
  // could occur for passing tensor `(..., T, ...)` to `(..., T, ...)` while traversing tensor's components
  if (target_type == original_type) {
    return rvect;
  }

  int target_w = target_type->get_width_on_stack();
  const TypeDataNullable* t_nullable = target_type->try_as<TypeDataNullable>();
  const TypeDataNullable* o_nullable = original_type->try_as<TypeDataNullable>();

  // handle `never`
  // it may occur due to smart cast and in unreachable branches
  // we can't do anything reasonable here, but (hopefully) execution will never reach this point, and stack won't be polluted
  if (original_type == TypeDataNever::create()) {
    std::vector<var_idx_t> dummy_rvect;
    dummy_rvect.reserve(target_w);
    for (int i = 0; i < target_w; ++i) {
      dummy_rvect.push_back(code.create_tmp_var(TypeDataUnknown::create(), loc, "(never)")[0]);
    }
    return dummy_rvect;
  }
  if (target_type == TypeDataNever::create()) {
    return {};
  }

  // pass `null` to `T?`
  // for primitives like `int?`, no changes in rvect, null occupies the same TVM slot
  // for tensors like `(int,int)?`, `null` is represented as N nulls + 1 null flag, insert N nulls
  if (t_nullable && original_type == TypeDataNullLiteral::create()) {
    tolk_assert(rvect.size() == 1);
    if (target_w == 1 && !t_nullable->is_primitive_nullable()) {    // `null` to `()?`
      rvect = code.create_tmp_var(TypeDataInt::create(), loc, "(NNFlag)");
      code.emplace_back(loc, Op::_IntConst, rvect, td::make_refint(0));
    }
    if (target_w > 1) {
      FunctionPtr builtin_sym = lookup_global_symbol("__null")->try_as<FunctionPtr>();
      rvect.reserve(target_w + 1);
      for (int i = 1; i < target_w - 1; ++i) {
        std::vector<var_idx_t> ith_null = code.create_tmp_var(TypeDataNullLiteral::create(), loc, "(null-literal)");
        code.emplace_back(loc, Op::_Call, ith_null, std::vector<var_idx_t>{}, builtin_sym);
        rvect.push_back(ith_null[0]);
      }
      std::vector<var_idx_t> null_flag_ir = code.create_tmp_var(TypeDataInt::create(), loc, "(NNFlag)");
      var_idx_t null_flag_ir_idx = null_flag_ir[0];
      code.emplace_back(loc, Op::_IntConst, std::move(null_flag_ir), td::make_refint(0));
      rvect.push_back(null_flag_ir_idx);
    }
    return rvect;
  }
  // pass `T` to `T?`
  // for primitives like `int?`, no changes in rvect: `int` and `int?` occupy the same TVM slot (null is represented as NULL TVM value)
  // for passing `(int, int)` to `(int, int)?` / `(int, null)` to `(int, (int,int)?)?`, add a null flag equals to 0
  if (t_nullable && !o_nullable) {
    if (!t_nullable->is_primitive_nullable()) {
      rvect = transition_expr_to_runtime_type_impl(std::move(rvect), code, original_type, t_nullable->inner, loc);
      tolk_assert(target_w == static_cast<int>(rvect.size() + 1));
      std::vector<var_idx_t> null_flag_ir = code.create_tmp_var(TypeDataInt::create(), loc, "(NNFlag)");
      var_idx_t null_flag_ir_idx = null_flag_ir[0];
      code.emplace_back(loc, Op::_IntConst, std::move(null_flag_ir), td::make_refint(-1));
      rvect.push_back(null_flag_ir_idx);
    }
    return rvect;
  }
  // pass `T1?` to `T2?`
  // for example, `int8?` to `int16?`
  // transition inner types, leaving nullable flag unchanged for tensors
  if (t_nullable && o_nullable) {
    if (target_w > 1) {
      var_idx_t null_flag_ir_idx = rvect.back();
      rvect.pop_back();
      rvect = transition_expr_to_runtime_type_impl(std::move(rvect), code, o_nullable->inner, t_nullable->inner, loc);
      rvect.push_back(null_flag_ir_idx);
    }
    return rvect;
  }
  // pass `T?` to `null`
  // it may occur due to smart cast, when a `T?` variable is guaranteed to be always null
  // (for instance, always-null `(int,int)?` will be represented as 1 TVM NULL value, not 3)
  if (target_type == TypeDataNullLiteral::create() && original_type->can_rhs_be_assigned(target_type)) {
    tolk_assert(o_nullable || original_type == TypeDataUnknown::create());
    if (o_nullable && !o_nullable->is_primitive_nullable()) {
      FunctionPtr builtin_sym = lookup_global_symbol("__null")->try_as<FunctionPtr>();
      rvect = code.create_tmp_var(TypeDataNullLiteral::create(), loc, "(null-literal)");
      code.emplace_back(loc, Op::_Call, rvect, std::vector<var_idx_t>{}, builtin_sym);
    }
    return rvect;
  }
  // pass `T?` to `T` (or, more generally, `T1?` to `T2`)
  // it may occur due to operator `!` or smart cast
  // for primitives like `int?`, no changes in rvect
  // for passing `(int, int)?` to `(int, int)`, drop the null flag from the tail
  // for complex scenarios like passing `(int, (int,int)?)?` to `(int, null)`, recurse the call
  // (it may occur on `someF(t = (3,null))` when `(3,null)` at first targeted to lhs, but actually its result is rhs)
  if (!t_nullable && o_nullable) {
    if (!o_nullable->is_primitive_nullable()) {
      rvect.pop_back();
      rvect = transition_expr_to_runtime_type_impl(std::move(rvect), code, original_type->try_as<TypeDataNullable>()->inner, target_type, loc);
    }
    return rvect;
  }
  // pass `bool` to `int`
  // in code, it's done via `as` operator, like `boolVar as int`
  // no changes in rvect, boolVar is guaranteed to be -1 or 0 at TVM level
  if (original_type == TypeDataBool::create() && target_type == TypeDataInt::create()) {
    return rvect;
  }
  // pass `bool` to `int8`
  // same as above
  if (original_type == TypeDataBool::create() && target_type->try_as<TypeDataIntN>()) {
    return rvect;
  }
  // pass `int8` to `int`
  // it comes from auto cast when an integer (even a literal) is assigned to intN
  // to changes in rvect, intN is int at TVM level
  if (target_type == TypeDataInt::create() && original_type->try_as<TypeDataIntN>()) {
    return rvect;
  }
  // pass `coins` to `int`
  // same as above
  if (target_type == TypeDataInt::create() && original_type == TypeDataCoins::create()) {
    return rvect;
  }
  // pass `int` to `int8`
  // in code, it's probably done with `as` operator
  // no changes in rvect
  if (original_type == TypeDataInt::create() && target_type->try_as<TypeDataIntN>()) {
    return rvect;
  }
  // pass `int` to `coins`
  // same as above
  if (original_type == TypeDataInt::create() && target_type == TypeDataCoins::create()) {
    return rvect;
  }
  // pass `int8` to `int16` / `int8` to `uint8`
  // in code, it's probably done with `as` operator
  // no changes in rvect
  if (original_type->try_as<TypeDataIntN>() && target_type->try_as<TypeDataIntN>()) {
    return rvect;
  }
  // pass `int8` to `coins`
  // same as above
  if (target_type == TypeDataCoins::create() && original_type->try_as<TypeDataIntN>()) {
    return rvect;
  }
  // pass `coins` to `int8`
  // same as above
  if (original_type == TypeDataCoins::create() && target_type->try_as<TypeDataIntN>()) {
    return rvect;
  }
  // pass `bytes32` to `slice`
  // in code, it's probably done with `as` operator
  // no changes in rvect, since bytesN is slice at TVM level
  if (target_type == TypeDataSlice::create() && original_type->try_as<TypeDataBytesN>()) {
    return rvect;
  }
  // pass `slice` to `bytes32`
  // same as above
  if (original_type == TypeDataSlice::create() && target_type->try_as<TypeDataBytesN>()) {
    return rvect;
  }
  // pass `bytes32` to `bytes64` / `bits128` to `bytes16`
  // no changes in rvect
  if (original_type->try_as<TypeDataBytesN>() && target_type->try_as<TypeDataBytesN>()) {
    return rvect;
  }
  // pass something to `unknown`
  // probably, it comes from `_ = rhs`, type of `_` is unknown, it's target_type of rhs
  // no changes in rvect
  if (target_type == TypeDataUnknown::create()) {
    return rvect;
  }
  // pass `unknown` to something
  // probably, it comes from `arg` in exception, it's inferred as `unknown` and could be cast to any value
  if (original_type == TypeDataUnknown::create()) {
    tolk_assert(rvect.size() == 1);
    return rvect;
  }
  // pass tensor to tensor, e.g. `(1, null)` to `(int, slice?)` / `(1, null)` to `(int, (int,int)?)`
  // every element of rhs tensor should be transitioned
  if (target_type->try_as<TypeDataTensor>() && original_type->try_as<TypeDataTensor>()) {
    const TypeDataTensor* target_tensor = target_type->try_as<TypeDataTensor>();
    const TypeDataTensor* inferred_tensor = original_type->try_as<TypeDataTensor>();
    tolk_assert(target_tensor->size() == inferred_tensor->size());
    tolk_assert(inferred_tensor->get_width_on_stack() == static_cast<int>(rvect.size()));
    std::vector<var_idx_t> result_rvect;
    result_rvect.reserve(target_w);
    int stack_offset = 0;
    for (int i = 0; i < inferred_tensor->size(); ++i) {
      int ith_w = inferred_tensor->items[i]->get_width_on_stack();
      std::vector<var_idx_t> rvect_i{rvect.begin() + stack_offset, rvect.begin() + stack_offset + ith_w};
      std::vector<var_idx_t> result_i = transition_expr_to_runtime_type_impl(std::move(rvect_i), code, inferred_tensor->items[i], target_tensor->items[i], loc);
      result_rvect.insert(result_rvect.end(), result_i.begin(), result_i.end());
      stack_offset += ith_w;
    }
    return result_rvect;
  }
  // pass tuple to tuple, e.g. `[1, null]` to `[int, int?]` / `[1, null]` to `[int, [int?,int?]?]`
  // to changes to rvect, since tuples contain only 1-slot elements
  if (target_type->try_as<TypeDataTypedTuple>() && original_type->try_as<TypeDataTypedTuple>()) {
    tolk_assert(target_type->get_width_on_stack() == original_type->get_width_on_stack());
    return rvect;
  }

  throw Fatal("unhandled transition_expr_to_runtime_type_impl() combination");
}

// invoke the function above only if potentially needed to
// (if an expression is targeted to another type)
#ifndef TOLK_DEBUG
GNU_ATTRIBUTE_ALWAYS_INLINE
#endif
static std::vector<var_idx_t> transition_to_target_type(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr target_type, AnyExprV v) {
  if (target_type != nullptr && target_type != v->inferred_type) {
    rvect = transition_expr_to_runtime_type_impl(std::move(rvect), code, v->inferred_type, target_type, v->loc);
  }
  return rvect;
}

// the second overload of the same function, invoke impl only when original and target differ
#ifndef TOLK_DEBUG
GNU_ATTRIBUTE_ALWAYS_INLINE
#endif
static std::vector<var_idx_t> transition_to_target_type(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr original_type, TypePtr target_type, SrcLocation loc) {
  if (target_type != original_type) {
    rvect = transition_expr_to_runtime_type_impl(std::move(rvect), code, original_type, target_type, loc);
  }
  return rvect;
}


std::vector<var_idx_t> pre_compile_symbol(SrcLocation loc, const Symbol* sym, CodeBlob& code, LValContext* lval_ctx) {
  if (GlobalVarPtr glob_ref = sym->try_as<GlobalVarPtr>()) {
    // handle `globalVar = rhs` / `mutate globalVar`
    if (lval_ctx && !lval_ctx->is_rval_inside_lval()) {
      std::vector<var_idx_t> lval_ir_idx = code.create_tmp_var(glob_ref->declared_type, loc, "(lval-glob)");
      lval_ctx->capture_global_modification(glob_ref, lval_ir_idx);
      return lval_ir_idx;
    }
    // `globalVar` is used for reading, just create local IR var to represent its value, Op GlobVar will fill it
    // note, that global tensors are stored as a tuple an unpacked to N vars on read, N determined by declared_type
    std::vector<var_idx_t> local_ir_idx = code.create_var(glob_ref->declared_type, loc, "g_" + glob_ref->name);
    code.emplace_back(loc, Op::_GlobVar, local_ir_idx, std::vector<var_idx_t>{}, glob_ref);
    if (lval_ctx) {   // `globalVar.0 = rhs`, globalVar is rval inside lval
      lval_ctx->capture_global_modification(glob_ref, local_ir_idx);
    }
    return local_ir_idx;
  }
  if (GlobalConstPtr const_ref = sym->try_as<GlobalConstPtr>()) {
    if (const_ref->value.is_int()) {
      std::vector<var_idx_t> rvect = code.create_tmp_var(TypeDataInt::create(), loc, "(glob-const)");
      code.emplace_back(loc, Op::_IntConst, rvect, const_ref->value.as_int());
      return rvect;
    } else {
      std::vector<var_idx_t> rvect = code.create_tmp_var(TypeDataSlice::create(), loc, "(glob-const)");
      code.emplace_back(loc, Op::_SliceConst, rvect, const_ref->value.as_slice());
      return rvect;
    }
  }
  if (FunctionPtr fun_ref = sym->try_as<FunctionPtr>()) {
    std::vector<var_idx_t> rvect = code.create_tmp_var(fun_ref->inferred_full_type, loc, "(glob-var-fun)");
    code.emplace_back(loc, Op::_GlobVar, rvect, std::vector<var_idx_t>{}, fun_ref);
    return rvect;
  }
  if (LocalVarPtr var_ref = sym->try_as<LocalVarPtr>()) {
#ifdef TOLK_DEBUG
    tolk_assert(static_cast<int>(var_ref->ir_idx.size()) == var_ref->declared_type->get_width_on_stack());
#endif
    return var_ref->ir_idx;
  }
  throw Fatal("pre_compile_symbol");
}

static std::vector<var_idx_t> process_reference(V<ast_reference> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  std::vector<var_idx_t> rvect = pre_compile_symbol(v->loc, v->sym, code, lval_ctx);

  // a local variable might be smart cast at this point, for example we're in `if (v != null)`
  // it means that we must drop the null flag (if it's a tensor), or maybe perform other stack transformations
  // (from original var_ref->ir_idx to fit smart cast)
  if (LocalVarPtr var_ref = v->sym->try_as<LocalVarPtr>()) {
    // note, inside `if (v != null)` when `v` is used for writing, v->inferred_type is an original (declared_type)
    // (smart casts apply only for rvalue, not for lvalue, we don't check it here, it's a property of inferring)
    rvect = transition_to_target_type(std::move(rvect), code, var_ref->declared_type, v->inferred_type, v->loc);
  }

  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_assignment(V<ast_assign> v, CodeBlob& code, TypePtr target_type) {
  AnyExprV lhs = v->get_lhs();
  AnyExprV rhs = v->get_rhs();

  if (auto lhs_decl = lhs->try_as<ast_local_vars_declaration>()) {
    std::vector<var_idx_t> rvect = pre_compile_let(code, lhs_decl->get_expr(), rhs, v->loc);
    return transition_to_target_type(std::move(rvect), code, target_type, v);
  } else {
    std::vector<var_idx_t> rvect = pre_compile_let(code, lhs, rhs, v->loc);
    // now rvect contains rhs IR vars constructed to fit lhs (for correct assignment, lhs type was target_type for rhs)
    // but the type of `lhs = rhs` is RHS (see type inferring), so rvect now should fit rhs->inferred_type (= v->inferred_type)
    // example: `t1 = t2 = null`, we're at `t2 = null`, earlier declared t1: `int?`, t2: `(int,int)?`
    // currently "null" matches t2 (3 null slots), but type of this assignment is "plain null" (1 slot) assigned later to t1
    rvect = transition_to_target_type(std::move(rvect), code, lhs->inferred_type, v->inferred_type, v->loc);
    return transition_to_target_type(std::move(rvect), code, target_type, v);
  }
}

static std::vector<var_idx_t> process_set_assign(V<ast_set_assign> v, CodeBlob& code, TypePtr target_type) {
  // for "a += b", emulate "a = a + b"
  // seems not beautiful, but it works; probably, this transformation should be done at AST level in advance
  std::string_view calc_operator = v->operator_name;  // "+" for operator +=
  auto v_apply = createV<ast_binary_operator>(v->loc, calc_operator, static_cast<TokenType>(v->tok - 1), v->get_lhs(), v->get_rhs());
  v_apply->assign_inferred_type(v->inferred_type);
  v_apply->assign_fun_ref(v->fun_ref);

  std::vector<var_idx_t> rvect = pre_compile_let(code, v->get_lhs(), v_apply, v->loc);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_binary_operator(V<ast_binary_operator> v, CodeBlob& code, TypePtr target_type) {
  TokenType t = v->tok;

  if (v->fun_ref) {   // almost all operators, fun_ref was assigned at type inferring
    std::vector<var_idx_t> args_vars = pre_compile_tensor(code, {v->get_lhs(), v->get_rhs()});
    std::vector<var_idx_t> rvect = gen_op_call(code, v->inferred_type, v->loc, std::move(args_vars), v->fun_ref, "(binary-op)");
    return transition_to_target_type(std::move(rvect), code, target_type, v);
  }
  if (t == tok_logical_and || t == tok_logical_or) {
    // do the following transformations:
    // a && b  ->  a ? (b != 0) : 0
    // a || b  ->  a ? 1 : (b != 0)
    AnyExprV v_0 = createV<ast_int_const>(v->loc, td::make_refint(0), "0");
    v_0->mutate()->assign_inferred_type(TypeDataInt::create());
    AnyExprV v_1 = createV<ast_int_const>(v->loc, td::make_refint(-1), "-1");
    v_1->mutate()->assign_inferred_type(TypeDataInt::create());
    auto v_b_ne_0 = createV<ast_binary_operator>(v->loc, "!=", tok_neq, v->get_rhs(), v_0);
    v_b_ne_0->mutate()->assign_inferred_type(TypeDataInt::create());
    v_b_ne_0->mutate()->assign_fun_ref(lookup_global_symbol("_!=_")->try_as<FunctionPtr>());
    std::vector<var_idx_t> cond = pre_compile_expr(v->get_lhs(), code, nullptr);
    tolk_assert(cond.size() == 1);
    std::vector<var_idx_t> rvect = code.create_tmp_var(v->inferred_type, v->loc, "(ternary)");
    Op& if_op = code.emplace_back(v->loc, Op::_If, cond);
    code.push_set_cur(if_op.block0);
    code.emplace_back(v->loc, Op::_Let, rvect, pre_compile_expr(t == tok_logical_and ? v_b_ne_0 : v_1, code, nullptr));
    code.close_pop_cur(v->loc);
    code.push_set_cur(if_op.block1);
    code.emplace_back(v->loc, Op::_Let, rvect, pre_compile_expr(t == tok_logical_and ? v_0 : v_b_ne_0, code, nullptr));
    code.close_pop_cur(v->loc);
    return transition_to_target_type(std::move(rvect), code, target_type, v);
  }

  throw UnexpectedASTNodeType(v, "process_binary_operator");
}

static std::vector<var_idx_t> process_unary_operator(V<ast_unary_operator> v, CodeBlob& code, TypePtr target_type) {
  std::vector<var_idx_t> rhs_vars = pre_compile_expr(v->get_rhs(), code, nullptr);
  std::vector<var_idx_t> rvect = gen_op_call(code, v->inferred_type, v->loc, std::move(rhs_vars), v->fun_ref, "(unary-op)");
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_ternary_operator(V<ast_ternary_operator> v, CodeBlob& code, TypePtr target_type) {
  std::vector<var_idx_t> cond = pre_compile_expr(v->get_cond(), code, nullptr);
  tolk_assert(cond.size() == 1);
  std::vector<var_idx_t> rvect = code.create_tmp_var(v->inferred_type, v->loc, "(cond)");
  
  if (v->get_cond()->is_always_true) {
    code.emplace_back(v->get_when_true()->loc, Op::_Let, rvect, pre_compile_expr(v->get_when_true(), code, v->inferred_type));
  } else if (v->get_cond()->is_always_false) {
    code.emplace_back(v->get_when_false()->loc, Op::_Let, rvect, pre_compile_expr(v->get_when_false(), code, v->inferred_type));
  } else {
    Op& if_op = code.emplace_back(v->loc, Op::_If, cond);
    code.push_set_cur(if_op.block0);
    code.emplace_back(v->get_when_true()->loc, Op::_Let, rvect, pre_compile_expr(v->get_when_true(), code, v->inferred_type));
    code.close_pop_cur(v->get_when_true()->loc);
    code.push_set_cur(if_op.block1);
    code.emplace_back(v->get_when_false()->loc, Op::_Let, rvect, pre_compile_expr(v->get_when_false(), code, v->inferred_type));
    code.close_pop_cur(v->get_when_false()->loc);
  }

  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_cast_as_operator(V<ast_cast_as_operator> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  TypePtr child_target_type = v->cast_to_type;
  std::vector<var_idx_t> rvect = pre_compile_expr(v->get_expr(), code, child_target_type, lval_ctx);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_not_null_operator(V<ast_not_null_operator> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  TypePtr child_target_type = v->get_expr()->inferred_type;
  if (const auto* as_nullable = child_target_type->try_as<TypeDataNullable>()) {
    child_target_type = as_nullable->inner;
  }
  std::vector<var_idx_t> rvect = pre_compile_expr(v->get_expr(), code, child_target_type, lval_ctx);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_is_null_check(V<ast_is_null_check> v, CodeBlob& code, TypePtr target_type) {
  std::vector<var_idx_t> expr_ir_idx = pre_compile_expr(v->get_expr(), code, nullptr);
  std::vector<var_idx_t> isnull_ir_idx = code.create_tmp_var(TypeDataBool::create(), v->loc, "(is-null)");
  TypePtr expr_type = v->get_expr()->inferred_type;

  if (const TypeDataNullable* t_nullable = expr_type->try_as<TypeDataNullable>()) {
    if (!t_nullable->is_primitive_nullable()) {
      std::vector<var_idx_t> zero_ir_idx = code.create_tmp_var(TypeDataInt::create(), v->loc, "(zero)");
      code.emplace_back(v->loc, Op::_IntConst, zero_ir_idx, td::make_refint(0));
      FunctionPtr eq_sym = lookup_global_symbol("_==_")->try_as<FunctionPtr>();
      code.emplace_back(v->loc, Op::_Call, isnull_ir_idx, std::vector{expr_ir_idx.back(), zero_ir_idx[0]}, eq_sym);
    } else {
      FunctionPtr builtin_sym = lookup_global_symbol("__isNull")->try_as<FunctionPtr>();
      code.emplace_back(v->loc, Op::_Call, isnull_ir_idx, expr_ir_idx, builtin_sym);
    }
  } else {
    bool always_null = expr_type == TypeDataNullLiteral::create();
    code.emplace_back(v->loc, Op::_IntConst, isnull_ir_idx, td::make_refint(always_null ? -1 : 0));
  }

  if (v->is_negated) {
    FunctionPtr not_sym = lookup_global_symbol("!b_")->try_as<FunctionPtr>();
    code.emplace_back(v->loc, Op::_Call, isnull_ir_idx, std::vector{isnull_ir_idx}, not_sym);
  }
  return transition_to_target_type(std::move(isnull_ir_idx), code, target_type, v);
}

static std::vector<var_idx_t> process_dot_access(V<ast_dot_access> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  // it's NOT a method call `t.tupleSize()` (since such cases are handled by process_function_call)
  // it's `t.0`, `getUser().id`, and `t.tupleSize` (as a reference, not as a call)
  if (!v->is_target_fun_ref()) {
    TypePtr obj_type = v->get_obj()->inferred_type;
    int index_at = std::get<int>(v->target);
    // `tensorVar.0`
    if (const auto* t_tensor = obj_type->try_as<TypeDataTensor>()) {
      // handle `tensorVar.0 = rhs` if tensors is a global, special case, then the global will be read on demand
      if (lval_ctx && !lval_ctx->is_rval_inside_lval()) {
        if (auto sink = calc_sink_leftmost_obj(v); sink && sink->sym->try_as<GlobalVarPtr>()) {
          std::vector<var_idx_t> lval_ir_idx = code.create_tmp_var(v->inferred_type, v->loc, "(lval-global-tensor)");
          lval_ctx->capture_field_of_global_modification(v->get_obj(), index_at, lval_ir_idx);
          return lval_ir_idx;
        }
      }
      // since a tensor of N elems are N vars on a stack actually, calculate offset
      std::vector<var_idx_t> lhs_vars = pre_compile_expr(v->get_obj(), code, nullptr, lval_ctx);
      int stack_width = t_tensor->items[index_at]->get_width_on_stack();
      int stack_offset = 0;
      for (int i = 0; i < index_at; ++i) {
        stack_offset += t_tensor->items[i]->get_width_on_stack();
      }
      std::vector<var_idx_t> rvect{lhs_vars.begin() + stack_offset, lhs_vars.begin() + stack_offset + stack_width};
      // a tensor index might be smart cast at this point, for example we're in `if (t.1 != null)`
      // it means that we must drop the null flag (if `t.1` is a tensor), or maybe perform other stack transformations
      // (from original rvect = (vars of t.1) to fit smart cast)
      rvect = transition_to_target_type(std::move(rvect), code, t_tensor->items[index_at], v->inferred_type, v->loc);
      return transition_to_target_type(std::move(rvect), code, target_type, v);
    }
    // `tupleVar.0`
    if (obj_type->try_as<TypeDataTypedTuple>() || obj_type->try_as<TypeDataTuple>()) {
      // handle `tupleVar.0 = rhs`, "0 SETINDEX" will be called when this was is modified
      if (lval_ctx && !lval_ctx->is_rval_inside_lval() && calc_sink_leftmost_obj(v)) {
        std::vector<var_idx_t> lval_ir_idx = code.create_tmp_var(v->inferred_type, v->loc, "(lval-tuple-field)");
        lval_ctx->capture_tuple_index_modification(v->get_obj(), index_at, lval_ir_idx);
        return lval_ir_idx;
      }
      // `tupleVar.0` as rvalue: the same as "tupleAt(tupleVar, 0)" written in terms of IR vars
      std::vector<var_idx_t> tuple_ir_idx = pre_compile_expr(v->get_obj(), code);
      std::vector<var_idx_t> index_ir_idx = code.create_tmp_var(TypeDataInt::create(), v->get_identifier()->loc, "(tuple-idx)");
      code.emplace_back(v->loc, Op::_IntConst, index_ir_idx, td::make_refint(index_at));
      std::vector<var_idx_t> field_ir_idx = code.create_tmp_var(v->inferred_type, v->loc, "(tuple-field)");
      tolk_assert(tuple_ir_idx.size() == 1 && field_ir_idx.size() == 1);  // tuples contain only 1-slot values
      FunctionPtr builtin_sym = lookup_global_symbol("tupleAt")->try_as<FunctionPtr>();
      code.emplace_back(v->loc, Op::_Call, field_ir_idx, std::vector{tuple_ir_idx[0], index_ir_idx[0]}, builtin_sym);
      if (lval_ctx && calc_sink_leftmost_obj(v)) {    // `tupleVar.0.1 = rhs`, then `tupleVar.0` is rval inside lval
        lval_ctx->capture_tuple_index_modification(v->get_obj(), index_at, field_ir_idx);
      }
      // like tensor index, `tupleVar.1` also might be smart cast, for example we're in `if (tupleVar.1 != null)`
      // but since tuple's elements are only 1-slot width (no tensors and unions), no stack transformations required
      return transition_to_target_type(std::move(field_ir_idx), code, target_type, v);
    }
    tolk_assert(false);
  }

  // okay, v->target refs a function, like `obj.method`, filled at type inferring
  // (currently, nothing except a global function can be referenced, no object-scope methods exist)
  FunctionPtr fun_ref = std::get<FunctionPtr>(v->target);
  tolk_assert(fun_ref);
  std::vector<var_idx_t> rvect = pre_compile_symbol(v->loc, fun_ref, code, lval_ctx);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_function_call(V<ast_function_call> v, CodeBlob& code, TypePtr target_type) {
  // v is `globalF(args)` / `globalF<int>(args)` / `obj.method(args)` / `local_var(args)` / `getF()(args)`
  FunctionPtr fun_ref = v->fun_maybe;
  if (!fun_ref) {
    // it's `local_var(args)`, treat args like a tensor:
    // 1) when variables are modified like `local_var(x, x += 2, x)`, regular mechanism of watching automatically works
    // 2) when `null` is passed to `(int, int)?`, or any other type transitions, it automatically works
    std::vector<AnyExprV> args;
    args.reserve(v->get_num_args());
    for (int i = 0; i < v->get_num_args(); ++i) {
      args.push_back(v->get_arg(i)->get_expr());
    }
    std::vector<TypePtr> params_types = v->get_callee()->inferred_type->try_as<TypeDataFunCallable>()->params_types;
    const TypeDataTensor* tensor_tt = TypeDataTensor::create(std::move(params_types))->try_as<TypeDataTensor>();
    std::vector<std::vector<var_idx_t>> vars_per_arg = pre_compile_tensor_inner(code, args, tensor_tt, nullptr);
    std::vector<var_idx_t> args_vars;
    for (const std::vector<var_idx_t>& list : vars_per_arg) {
      args_vars.insert(args_vars.end(), list.cbegin(), list.cend());
    }
    std::vector<var_idx_t> tfunc = pre_compile_expr(v->get_callee(), code, nullptr);
    tolk_assert(tfunc.size() == 1);
    args_vars.push_back(tfunc[0]);
    std::vector<var_idx_t> rvect = code.create_tmp_var(v->inferred_type, v->loc, "(call-ind)");
    Op& op = code.emplace_back(v->loc, Op::_CallInd, rvect, std::move(args_vars));
    op.set_impure_flag();
    return transition_to_target_type(std::move(rvect), code, target_type, v);
  }

  int delta_self = v->is_dot_call();
  AnyExprV obj_leftmost = nullptr;
  std::vector<AnyExprV> args;
  args.reserve(delta_self + v->get_num_args());
  if (delta_self) {
    args.push_back(v->get_dot_obj());
    obj_leftmost = v->get_dot_obj();
    while (obj_leftmost->type == ast_function_call && obj_leftmost->as<ast_function_call>()->is_dot_call() && obj_leftmost->as<ast_function_call>()->fun_maybe && obj_leftmost->as<ast_function_call>()->fun_maybe->does_return_self()) {
      obj_leftmost = obj_leftmost->as<ast_function_call>()->get_dot_obj();
    }
  }
  for (int i = 0; i < v->get_num_args(); ++i) {
    args.push_back(v->get_arg(i)->get_expr());
  }
  // the purpose of tensor_tt ("tensor target type") is to transition `null` to `(int, int)?` and so on
  // the purpose of calling `pre_compile_tensor_inner` is to have 0-th IR vars to handle return self
  std::vector<TypePtr> params_types = fun_ref->inferred_full_type->try_as<TypeDataFunCallable>()->params_types;
  const TypeDataTensor* tensor_tt = TypeDataTensor::create(std::move(params_types))->try_as<TypeDataTensor>();
  std::vector<std::vector<var_idx_t>> vars_per_arg = pre_compile_tensor_inner(code, args, tensor_tt, nullptr);

  TypePtr op_call_type = v->inferred_type;
  TypePtr real_ret_type = v->inferred_type;
  if (delta_self && fun_ref->does_return_self()) {
    real_ret_type = TypeDataVoid::create();
    if (!fun_ref->parameters[0].is_mutate_parameter()) {
      op_call_type = TypeDataVoid::create();
    }
  }
  if (fun_ref->has_mutate_params()) {
    std::vector<TypePtr> types_list;
    for (int i = 0; i < delta_self + v->get_num_args(); ++i) {
      if (fun_ref->parameters[i].is_mutate_parameter()) {
        types_list.push_back(fun_ref->parameters[i].declared_type);
      }
    }
    types_list.push_back(real_ret_type);
    op_call_type = TypeDataTensor::create(std::move(types_list));
  }

  std::vector<var_idx_t> args_vars;
  for (const std::vector<var_idx_t>& list : vars_per_arg) {
    args_vars.insert(args_vars.end(), list.cbegin(), list.cend());
  }
  std::vector<var_idx_t> rvect_apply = gen_op_call(code, op_call_type, v->loc, std::move(args_vars), fun_ref, "(fun-call)");

  if (fun_ref->has_mutate_params()) {
    LValContext local_lval;
    std::vector<var_idx_t> left;
    for (int i = 0; i < delta_self + v->get_num_args(); ++i) {
      if (fun_ref->parameters[i].is_mutate_parameter()) {
        AnyExprV arg_i = obj_leftmost && i == 0 ? obj_leftmost : args[i];
        tolk_assert(arg_i->is_lvalue || i == 0);
        if (arg_i->is_lvalue) {
          std::vector<var_idx_t> ith_var_idx = pre_compile_expr(arg_i, code, nullptr, &local_lval);
          left.insert(left.end(), ith_var_idx.begin(), ith_var_idx.end());
        } else {
          left.insert(left.end(), vars_per_arg[0].begin(), vars_per_arg[0].end());
        }
      }
    }
    std::vector<var_idx_t> rvect = code.create_tmp_var(real_ret_type, v->loc, "(fun-call)");
    left.insert(left.end(), rvect.begin(), rvect.end());
    vars_modification_watcher.trigger_callbacks(left, v->loc);
    code.emplace_back(v->loc, Op::_Let, left, rvect_apply);
    local_lval.after_let(std::move(left), code, v->loc);
    rvect_apply = rvect;
  }

  if (obj_leftmost && fun_ref->does_return_self()) {
    if (obj_leftmost->is_lvalue) {    // to handle if obj is global var, potentially re-assigned inside a chain
      rvect_apply = pre_compile_expr(obj_leftmost, code, nullptr);
    } else {                          // temporary object, not lvalue, pre_compile_expr
      rvect_apply = vars_per_arg[0];
    }
  }

  return transition_to_target_type(std::move(rvect_apply), code, target_type, v);
}

static std::vector<var_idx_t> process_tensor(V<ast_tensor> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  // tensor is compiled "as is", for example `(1, null)` occupies 2 slots
  // and if assigned/passed to something other, like `(int, (int,int)?)`, a whole tensor is transitioned, it works
  std::vector<var_idx_t> rvect = pre_compile_tensor(code, v->get_items(), lval_ctx);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_typed_tuple(V<ast_typed_tuple> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  if (lval_ctx) {       // todo some time, make "var (a, [b,c]) = (1, [2,3])" work
    v->error("[...] can not be used as lvalue here");
  }
  std::vector<var_idx_t> left = code.create_tmp_var(v->inferred_type, v->loc, "(pack-tuple)");
  std::vector<var_idx_t> right = pre_compile_tensor(code, v->get_items(), lval_ctx);
  code.emplace_back(v->loc, Op::_Tuple, left, std::move(right));
  return transition_to_target_type(std::move(left), code, target_type, v);
}

static std::vector<var_idx_t> process_int_const(V<ast_int_const> v, CodeBlob& code, TypePtr target_type) {
  std::vector<var_idx_t> rvect = code.create_tmp_var(v->inferred_type, v->loc, "(int-const)");
  code.emplace_back(v->loc, Op::_IntConst, rvect, v->intval);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_string_const(V<ast_string_const> v, CodeBlob& code, TypePtr target_type) {
  tolk_assert(v->literal_value.is_slice());
  std::vector<var_idx_t> rvect = code.create_tmp_var(v->inferred_type, v->loc, "(str-const)");
  code.emplace_back(v->loc, Op::_SliceConst, rvect, v->literal_value.as_slice());
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_bool_const(V<ast_bool_const> v, CodeBlob& code, TypePtr target_type) {
  FunctionPtr builtin_sym = lookup_global_symbol(v->bool_val ? "__true" : "__false")->try_as<FunctionPtr>();
  std::vector<var_idx_t> rvect = gen_op_call(code, v->inferred_type, v->loc, {}, builtin_sym, "(bool-const)");
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_null_keyword(V<ast_null_keyword> v, CodeBlob& code, TypePtr target_type) {
  FunctionPtr builtin_sym = lookup_global_symbol("__null")->try_as<FunctionPtr>();
  std::vector<var_idx_t> rvect = gen_op_call(code, v->inferred_type, v->loc, {}, builtin_sym, "(null-literal)");
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_local_var(V<ast_local_var_lhs> v, CodeBlob& code, TypePtr target_type) {
  if (v->marked_as_redef) {
    std::vector<var_idx_t> rvect = pre_compile_symbol(v->loc, v->var_ref, code, nullptr);
    return transition_to_target_type(std::move(rvect), code, target_type, v);
  }

  tolk_assert(v->var_ref->ir_idx.empty());
  v->var_ref->mutate()->assign_ir_idx(code.create_var(v->inferred_type, v->loc, v->var_ref->name));
  std::vector<var_idx_t> rvect = v->var_ref->ir_idx;
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_local_vars_declaration(V<ast_local_vars_declaration>, CodeBlob&) {
  // it can not appear as a standalone expression
  // `var ... = rhs` is handled by ast_assign
  tolk_assert(false);
}

static std::vector<var_idx_t> process_underscore(V<ast_underscore> v, CodeBlob& code) {
  // when _ is used as left side of assignment, like `(cs, _) = cs.loadAndReturn()`
  return code.create_tmp_var(v->inferred_type, v->loc, "(underscore)");
}

std::vector<var_idx_t> pre_compile_expr(AnyExprV v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  switch (v->type) {
    case ast_reference:
      return process_reference(v->as<ast_reference>(), code, target_type, lval_ctx);
    case ast_assign:
      return process_assignment(v->as<ast_assign>(), code, target_type);
    case ast_set_assign:
      return process_set_assign(v->as<ast_set_assign>(), code, target_type);
    case ast_binary_operator:
      return process_binary_operator(v->as<ast_binary_operator>(), code, target_type);
    case ast_unary_operator:
      return process_unary_operator(v->as<ast_unary_operator>(), code, target_type);
    case ast_ternary_operator:
      return process_ternary_operator(v->as<ast_ternary_operator>(), code, target_type);
    case ast_cast_as_operator:
      return process_cast_as_operator(v->as<ast_cast_as_operator>(), code, target_type, lval_ctx);
    case ast_not_null_operator:
      return process_not_null_operator(v->as<ast_not_null_operator>(), code, target_type, lval_ctx);
    case ast_is_null_check:
      return process_is_null_check(v->as<ast_is_null_check>(), code, target_type);
    case ast_dot_access:
      return process_dot_access(v->as<ast_dot_access>(), code, target_type, lval_ctx);
    case ast_function_call:
      return process_function_call(v->as<ast_function_call>(), code, target_type);
    case ast_parenthesized_expression:
      return pre_compile_expr(v->as<ast_parenthesized_expression>()->get_expr(), code, target_type, lval_ctx);
    case ast_tensor:
      return process_tensor(v->as<ast_tensor>(), code, target_type, lval_ctx);
    case ast_typed_tuple:
      return process_typed_tuple(v->as<ast_typed_tuple>(), code, target_type, lval_ctx);
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
    default:
      throw UnexpectedASTNodeType(v, "pre_compile_expr");
  }
}


static void process_sequence(V<ast_sequence> v, CodeBlob& code) {
  for (AnyV item : v->get_items()) {
    process_any_statement(item, code);
  }
}

static void process_assert_statement(V<ast_assert_statement> v, CodeBlob& code) {
  std::vector<AnyExprV> args(3);
  if (auto v_not = v->get_cond()->try_as<ast_unary_operator>(); v_not && v_not->tok == tok_logical_not) {
    args[0] = v->get_thrown_code();
    args[1] = v->get_cond()->as<ast_unary_operator>()->get_rhs();
    args[2] = createV<ast_bool_const>(v->loc, true);
    args[2]->mutate()->assign_inferred_type(TypeDataInt::create());
  } else {
    args[0] = v->get_thrown_code();
    args[1] = v->get_cond();
    args[2] = createV<ast_bool_const>(v->loc, false);
    args[2]->mutate()->assign_inferred_type(TypeDataInt::create());
  }

  FunctionPtr builtin_sym = lookup_global_symbol("__throw_if_unless")->try_as<FunctionPtr>();
  std::vector<var_idx_t> args_vars = pre_compile_tensor(code, args);
  gen_op_call(code, TypeDataVoid::create(), v->loc, std::move(args_vars), builtin_sym, "(throw-call)");
}

static void process_catch_variable(AnyExprV v_catch_var, CodeBlob& code) {
  if (auto v_ref = v_catch_var->try_as<ast_reference>(); v_ref && v_ref->sym) { // not underscore
    LocalVarPtr var_ref = v_ref->sym->try_as<LocalVarPtr>();
    tolk_assert(var_ref->ir_idx.empty());
    var_ref->mutate()->assign_ir_idx(code.create_var(v_catch_var->inferred_type, v_catch_var->loc, var_ref->name));
  }
}

static void process_try_catch_statement(V<ast_try_catch_statement> v, CodeBlob& code) {
  code.require_callxargs = true;
  Op& try_catch_op = code.emplace_back(v->loc, Op::_TryCatch);
  code.push_set_cur(try_catch_op.block0);
  process_any_statement(v->get_try_body(), code);
  code.close_pop_cur(v->get_try_body()->loc_end);
  code.push_set_cur(try_catch_op.block1);

  // transform catch (excNo, arg) into TVM-catch (arg, excNo), where arg is untyped and thus almost useless now
  const std::vector<AnyExprV>& catch_vars = v->get_catch_expr()->get_items();
  tolk_assert(catch_vars.size() == 2);
  process_catch_variable(catch_vars[0], code);
  process_catch_variable(catch_vars[1], code);
  try_catch_op.left = pre_compile_tensor(code, {catch_vars[1], catch_vars[0]});
  process_any_statement(v->get_catch_body(), code);
  code.close_pop_cur(v->get_catch_body()->loc_end);
}

static void process_repeat_statement(V<ast_repeat_statement> v, CodeBlob& code) {
  std::vector<var_idx_t> tmp_vars = pre_compile_expr(v->get_cond(), code, nullptr);
  Op& repeat_op = code.emplace_back(v->loc, Op::_Repeat, tmp_vars);
  code.push_set_cur(repeat_op.block0);
  process_any_statement(v->get_body(), code);
  code.close_pop_cur(v->get_body()->loc_end);
}

static void process_if_statement(V<ast_if_statement> v, CodeBlob& code) {
  std::vector<var_idx_t> cond = pre_compile_expr(v->get_cond(), code, nullptr);
  tolk_assert(cond.size() == 1);

  if (v->get_cond()->is_always_true) {
    process_any_statement(v->get_if_body(), code);      // v->is_ifnot does not matter here
    return;
  }
  if (v->get_cond()->is_always_false) {
    process_any_statement(v->get_else_body(), code);
    return;
  }

  Op& if_op = code.emplace_back(v->loc, Op::_If, std::move(cond));
  code.push_set_cur(if_op.block0);
  process_any_statement(v->get_if_body(), code);
  code.close_pop_cur(v->get_if_body()->loc_end);
  code.push_set_cur(if_op.block1);
  process_any_statement(v->get_else_body(), code);
  code.close_pop_cur(v->get_else_body()->loc_end);
  if (v->is_ifnot) {
    std::swap(if_op.block0, if_op.block1);
  }
}

static void process_do_while_statement(V<ast_do_while_statement> v, CodeBlob& code) {
  Op& until_op = code.emplace_back(v->loc, Op::_Until);
  code.push_set_cur(until_op.block0);
  process_any_statement(v->get_body(), code);

  // in TVM, there is only "do until", but in Tolk, we want "do while"
  // here we negate condition to pass it forward to legacy to Op::_Until
  // also, handle common situations as a hardcoded "optimization": replace (a<0) with (a>=0) and so on
  // todo these hardcoded conditions should be removed from this place in the future
  AnyExprV cond = v->get_cond();
  AnyExprV until_cond;
  if (auto v_not = cond->try_as<ast_unary_operator>(); v_not && v_not->tok == tok_logical_not) {
    until_cond = v_not->get_rhs();
  } else if (auto v_eq = cond->try_as<ast_binary_operator>(); v_eq && v_eq->tok == tok_eq) {
    until_cond = createV<ast_binary_operator>(cond->loc, "!=", tok_neq, v_eq->get_lhs(), v_eq->get_rhs());
  } else if (auto v_neq = cond->try_as<ast_binary_operator>(); v_neq && v_neq->tok == tok_neq) {
    until_cond = createV<ast_binary_operator>(cond->loc, "==", tok_eq, v_neq->get_lhs(), v_neq->get_rhs());
  } else if (auto v_leq = cond->try_as<ast_binary_operator>(); v_leq && v_leq->tok == tok_leq) {
    until_cond = createV<ast_binary_operator>(cond->loc, ">", tok_gt, v_leq->get_lhs(), v_leq->get_rhs());
  } else if (auto v_lt = cond->try_as<ast_binary_operator>(); v_lt && v_lt->tok == tok_lt) {
    until_cond = createV<ast_binary_operator>(cond->loc, ">=", tok_geq, v_lt->get_lhs(), v_lt->get_rhs());
  } else if (auto v_geq = cond->try_as<ast_binary_operator>(); v_geq && v_geq->tok == tok_geq) {
    until_cond = createV<ast_binary_operator>(cond->loc, "<", tok_lt, v_geq->get_lhs(), v_geq->get_rhs());
  } else if (auto v_gt = cond->try_as<ast_binary_operator>(); v_gt && v_gt->tok == tok_gt) {
    until_cond = createV<ast_binary_operator>(cond->loc, "<=", tok_geq, v_gt->get_lhs(), v_gt->get_rhs());
  } else if (cond->inferred_type == TypeDataBool::create()) {
    until_cond = createV<ast_unary_operator>(cond->loc, "!b", tok_logical_not, cond);
  } else {
    until_cond = createV<ast_unary_operator>(cond->loc, "!", tok_logical_not, cond);
  }
  until_cond->mutate()->assign_inferred_type(TypeDataInt::create());
  if (auto v_bin = until_cond->try_as<ast_binary_operator>(); v_bin && !v_bin->fun_ref) {
    v_bin->mutate()->assign_fun_ref(lookup_global_symbol("_" + static_cast<std::string>(v_bin->operator_name) + "_")->try_as<FunctionPtr>());
  } else if (auto v_un = until_cond->try_as<ast_unary_operator>(); v_un && !v_un->fun_ref) {
    v_un->mutate()->assign_fun_ref(lookup_global_symbol(static_cast<std::string>(v_un->operator_name) + "_")->try_as<FunctionPtr>());
  }

  until_op.left = pre_compile_expr(until_cond, code, nullptr);
  tolk_assert(until_op.left.size() == 1);
  code.close_pop_cur(v->get_body()->loc_end);
}

static void process_while_statement(V<ast_while_statement> v, CodeBlob& code) {
  Op& while_op = code.emplace_back(v->loc, Op::_While);
  code.push_set_cur(while_op.block0);
  while_op.left = pre_compile_expr(v->get_cond(), code, nullptr);
  tolk_assert(while_op.left.size() == 1);
  code.close_pop_cur(v->get_body()->loc);
  code.push_set_cur(while_op.block1);
  process_any_statement(v->get_body(), code);
  code.close_pop_cur(v->get_body()->loc_end);
}

static void process_throw_statement(V<ast_throw_statement> v, CodeBlob& code) {
  if (v->has_thrown_arg()) {
    FunctionPtr builtin_sym = lookup_global_symbol("__throw_arg")->try_as<FunctionPtr>();
    std::vector<var_idx_t> args_vars = pre_compile_tensor(code, {v->get_thrown_arg(), v->get_thrown_code()});
    gen_op_call(code, TypeDataVoid::create(), v->loc, std::move(args_vars), builtin_sym, "(throw-call)");
  } else {
    FunctionPtr builtin_sym = lookup_global_symbol("__throw")->try_as<FunctionPtr>();
    std::vector<var_idx_t> args_vars = pre_compile_tensor(code, {v->get_thrown_code()});
    gen_op_call(code, TypeDataVoid::create(), v->loc, std::move(args_vars), builtin_sym, "(throw-call)");
  }
}

static void process_return_statement(V<ast_return_statement> v, CodeBlob& code) {
  std::vector<var_idx_t> return_vars;
  if (v->has_return_value()) {
    TypePtr child_target_type = code.fun_ref->inferred_return_type;
    if (code.fun_ref->does_return_self()) {
      child_target_type = code.fun_ref->parameters[0].declared_type;
    }
    return_vars = pre_compile_expr(v->get_return_value(), code, child_target_type);
  }
  if (code.fun_ref->does_return_self()) {
    return_vars = {};
  }
  if (code.fun_ref->has_mutate_params()) {
    std::vector<var_idx_t> mutated_vars;
    for (const LocalVarData& p_sym: code.fun_ref->parameters) {
      if (p_sym.is_mutate_parameter()) {
        mutated_vars.insert(mutated_vars.end(), p_sym.ir_idx.begin(), p_sym.ir_idx.end());
      }
    }
    return_vars.insert(return_vars.begin(), mutated_vars.begin(), mutated_vars.end());
  }
  code.emplace_back(v->loc, Op::_Return, std::move(return_vars));
}

// append "return" (void) to the end of the function
// if it's not reachable, it will be dropped
// (IR cfg reachability may differ from FlowContext in case of "never" types, so there may be situations,
//  when IR will consider this "return" reachable and leave it, but actually execution will never reach it)
static void append_implicit_return_statement(SrcLocation loc_end, CodeBlob& code) {
  std::vector<var_idx_t> mutated_vars;
  if (code.fun_ref->has_mutate_params()) {
    for (const LocalVarData& p_sym: code.fun_ref->parameters) {
      if (p_sym.is_mutate_parameter()) {
        mutated_vars.insert(mutated_vars.end(), p_sym.ir_idx.begin(), p_sym.ir_idx.end());
      }
    }
  }
  code.emplace_back(loc_end, Op::_Return, std::move(mutated_vars));
}


void process_any_statement(AnyV v, CodeBlob& code) {
  switch (v->type) {
    case ast_sequence:
      return process_sequence(v->as<ast_sequence>(), code);
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
  auto v_body = fun_ref->ast_root->as<ast_function_declaration>()->get_body()->as<ast_sequence>();
  CodeBlob* blob = new CodeBlob{fun_ref->name, fun_ref->loc, fun_ref};

  std::vector<var_idx_t> rvect_import;
  int total_arg_width = 0;
  for (int i = 0; i < fun_ref->get_num_params(); ++i) {
    total_arg_width += fun_ref->parameters[i].declared_type->get_width_on_stack();
  }
  rvect_import.reserve(total_arg_width);

  for (int i = 0; i < fun_ref->get_num_params(); ++i) {
    const LocalVarData& param_i = fun_ref->parameters[i];
    std::vector<var_idx_t> ir_idx = blob->create_var(param_i.declared_type, param_i.loc, param_i.name);
    rvect_import.insert(rvect_import.end(), ir_idx.begin(), ir_idx.end());
    param_i.mutate()->assign_ir_idx(std::move(ir_idx));
  }
  blob->emplace_back(fun_ref->loc, Op::_Import, rvect_import);
  blob->in_var_cnt = blob->var_cnt;
  tolk_assert(blob->var_cnt == total_arg_width);

  for (AnyV item : v_body->get_items()) {
    process_any_statement(item, *blob);
  }
  append_implicit_return_statement(v_body->loc_end, *blob);

  blob->close_blk(v_body->loc_end);
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
          asm_ops.push_back(AsmOp::Parse(op, cnt, width));
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
      asm_ops.push_back(AsmOp::Parse(op, cnt, width));
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

    // example: `fun f(a: int, b: (int, (int, int)), c: int)` with `asm (b a c)`
    // current arg_order is [1 0 2]
    // needs to be converted to [1 2 3 0 4] because b width is 3
    if (has_arg_width_not_1) {
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
        v_function->get_body()->error("ret_order (after ->) expected to contain " + std::to_string(expected_width) + " numbers");
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
    if (fun_ref->is_code_function()) {
      convert_function_body_to_CodeBlob(fun_ref, std::get<FunctionBodyCode*>(fun_ref->body));
    } else if (fun_ref->is_asm_function()) {
      convert_asm_body_to_AsmOp(fun_ref, std::get<FunctionBodyAsm*>(fun_ref->body));
    }
  }
};

void pipeline_convert_ast_to_legacy_Expr_Op() {
  visit_ast_of_all_functions<UpdateArgRetOrderConsideringStackWidth>();
  visit_ast_of_all_functions<ConvertASTToLegacyOpVisitor>();
}

} // namespace tolk
