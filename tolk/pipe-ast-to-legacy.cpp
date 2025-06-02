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
#include "ast-aux-data.h"
#include "ast-visitor.h"
#include "type-system.h"
#include "common/refint.h"
#include "smart-casts-cfg.h"
#include "pack-unpack-api.h"
#include "generics-helpers.h"
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
 *   Example: `var a: int|slice = 5`. This `5` should be extended as "5 1" (5 for value, 1 for type_id of `int`).
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


static int calc_offset_on_stack(const TypeDataTensor* t_tensor, int index_at) {
  int stack_offset = 0;
  for (int i = 0; i < index_at; ++i) {
    stack_offset += t_tensor->items[i]->get_width_on_stack();
  }
  return stack_offset;
}

static int calc_offset_on_stack(const TypeDataStruct* t_struct, int field_idx) {
  int stack_offset = 0;
  for (int i = 0; i < field_idx; ++i) {
    stack_offset += t_struct->struct_ref->fields[i]->declared_type->get_width_on_stack();
  }
  return stack_offset;
}


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
    AnyExprV tensor_obj;      // it's a tensor or struct
    int index_at;             // for tensors, it's index_at; for structs, it's field_idx
    std::vector<var_idx_t> lval_ir_idx;

    void apply(CodeBlob& code, SrcLocation loc) const {
      LValContext local_lval;
      local_lval.enter_rval_inside_lval();
      std::vector<var_idx_t> obj_ir_idx = pre_compile_expr(tensor_obj, code, nullptr, &local_lval);

      int stack_width, stack_offset;
      TypePtr obj_type = tensor_obj->inferred_type->unwrap_alias();
      if (const TypeDataTensor* t_tensor = obj_type->try_as<TypeDataTensor>()) {
        stack_width = t_tensor->items[index_at]->get_width_on_stack();
        stack_offset = calc_offset_on_stack(t_tensor, index_at);
      } else if (const TypeDataStruct* t_struct = obj_type->try_as<TypeDataStruct>()) {
        stack_width = t_struct->struct_ref->get_field(index_at)->declared_type->get_width_on_stack();
        stack_offset = calc_offset_on_stack(t_struct, index_at);
      } else {
        tolk_assert(false);
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
      FunctionPtr builtin_sym = lookup_function("tuple.set");
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

// the purpose of this class is having a call `f(a1,a2,...)` when f has asm arg_order, to check
// whether it's safe to rearrange arguments (to evaluate them in arg_order right here for fewer stack manipulations)
// or it's unsafe, and we should evaluate them left-to-right;
// example: `f(1,2,3)` / `b.storeUint(2,32)` is safe;
// example: `f(x,x+=5,x)` / `f(impureF1(), global_var)` / `f(s.loadInt(), s.loadInt())` is unsafe;
// the same rules are used to check an object literal: is it safe to convert `{y:expr, x:expr}` to declaration order {x,y}
class CheckReorderingForAsmArgOrderIsSafeVisitor final : public ASTVisitorFunctionBody {
  bool has_side_effects = false;

protected:
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
    if (!v_dot->is_target_indexed_access() && !v_dot->is_target_struct_field()) {
      break;
    }
    leftmost_obj = unwrap_not_null_operator(v_dot->get_obj());
  }
  return leftmost_obj->kind == ast_reference ? leftmost_obj->as<ast_reference>() : nullptr;
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
                                          LValContext* lval_ctx = nullptr, const TypeDataTensor* tensor_target_type = nullptr) {
  if (tensor_target_type == nullptr) {
    std::vector<TypePtr> types_list;
    types_list.reserve(args.size());
    for (AnyExprV item : args) {
      types_list.push_back(item->inferred_type);
    }
    tensor_target_type = TypeDataTensor::create(std::move(types_list))->try_as<TypeDataTensor>();
  }
  std::vector<std::vector<var_idx_t>> res_lists = pre_compile_tensor_inner(code, args, tensor_target_type, lval_ctx);
  std::vector<var_idx_t> res;
  for (const std::vector<var_idx_t>& list : res_lists) {
    res.insert(res.end(), list.cbegin(), list.cend());
  }
  return res;
}

static std::vector<var_idx_t> pre_compile_let(CodeBlob& code, AnyExprV lhs, AnyExprV rhs, SrcLocation loc) {
  // [lhs] = [rhs]; since type checking is ok, it's the same as "lhs = rhs"
  if (lhs->kind == ast_bracket_tuple && rhs->kind == ast_bracket_tuple) {
    // note: there are no type transitions (adding nullability flag, etc.), since only 1-slot elements allowed in tuples
    LValContext local_lval;
    std::vector<var_idx_t> left = pre_compile_tensor(code, lhs->as<ast_bracket_tuple>()->get_items(), &local_lval);
    vars_modification_watcher.trigger_callbacks(left, loc);
    std::vector<var_idx_t> rvect = pre_compile_tensor(code, rhs->as<ast_bracket_tuple>()->get_items());
    code.emplace_back(loc, Op::_Let, left, rvect);
    local_lval.after_let(std::move(left), code, loc);
    std::vector<var_idx_t> right = code.create_tmp_var(TypeDataTuple::create(), loc, "(tuple)");
    code.emplace_back(lhs->loc, Op::_Tuple, right, std::move(rvect));
    return right;
  }
  // [lhs] = rhs; it's un-tuple to N left vars
  if (lhs->kind == ast_bracket_tuple) {
    LValContext local_lval;
    std::vector<var_idx_t> left = pre_compile_tensor(code, lhs->as<ast_bracket_tuple>()->get_items(), &local_lval);
    vars_modification_watcher.trigger_callbacks(left, loc);
    std::vector<var_idx_t> right = pre_compile_expr(rhs, code, nullptr);
    const TypeDataBrackets* inferred_tuple = rhs->inferred_type->unwrap_alias()->try_as<TypeDataBrackets>();
    std::vector<TypePtr> types_list = inferred_tuple->items;
    std::vector<var_idx_t> rvect = code.create_tmp_var(TypeDataTensor::create(std::move(types_list)), rhs->loc, "(unpack-tuple)");
    code.emplace_back(lhs->loc, Op::_UnTuple, rvect, std::move(right));
    code.emplace_back(loc, Op::_Let, left, rvect);
    local_lval.after_let(std::move(left), code, loc);
    return right;
  }
  // small optimization: `var x = rhs` or `local_var = rhs` (90% cases), LValContext not needed actually
  if (lhs->kind == ast_local_var_lhs || (lhs->kind == ast_reference && lhs->as<ast_reference>()->sym->try_as<LocalVarPtr>())) {
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

std::vector<var_idx_t> pre_compile_is_type(CodeBlob& code, TypePtr expr_type, TypePtr cmp_type, const std::vector<var_idx_t>& expr_ir_idx, SrcLocation loc, const char* debug_desc) {
  FunctionPtr eq_sym = lookup_function("_==_");
  FunctionPtr isnull_sym = lookup_function("__isNull");
  FunctionPtr not_sym = lookup_function("!b_");
  std::vector<var_idx_t> result_ir_idx = code.create_tmp_var(TypeDataBool::create(), loc, debug_desc);

  const TypeDataUnion* lhs_union = expr_type->try_as<TypeDataUnion>();
  if (!lhs_union) {
    // `int` is `int` / `int` is `builder`, it's compile-time, either 0, or -1
    bool types_eq = expr_type->get_type_id() == cmp_type->get_type_id();
    code.emplace_back(loc, Op::_IntConst, result_ir_idx, td::make_refint(types_eq ? -1 : 0));
  } else if (lhs_union->is_primitive_nullable() && cmp_type == TypeDataNullLiteral::create()) {
    // `int?` is `null` for primitive 1-slot nullables, they hold either value of TVM NULL, no extra union tag slot
    code.emplace_back(loc, Op::_Call, result_ir_idx, expr_ir_idx, isnull_sym);
  } else if (lhs_union->is_primitive_nullable()) {
    // `int?` is `int` (check for null actually) / `int?` is `builder` (compile-time false actually)
    bool cant_happen = lhs_union->or_null->get_type_id() != cmp_type->get_type_id();
    if (cant_happen) {
      code.emplace_back(loc, Op::_IntConst, result_ir_idx, td::make_refint(0));
    } else {
      code.emplace_back(loc, Op::_Call, result_ir_idx, expr_ir_idx, isnull_sym);
      code.emplace_back(loc, Op::_Call, result_ir_idx, result_ir_idx, not_sym);
    }
  } else {
    // `int | slice` is `int`, check type id
    std::vector<var_idx_t> typeid_ir_idx = code.create_tmp_var(TypeDataInt::create(), loc, "(type-id)");
    code.emplace_back(loc, Op::_IntConst, typeid_ir_idx, td::make_refint(cmp_type->get_type_id()));
    code.emplace_back(loc, Op::_Call, result_ir_idx, std::vector{typeid_ir_idx[0], expr_ir_idx.back()}, eq_sym);
  }

  return result_ir_idx;
}

static std::vector<var_idx_t> gen_op_call(CodeBlob& code, TypePtr ret_type, SrcLocation loc,
                                          std::vector<var_idx_t>&& args_vars, FunctionPtr fun_ref, const char* debug_desc,
                                          bool arg_order_already_equals_asm = false) {
  std::vector<var_idx_t> rvect = code.create_tmp_var(ret_type, loc, debug_desc);
  Op& op = code.emplace_back(loc, Op::_Call, rvect, std::move(args_vars), fun_ref);
  if (!fun_ref->is_marked_as_pure()) {
    op.set_impure_flag();
  }
  if (arg_order_already_equals_asm) {
    op.set_arg_order_already_equals_asm_flag();
  }
  return rvect;
}

static std::vector<var_idx_t> gen_compile_time_code_instead_of_fun_call(CodeBlob& code, SrcLocation loc, const std::vector<std::vector<var_idx_t>>& vars_per_arg, FunctionPtr called_f) {
  if (called_f->is_instantiation_of_generic_function()) {
    std::string_view f_name = called_f->base_fun_ref->name;
    TypePtr typeT = called_f->substitutedTs->typeT_at(0);

    if (f_name == "T.toCell") {
      // in: object T, out: Cell<T> (just a cell, wrapped)
      std::vector ir_obj = vars_per_arg[0];
      return generate_pack_struct_to_cell(code, loc, typeT, std::move(ir_obj), vars_per_arg[1]);
    }
    if (f_name == "T.fromCell") {
      // in: cell, out: object T
      std::vector ir_cell = vars_per_arg[0];
      return generate_unpack_struct_from_cell(code, loc, typeT, std::move(ir_cell), vars_per_arg[1]);
    }
    if (f_name == "T.fromSlice") {
      // in: slice, out: object T, input slice NOT mutated
      std::vector ir_slice = vars_per_arg[0];
      return generate_unpack_struct_from_slice(code, loc, typeT, std::move(ir_slice), false, vars_per_arg[1]);
    }
    if (f_name == "Cell<T>.load") {
      // in: cell, out: object T
      std::vector ir_cell = vars_per_arg[0];
      return generate_unpack_struct_from_cell(code, loc, typeT, std::move(ir_cell), vars_per_arg[1]);
    }
    if (f_name == "slice.loadAny") {
      // in: slice, out: object T, input slice is mutated, so prepend self before an object
      var_idx_t ir_self = vars_per_arg[0][0];
      std::vector ir_slice = vars_per_arg[0];
      std::vector ir_obj = generate_unpack_struct_from_slice(code, loc, typeT, std::move(ir_slice), true, vars_per_arg[1]);
      std::vector ir_result = {ir_self};
      ir_result.insert(ir_result.end(), ir_obj.begin(), ir_obj.end());
      return ir_result;
    }
    if (f_name == "slice.skipAny") {
      // in: slice, out: the same slice, with a shifted pointer
      std::vector ir_slice = vars_per_arg[0];
      return generate_skip_struct_in_slice(code, loc, typeT, std::move(ir_slice), vars_per_arg[1]);
    }
    if (f_name == "builder.storeAny") {
      // in: builder and object T, out: mutated builder
      std::vector ir_builder = vars_per_arg[0];
      std::vector ir_obj = vars_per_arg[1];
      return generate_pack_struct_to_builder(code, loc, typeT, std::move(ir_builder), std::move(ir_obj), vars_per_arg[2]);
    }
    if (f_name == "T.estimatePackSize") {
      return generate_estimate_size_call(code, loc, typeT);
    }
  }

  tolk_assert(false);
}

// "Transition to target (runtime) type" is the following process.
// Imagine `fun analyze(t: (int,int)?)` and a call `analyze((1,2))`.
// `(1,2)` (inferred_type) is 2 stack slots, but `t` (target_type) is 3 (one for null-flag).
// So, this null flag should be implicitly added (non-zero, since a variable is not null).
// Another example: `var t: (int, int)? = null`.
// `null` (inferred_type) is 1 stack slots, but target_type is 3, we should add 2 nulls.
// Another example: `var t1 = (1, null); var t2: (int, (int,int)?) = t1;`.
// Then t1's rvect is 2 vars (1 and null), but t1's `null` should be converted to 3 stack slots (resulting in 4 total).
// The same mechanism works for union types, but there is a union tag (UTag) slot instead of null flag.
// Another example: `var i: int|slice = 5;`. This "5" is represented as "5 1" (5 for value, 1 is type_id of `int`).
GNU_ATTRIBUTE_NOINLINE
static std::vector<var_idx_t> transition_expr_to_runtime_type_impl(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr original_type, TypePtr target_type, SrcLocation loc) {
#ifdef TOLK_DEBUG
  tolk_assert(static_cast<int>(rvect.size()) == original_type->get_width_on_stack());
#endif

  original_type = original_type->unwrap_alias();
  target_type = target_type->unwrap_alias();

  // pass `T` to `T`
  // could occur for passing tensor `(..., T, ...)` to `(..., T, ...)` while traversing tensor's components
  if (target_type == original_type) {
    return rvect;
  }

  int target_w = target_type->get_width_on_stack();
  int orig_w = original_type->get_width_on_stack();   // = rvect.size()
  const TypeDataUnion* t_union = target_type->try_as<TypeDataUnion>();
  const TypeDataUnion* o_union = original_type->try_as<TypeDataUnion>();

  // most common case, simple nullability:
  // - `int` to `int?`
  // - `null` to `int?`
  // - `int8?` to `int16?`
  // - `null` to `StructWith1Int?`
  // in general, pass `T1` to `T2?` when `T2?` still occupies 1 stack slot (value or TVM NULL)
  if (t_union && t_union->is_primitive_nullable() && orig_w == 1) {
    // rvect has 1 slot, either value or TVM NULL
    return rvect;
  }

  // smart cast of a primitive 1-slot nullable:
  // - `int?` to `int`
  // - `int?` to `null`
  // - `StructWith1Int?` to `null`
  // this value (one slot) is either a TVM primitive or TVM NULL at runtime
  if (o_union && o_union->is_primitive_nullable() && target_w == 1) {
    // rvect has 1 slot, but its contents is compile-time guaranteed to match target_type
    return rvect;
  }

  // pass `T` to `never`
  // it occurs due to smart cast, in unreachable branches, for example `if (intVal == null) { return intVal; }`
  // we can't do anything reasonable here, but (hopefully) execution will never reach this point, and stack won't be polluted
  if (target_type == TypeDataNever::create() || original_type == TypeDataNever::create() || target_type == TypeDataUnknown::create()) {
    return rvect;
  }

  // smart cast to a primitive 1-slot nullable:
  // - `int | slice | null` to `slice?`
  // - `A | int | null` to `int?`
  // so, originally a type occupies N slots, but needs to be converted to 1 slot
  if (t_union && t_union->is_primitive_nullable() && orig_w > 0) {
    // nothing except "T1 | T2 | ... null" can be cast to 1-slot nullable `T1?`
    tolk_assert(o_union && o_union->has_null() && o_union->has_variant_with_type_id(t_union->or_null));
    // here we exploit rvect shape, how union types and multi-slot nullables are stored on a stack
    // `T1 | T2 | ... | null` occupies N+1 slots, where the last is for UTag
    // when it holds null value, N slots are null, and UTag slot is 0 (it's type_id of TypeDataNullLiteral)
    return {rvect[rvect.size() - 2]};
  }

  // pass `null` to `T?` when T is wide (stores some nulls and UTag=0 at runtime)
  // - `null` to `(int, int)?`
  // - `null` to `int | slice | null`
  // to represent a non-primitive null value, we need N nulls + 1 null flag (UTag=0, type_id of TypeDataNullLiteral)
  if (t_union && target_w > 1 && original_type == TypeDataNullLiteral::create()) {
    tolk_assert(t_union->has_null());
    FunctionPtr null_sym = lookup_function("__null");
    rvect.reserve(target_w);      // keep rvect[0], it's already null
    for (int i = 1; i < target_w - 1; ++i) {
      std::vector<var_idx_t> ith_null = code.create_tmp_var(TypeDataNullLiteral::create(), loc, "(null-literal)");
      code.emplace_back(loc, Op::_Call, ith_null, std::vector<var_idx_t>{}, null_sym);
      rvect.push_back(ith_null[0]);
    }
    std::vector<var_idx_t> last_null = code.create_tmp_var(TypeDataInt::create(), loc, "(UTag)");
    code.emplace_back(loc, Op::_IntConst, last_null, td::make_refint(0));
    rvect.push_back(last_null[0]);
    return rvect;
  }

  // pass `null` to nullable empty type
  // - `null` to `()?`
  // - `null` to `EmptyStruct?`
  // - `null` to `Empty1 | Empty2 | null`
  // so, rvect contains TVM NULL, but instead, we should push UTag=0
  if (t_union && original_type == TypeDataNullLiteral::create()) {
    tolk_assert(t_union->has_null() && target_w == 1);
    std::vector<var_idx_t> new_rvect = code.create_tmp_var(TypeDataInt::create(), loc, "(UTag)");
    code.emplace_back(loc, Op::_IntConst, new_rvect, td::make_refint(0));
    return new_rvect;
  }

  // smart cast of a wide nullable union to plain `null`
  // - `(int, int)?` to `null`
  // - `int | slice | null` to `null`
  if (o_union && target_type == TypeDataNullLiteral::create() && orig_w > 1) {
    tolk_assert(o_union->has_null());
    // if we are here, it's guaranteed that original value holds null
    // it means, that its shape is N nulls + 1 UTag (equals 0)
    return {rvect[rvect.size() - 2]};
  }

  // smart cast of nullable empty tensor to plain `null`
  // - `()?` to `null`
  // - `EmptyStruct?` to `null`
  // - `Empty1 | Empty2 | null` to `null`
  // so, rvect contains UTag, we need TVM NULL
  if (o_union && target_type == TypeDataNullLiteral::create()) {
    tolk_assert(orig_w == 1 && o_union->has_null());
    FunctionPtr null_sym = lookup_function("__null");
    std::vector<var_idx_t> new_rvect = code.create_tmp_var(TypeDataNullLiteral::create(), loc, "(null-literal)");
    code.emplace_back(loc, Op::_Call, new_rvect, std::vector<var_idx_t>{}, null_sym);
    return new_rvect;
  }

  // pass primitive 1-slot `T?` to a wider nullable union
  // - `int?` to `int | slice | null`
  // - `slice?` to `(int, int) | slice | builder | null`
  // so, originally `T?` is 1-slot, but needs to be converted to N+1 slots, keeping its value
  if (o_union && o_union->is_primitive_nullable() && t_union) {
    tolk_assert(t_union->has_null() && t_union->has_variant_with_type_id(o_union->or_null) && target_w > 1);
    // the transformation is tricky:
    // when value is null, we need to achieve "... (null) 0"         (value is already null, so "... value 0")
    // when value is not null, we need to get "... value {type_id}"
    // this can be done only via IFs at runtime; luckily, this case is very uncommon in practice
    // for "...", we might need N-1 nulls: `int?` to `(int,int,int) | int | null` is `(null) (null) value/(null) 0/1`
    FunctionPtr null_sym = lookup_function("__null");
    std::vector<var_idx_t> new_rvect;
    new_rvect.resize(target_w);
    for (int i = 0; i < target_w - 2; ++i) {    // N-1 nulls
      std::vector<var_idx_t> ith_null = code.create_tmp_var(TypeDataNullLiteral::create(), loc, "(null-literal)");
      code.emplace_back(loc, Op::_Call, ith_null, std::vector<var_idx_t>{}, null_sym);
      new_rvect[i] = ith_null[0];
    }
    new_rvect[target_w - 2] = rvect[0];   // value
    new_rvect[target_w - 1] = code.create_tmp_var(TypeDataInt::create(), loc, "(UTag)")[0];

    std::vector<var_idx_t> eq_null_cond = code.create_tmp_var(TypeDataBool::create(), loc, "(value-is-null)");
    FunctionPtr isnull_sym = lookup_function("__isNull");
    code.emplace_back(loc, Op::_Call, eq_null_cond, rvect, isnull_sym);
    Op& if_op = code.emplace_back(loc, Op::_If, eq_null_cond);
    code.push_set_cur(if_op.block0);
    code.emplace_back(loc, Op::_IntConst, std::vector{new_rvect[target_w - 1]}, td::make_refint(0));
    code.close_pop_cur(loc);
    code.push_set_cur(if_op.block1);
    code.emplace_back(loc, Op::_IntConst, std::vector{new_rvect[target_w - 1]}, td::make_refint(o_union->or_null->get_type_id()));
    code.close_pop_cur(loc);
    return new_rvect;
  }

  // extend a single type into a union type
  // - `int` to `int | slice`
  // - `int` to `int | (int, int) | null`
  // - `(int, int)` to `(int, int, cell) | builder | (int, int)`
  // - `(int, null)` to `(int, (int, int)?) | ...`: mind transition
  // - `(int, null)` to `(int, int | slice | null) | ...`: mind transition
  // so, probably need to prepend some nulls, and need to append UTag
  if (t_union && !o_union) {
    TypePtr t_subtype = t_union->calculate_exact_variant_to_fit_rhs(original_type);
    tolk_assert(t_subtype && target_w > t_subtype->get_width_on_stack());
    rvect = transition_expr_to_runtime_type_impl(std::move(rvect), code, original_type, t_subtype, loc);
    std::vector<var_idx_t> prepend_nulls;
    prepend_nulls.reserve(target_w - t_subtype->get_width_on_stack() - 1);
    for (int i = 0; i < target_w - t_subtype->get_width_on_stack() - 1; ++i) {
      FunctionPtr null_sym = lookup_function("__null");
      std::vector<var_idx_t> ith_null = code.create_tmp_var(TypeDataNullLiteral::create(), loc, "(UVar.null)");
      prepend_nulls.push_back(ith_null[0]);
      code.emplace_back(loc, Op::_Call, std::move(ith_null), std::vector<var_idx_t>{}, null_sym);
    }
    rvect.insert(rvect.begin(), prepend_nulls.begin(), prepend_nulls.end());

    std::vector<var_idx_t> last_utag = code.create_tmp_var(TypeDataInt::create(), loc, "(UTag)");
    code.emplace_back(loc, Op::_IntConst, last_utag, td::make_refint(t_subtype->get_type_id()));
    rvect.push_back(last_utag[0]);
    return rvect;
  }

  // smart cast a union type to a single type
  // - `int | slice` to `int`
  // - `int | (int, int) | null` to `int`
  // - `(int, (int, int)?) | ...` to `(int, null)`: mind transition
  // so, cut off UTag and probably some unused tags from the start
  if (o_union && !t_union) {
    TypePtr o_subtype = o_union->calculate_exact_variant_to_fit_rhs(target_type);
    tolk_assert(o_subtype && orig_w > o_subtype->get_width_on_stack());
    rvect = std::vector(rvect.begin() + orig_w - o_subtype->get_width_on_stack() - 1, rvect.end() - 1);
    rvect = transition_expr_to_runtime_type_impl(std::move(rvect), code, o_subtype, target_type, loc);
    return rvect;
  }

  // extend a union type to a wider one
  // - `int | slice` to `int | slice | builder`
  // - `int | slice` to `int | (int, int) | slice | null`
  // so, both original and target have UTag slot, but rvect probably needs to be prepended by nulls
  if (t_union && o_union && t_union->size() >= o_union->size()) {
    tolk_assert(target_w >= orig_w && t_union->has_all_variants_of(o_union));
    std::vector<var_idx_t> prepend_nulls;
    prepend_nulls.reserve(target_w - orig_w);
    for (int i = 0; i < target_w - orig_w; ++i) {
      FunctionPtr null_sym = lookup_function("__null");
      std::vector<var_idx_t> ith_null_ir_idx = code.create_tmp_var(TypeDataNullLiteral::create(), loc, "(UVar.null)");
      prepend_nulls.push_back(ith_null_ir_idx[0]);
      code.emplace_back(loc, Op::_Call, std::move(ith_null_ir_idx), std::vector<var_idx_t>{}, null_sym);
    }
    rvect.insert(rvect.begin(), prepend_nulls.begin(), prepend_nulls.end());
    return rvect;
  }

  // smart cast a wider union type to a narrow one
  // - `int | slice | builder` to `int | slice`
  // - `int | (int, int) | slice | null` to `int | slice`
  // so, both original and target have UTag slot, but rvect needs to be cut off from the left
  if (t_union && o_union) {
    tolk_assert(target_w <= orig_w && o_union->has_all_variants_of(t_union));
    rvect = std::vector(rvect.begin() + (orig_w - target_w), rvect.end());
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
  if (target_type->try_as<TypeDataBrackets>() && original_type->try_as<TypeDataBrackets>()) {
    tolk_assert(target_w == 1 && orig_w == 1);
    return rvect;
  }

  // pass callable to callable
  // their types aren't exactly equal, but they match (containing aliases, for example)
  if (original_type->try_as<TypeDataFunCallable>() && target_type->try_as<TypeDataFunCallable>()) {
    tolk_assert(rvect.size() == 1);
    return rvect;
  }

  // pass struct A to struct B
  // different structs are typically not assignable, but Wrapper<WrapperAlias<int>> is ok to Wrapper<Wrapper<int>>
  if (original_type->try_as<TypeDataStruct>() && target_type->try_as<TypeDataStruct>()) {
    tolk_assert(target_type->can_rhs_be_assigned(original_type) && orig_w == target_w);
    return rvect;
  }

  // pass slice to address
  // no changes in rvect: address is TVM slice under the hood
  if (original_type == TypeDataSlice::create() && target_type == TypeDataAddress::create()) {
    return rvect;
  }

  // pass address to slice
  // same, no changes in rvect
  if (original_type == TypeDataAddress::create() && target_type == TypeDataSlice::create()) {
    return rvect;
  }

  // pass `Cell<Something>` to `cell`, e.g. `setContractData(obj.toCell())`
  if (target_type == TypeDataCell::create() && original_type->try_as<TypeDataStruct>()) {
    tolk_assert(orig_w == 1 && original_type->try_as<TypeDataStruct>()->struct_ref->is_instantiation_of_generic_struct());
    return rvect;
  }
  // and vice versa, `cell as Cell<Something>`
  if (original_type == TypeDataCell::create() && target_type->try_as<TypeDataStruct>()) {
    tolk_assert(target_w == 1 && target_type->try_as<TypeDataStruct>()->struct_ref->is_instantiation_of_generic_struct());
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
std::vector<var_idx_t> transition_to_target_type(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr original_type, TypePtr target_type, SrcLocation loc) {
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
    tolk_assert(lval_ctx == nullptr);
    ASTAuxData* aux_data = new AuxData_ForceFiftLocation(loc);
    auto v_force_loc = createV<ast_artificial_aux_vertex>(loc, const_ref->init_value, aux_data, const_ref->inferred_type);
    return pre_compile_expr(v_force_loc, code, const_ref->declared_type);
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
    v_b_ne_0->mutate()->assign_fun_ref(lookup_function("_!=_"));
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
  if (t == tok_eq || t == tok_neq) {
    if (v->get_lhs()->inferred_type->unwrap_alias() == TypeDataAddress::create() && v->get_rhs()->inferred_type->unwrap_alias() == TypeDataAddress::create()) {
      FunctionPtr f_sliceEq = lookup_function("slice.bitsEqual");
      std::vector<var_idx_t> ir_lhs_slice = pre_compile_expr(v->get_lhs(), code);
      std::vector<var_idx_t> ir_rhs_slice = pre_compile_expr(v->get_rhs(), code);
      std::vector<var_idx_t> rvect = code.create_tmp_var(TypeDataBool::create(), v->loc, "(addr-eq)");
      code.emplace_back(v->loc, Op::_Call, rvect, std::vector{ir_lhs_slice[0], ir_rhs_slice[0]}, f_sliceEq);
      if (t == tok_neq) {
        FunctionPtr not_sym = lookup_function("!b_");
        code.emplace_back(v->loc, Op::_Call, rvect, rvect, not_sym);
      }
      return transition_to_target_type(std::move(rvect), code, target_type, v);
    }
  }

  throw UnexpectedASTNodeKind(v, "process_binary_operator");
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
  TypePtr child_target_type = v->type_node->resolved_type;
  std::vector<var_idx_t> rvect = pre_compile_expr(v->get_expr(), code, child_target_type, lval_ctx);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_is_type_operator(V<ast_is_type_operator> v, CodeBlob& code, TypePtr target_type) {
  TypePtr lhs_type = v->get_expr()->inferred_type->unwrap_alias();
  TypePtr cmp_type = v->type_node->resolved_type->unwrap_alias();
  bool is_null_check = cmp_type == TypeDataNullLiteral::create();    // `v == null`, not `v is T`
  tolk_assert(!cmp_type->try_as<TypeDataUnion>());  // `v is int|slice` is a type checker error

  std::vector<var_idx_t> expr_ir_idx = pre_compile_expr(v->get_expr(), code, nullptr);
  std::vector<var_idx_t> result_ir_idx = pre_compile_is_type(code, lhs_type, cmp_type, expr_ir_idx, v->loc, is_null_check ? "(is-null)" : "(is-type)");

  if (v->is_negated) {
    FunctionPtr not_sym = lookup_function("!b_");
    code.emplace_back(v->loc, Op::_Call, result_ir_idx, result_ir_idx, not_sym);
  }
  return transition_to_target_type(std::move(result_ir_idx), code, target_type, v);
}

static std::vector<var_idx_t> process_not_null_operator(V<ast_not_null_operator> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  TypePtr expr_type = v->get_expr()->inferred_type->unwrap_alias();
  TypePtr without_null_type = calculate_type_subtract_rhs_type(expr_type, TypeDataNullLiteral::create());
  TypePtr child_target_type = without_null_type != TypeDataNever::create() ? without_null_type : expr_type;

  std::vector<var_idx_t> rvect = pre_compile_expr(v->get_expr(), code, child_target_type, lval_ctx);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_match_expression(V<ast_match_expression> v, CodeBlob& code, TypePtr target_type) {
  TypePtr lhs_type = v->get_subject()->inferred_type->unwrap_alias();

  int n_arms = v->get_arms_count();
  std::vector<var_idx_t> subj_ir_idx = pre_compile_expr(v->get_subject(), code, nullptr);
  std::vector<var_idx_t> result_ir_idx = code.create_tmp_var(v->inferred_type, v->loc, "(match-expression)");

  if (!n_arms) {    // `match (subject) {}`
    tolk_assert(v->is_statement());
    return {};
  }

  // it's either `match` by type (all arms patterns are types) or `match` by expression
  bool is_match_by_type = v->get_arm(0)->pattern_kind == MatchArmKind::exact_type;
  // detect whether `match` is exhaustive
  bool is_exhaustive = is_match_by_type   // match by type covers all cases, checked earlier
       || !v->is_statement()              // match by expression is guaranteely exhaustive, checked earlier
       || v->get_arm(n_arms - 1)->pattern_kind == MatchArmKind::else_branch;

  // example 1 (exhaustive): `match (v) { int => ... slice => ... builder => ... }`
  // construct nested IFs: IF is int { ... } ELSE { IF is slice { ... } ELSE { ... } }
  // example 2 (exhaustive): `match (v) { -1 => ... 0 => ... else => ... }`
  // construct nested IFs: IF is int { ... } ELSE { IF is slice { ... } ELSE { ... } }
  // example 3 (not exhaustive): `match (v) { -1 => ... 0 => ... 1 => ... }`
  // construct nested IFs: IF == -1 { ... } ELSE { IF == 0 { ... } ELSE { IF == 1 { ... } } }
  FunctionPtr eq_sym = lookup_function("_==_");
  for (int i = 0; i < n_arms - is_exhaustive; ++i) {
    auto v_ith_arm = v->get_arm(i);
    std::vector<var_idx_t> eq_ith_ir_idx;
    if (is_match_by_type) {
      TypePtr cmp_type = v_ith_arm->pattern_type_node->resolved_type->unwrap_alias();
      tolk_assert(!cmp_type->try_as<TypeDataUnion>());  // `match` over `int|slice` is a type checker error
      eq_ith_ir_idx = pre_compile_is_type(code, lhs_type, cmp_type, subj_ir_idx, v_ith_arm->loc, "(arm-cond-eq)");
    } else {
      std::vector<var_idx_t> ith_ir_idx = pre_compile_expr(v_ith_arm->get_pattern_expr(), code);
      tolk_assert(subj_ir_idx.size() == 1 && ith_ir_idx.size() == 1);
      eq_ith_ir_idx = code.create_tmp_var(TypeDataBool::create(), v_ith_arm->loc, "(arm-cond-eq)");
      code.emplace_back(v_ith_arm->loc, Op::_Call, eq_ith_ir_idx, std::vector{subj_ir_idx[0], ith_ir_idx[0]}, eq_sym);
    }
    Op& if_op = code.emplace_back(v_ith_arm->loc, Op::_If, std::move(eq_ith_ir_idx));
    code.push_set_cur(if_op.block0);
    if (v->is_statement()) {
      pre_compile_expr(v_ith_arm->get_body(), code);
    } else {
      std::vector<var_idx_t> arm_ir_idx = pre_compile_expr(v_ith_arm->get_body(), code, v->inferred_type);
      code.emplace_back(v->loc, Op::_Let, result_ir_idx, std::move(arm_ir_idx));
    }
    code.close_pop_cur(v->loc);
    code.push_set_cur(if_op.block1);    // open ELSE
  }

  if (is_exhaustive) {
    // we're inside the last ELSE
    auto v_last_arm = v->get_arm(n_arms - 1);
    if (v->is_statement()) {
      pre_compile_expr(v_last_arm->get_body(), code);
    } else {
      std::vector<var_idx_t> arm_ir_idx = pre_compile_expr(v_last_arm->get_body(), code, v->inferred_type);
      code.emplace_back(v->loc, Op::_Let, result_ir_idx, std::move(arm_ir_idx));
    }
  }
  // close all outer IFs
  for (int i = 0; i < n_arms - is_exhaustive; ++i) {
    code.close_pop_cur(v->loc);
  }

  return transition_to_target_type(std::move(result_ir_idx), code, target_type, v);
}

static std::vector<var_idx_t> process_dot_access(V<ast_dot_access> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  // it's NOT a method call `t.tupleSize()` (since such cases are handled by process_function_call)
  // it's `t.0`, `getUser().id`, and `t.tupleSize` (as a reference, not as a call)
  if (!v->is_target_fun_ref()) {
    TypePtr obj_type = v->get_obj()->inferred_type->unwrap_alias();
    // `user.id`; internally, a struct (an object) is a tensor
    if (const auto* t_struct = obj_type->try_as<TypeDataStruct>()) {
      StructFieldPtr field_ref = std::get<StructFieldPtr>(v->target);
      // handle `globalObj.field = rhs`, special case, then the global will be read on demand
      if (lval_ctx && !lval_ctx->is_rval_inside_lval()) {
        if (auto sink = calc_sink_leftmost_obj(v); sink && sink->sym->try_as<GlobalVarPtr>()) {
          std::vector<var_idx_t> lval_ir_idx = code.create_tmp_var(v->inferred_type, v->loc, "(lval-global-field)");
          lval_ctx->capture_field_of_global_modification(v->get_obj(), field_ref->field_idx, lval_ir_idx);
          return lval_ir_idx;
        }
      }
      std::vector<var_idx_t> lhs_vars = pre_compile_expr(v->get_obj(), code, nullptr, lval_ctx);
      int stack_width = field_ref->declared_type->get_width_on_stack();
      int stack_offset = calc_offset_on_stack(t_struct, field_ref->field_idx);
      std::vector<var_idx_t> rvect{lhs_vars.begin() + stack_offset, lhs_vars.begin() + stack_offset + stack_width};
      // an object field might be smart cast at this point, for example we're in `if (user.t != null)`
      // it means that we must drop the null flag (if `user.t` is a tensor), or maybe perform other stack transformations
      // (from original rvect = (vars of user.t) to fit smart cast)
      rvect = transition_to_target_type(std::move(rvect), code, field_ref->declared_type, v->inferred_type, v->loc);
      return transition_to_target_type(std::move(rvect), code, target_type, v);
    }
    // `tensorVar.0`
    if (const auto* t_tensor = obj_type->try_as<TypeDataTensor>()) {
      int index_at = std::get<int>(v->target);
      // handle `globalTensorVar.0 = rhs`, special case, then the global will be read on demand
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
      int stack_offset = calc_offset_on_stack(t_tensor, index_at);
      std::vector<var_idx_t> rvect{lhs_vars.begin() + stack_offset, lhs_vars.begin() + stack_offset + stack_width};
      // a tensor index might be smart cast at this point, for example we're in `if (t.1 != null)`
      // it means that we must drop the null flag (if `t.1` is a tensor), or maybe perform other stack transformations
      // (from original rvect = (vars of t.1) to fit smart cast)
      rvect = transition_to_target_type(std::move(rvect), code, t_tensor->items[index_at], v->inferred_type, v->loc);
      return transition_to_target_type(std::move(rvect), code, target_type, v);
    }
    // `tupleVar.0`
    if (obj_type->try_as<TypeDataBrackets>() || obj_type->try_as<TypeDataTuple>()) {
      int index_at = std::get<int>(v->target);
      // handle `tupleVar.0 = rhs`, "0 SETINDEX" will be called when this was is modified
      if (lval_ctx && !lval_ctx->is_rval_inside_lval() && calc_sink_leftmost_obj(v)) {
        std::vector<var_idx_t> lval_ir_idx = code.create_tmp_var(v->inferred_type, v->loc, "(lval-tuple-field)");
        lval_ctx->capture_tuple_index_modification(v->get_obj(), index_at, lval_ir_idx);
        return lval_ir_idx;
      }
      // `tupleVar.0` as rvalue: the same as "tuple.get(tupleVar, 0)" written in terms of IR vars
      std::vector<var_idx_t> tuple_ir_idx = pre_compile_expr(v->get_obj(), code);
      std::vector<var_idx_t> index_ir_idx = code.create_tmp_var(TypeDataInt::create(), v->get_identifier()->loc, "(tuple-idx)");
      code.emplace_back(v->loc, Op::_IntConst, index_ir_idx, td::make_refint(index_at));
      std::vector<var_idx_t> field_ir_idx = code.create_tmp_var(v->inferred_type, v->loc, "(tuple-field)");
      tolk_assert(tuple_ir_idx.size() == 1 && field_ir_idx.size() == 1);  // tuples contain only 1-slot values
      FunctionPtr builtin_sym = lookup_function("tuple.get");
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
    std::vector<TypePtr> params_types = v->get_callee()->inferred_type->unwrap_alias()->try_as<TypeDataFunCallable>()->params_types;
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

  // fill args for evaluation: dot object + passed arguments + parameters defaults if not all passed
  AnyExprV obj_leftmost = v->get_self_obj();
  int delta_self = obj_leftmost != nullptr;
  std::vector<AnyExprV> args;
  args.reserve(fun_ref->get_num_params());
  if (delta_self) {
    args.push_back(obj_leftmost);
    while (obj_leftmost->kind == ast_function_call && obj_leftmost->as<ast_function_call>()->get_self_obj() && obj_leftmost->as<ast_function_call>()->fun_maybe && obj_leftmost->as<ast_function_call>()->fun_maybe->does_return_self()) {
      obj_leftmost = obj_leftmost->as<ast_function_call>()->get_self_obj();
    }
  }
  for (int i = 0; i < v->get_num_args(); ++i) {
    args.push_back(v->get_arg(i)->get_expr());
  }
  for (int i = delta_self + v->get_num_args(); i < fun_ref->get_num_params(); ++i) {
    LocalVarPtr param_ref = &fun_ref->get_param(i);
    tolk_assert(param_ref->has_default_value());
    SrcLocation last_loc = args.empty() ? v->loc : args.back()->loc;
    ASTAuxData *aux_data = new AuxData_ForceFiftLocation(last_loc);
    auto v_force_loc = createV<ast_artificial_aux_vertex>(last_loc, param_ref->default_value, aux_data, param_ref->declared_type);
    args.push_back(v_force_loc);
  }

  // the purpose of tensor_tt ("tensor target type") is to transition `null` to `(int, int)?` and so on
  // the purpose of calling `pre_compile_tensor_inner` is to have 0-th IR vars to handle return self
  std::vector<TypePtr> params_types = fun_ref->inferred_full_type->try_as<TypeDataFunCallable>()->params_types;

  // if fun_ref has asm arg_order, maybe it's safe to swap arguments here (to put them onto a stack in the right way);
  // (if it's not safe, arguments are evaluated left-to-right, involving stack transformations later)
  bool arg_order_already_equals_asm = false;
  int asm_self_idx = 0;
  if (!fun_ref->arg_order.empty() && CheckReorderingForAsmArgOrderIsSafeVisitor::is_safe_to_reorder(v)) {
    std::vector<AnyExprV> new_args(args.size());
    std::vector<TypePtr> new_params_types(params_types.size());
    for (int i = 0; i < static_cast<int>(fun_ref->arg_order.size()); ++i) {
      int real_i = fun_ref->arg_order[i];
      new_args[i] = args[real_i];
      new_params_types[i] = params_types[real_i];
      if (real_i == 0) {
        asm_self_idx = i;
      }
    }
    args = std::move(new_args);
    params_types = std::move(new_params_types);
    arg_order_already_equals_asm = true;
  }

  const TypeDataTensor* tensor_tt = TypeDataTensor::create(std::move(params_types))->try_as<TypeDataTensor>();
  std::vector<std::vector<var_idx_t>> vars_per_arg = pre_compile_tensor_inner(code, args, tensor_tt, nullptr);

  TypePtr op_call_type = v->inferred_type;
  TypePtr real_ret_type = v->inferred_type;
  if (obj_leftmost && fun_ref->does_return_self()) {
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
  std::vector<var_idx_t> rvect_apply = fun_ref->is_compile_time_special_gen()
      ? gen_compile_time_code_instead_of_fun_call(code, v->loc, vars_per_arg, fun_ref)
      : gen_op_call(code, op_call_type, v->loc, std::move(args_vars), fun_ref, "(fun-call)", arg_order_already_equals_asm);

  if (fun_ref->has_mutate_params()) {
    LValContext local_lval;
    std::vector<var_idx_t> left;
    for (int i = 0; i < delta_self + v->get_num_args(); ++i) {
      int real_i = arg_order_already_equals_asm ? i == 0 && delta_self ? asm_self_idx : fun_ref->arg_order[i - delta_self] : i;
      if (fun_ref->parameters[i].is_mutate_parameter()) {
        AnyExprV arg_i = obj_leftmost && i == 0 ? obj_leftmost : args[real_i];
        tolk_assert(arg_i->is_lvalue || i == 0);
        if (arg_i->is_lvalue) {
          std::vector<var_idx_t> ith_var_idx = pre_compile_expr(arg_i, code, nullptr, &local_lval);
          left.insert(left.end(), ith_var_idx.begin(), ith_var_idx.end());
        } else {
          left.insert(left.end(), vars_per_arg[asm_self_idx].begin(), vars_per_arg[asm_self_idx].end());
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
      rvect_apply = vars_per_arg[asm_self_idx];
    }
  }

  return transition_to_target_type(std::move(rvect_apply), code, target_type, v);
}

static std::vector<var_idx_t> process_braced_expression(V<ast_braced_expression> v, CodeBlob& code, TypePtr target_type) {
  // `{ ... }` used as an expression can not return a value currently (there is no syntax in a language)
  // that's why it can appear only in special places, and its usage correctness has been checked
  tolk_assert(v->inferred_type == TypeDataVoid::create() || v->inferred_type == TypeDataNever::create());
  process_any_statement(v->get_block_statement(), code);
  static_cast<void>(target_type);
  return {};
}

static std::vector<var_idx_t> process_tensor(V<ast_tensor> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  // tensor is compiled "as is", for example `(1, null)` occupies 2 slots
  // and if assigned/passed to something other, like `(int, (int,int)?)`, a whole tensor is transitioned, it works
  std::vector<var_idx_t> rvect = pre_compile_tensor(code, v->get_items(), lval_ctx);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_typed_tuple(V<ast_bracket_tuple> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  if (lval_ctx) {       // todo some time, make "var (a, [b,c]) = (1, [2,3])" work
    v->error("[...] can not be used as lvalue here");
  }
  std::vector<var_idx_t> left = code.create_tmp_var(v->inferred_type, v->loc, "(pack-tuple)");
  std::vector<var_idx_t> right = pre_compile_tensor(code, v->get_items(), lval_ctx);
  code.emplace_back(v->loc, Op::_Tuple, left, std::move(right));
  return transition_to_target_type(std::move(left), code, target_type, v);
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
  const auto* tensor_target_type = TypeDataTensor::create(std::move(target_types))->try_as<TypeDataTensor>();
  std::vector<var_idx_t> literal_rvect = pre_compile_tensor(code, tensor_items, lval_ctx, tensor_target_type);

  std::vector<var_idx_t> rvect = code.create_tmp_var(TypeDataStruct::create(v->struct_ref), v->loc, "(object)");
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
        code.emplace_back(v->loc, Op::_Let, std::move(field_rvect), std::move(literal_field_rvect));
        break;
      }
      tensor_offset += tensor_item_width;
    }
    if (exists_in_literal || field_ref->declared_type == TypeDataNever::create()) {
      continue;
    }

    tolk_assert(field_ref->has_default_value());
    SrcLocation last_loc = v->get_body()->empty() ? v->loc : v->get_body()->get_all_fields().back()->loc;
    ASTAuxData *aux_data = new AuxData_ForceFiftLocation(last_loc);
    auto v_force_loc = createV<ast_artificial_aux_vertex>(v->loc, field_ref->default_value, aux_data, field_ref->declared_type);
    std::vector def_rvect = pre_compile_expr(v_force_loc, code, field_ref->declared_type);
    code.emplace_back(v->loc, Op::_Let, std::move(field_rvect), std::move(def_rvect));
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

  SrcLocation prev_loc = v->loc;
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
      if (field_ref->declared_type == TypeDataNever::create()) {
        continue;   // field of `never` type can be missed out of object literal (useful in generics defaults)
      }             // (it occupies 0 slots, nothing is assignable to it — like this field is missing from a struct)
      tolk_assert(field_ref->has_default_value());
      ASTAuxData *aux_data = new AuxData_ForceFiftLocation(prev_loc);
      auto v_force_loc = createV<ast_artificial_aux_vertex>(v->loc, field_ref->default_value, aux_data, field_ref->declared_type);
      v_init_val = v_force_loc;   // it may be a complex expression, but it's a constant, checked earlier
    }
    tensor_items.push_back(v_init_val);
    target_types.push_back(field_ref->declared_type);
    prev_loc = v_init_val->loc;
  }
  const auto* tensor_target_type = TypeDataTensor::create(std::move(target_types))->try_as<TypeDataTensor>();
  std::vector<var_idx_t> rvect = pre_compile_tensor(code, tensor_items, lval_ctx, tensor_target_type);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_int_const(V<ast_int_const> v, CodeBlob& code, TypePtr target_type) {
  std::vector<var_idx_t> rvect = code.create_tmp_var(v->inferred_type, v->loc, "(int-const)");
  code.emplace_back(v->loc, Op::_IntConst, rvect, v->intval);
  // here, like everywhere, even for just `int`, there might be a potential transition due to union types
  // example: passing `1` to `int | slice` puts actually "1 5" on a stack (1 for value, 5 for UTag = type_id of `int`)
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_string_const(V<ast_string_const> v, CodeBlob& code, TypePtr target_type) {
  std::vector<var_idx_t> rvect = code.create_tmp_var(v->inferred_type, v->loc, "(str-const)");
  code.emplace_back(v->loc, Op::_SliceConst, rvect, v->literal_value);
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_bool_const(V<ast_bool_const> v, CodeBlob& code, TypePtr target_type) {
  FunctionPtr builtin_sym = lookup_function(v->bool_val ? "__true" : "__false");
  std::vector<var_idx_t> rvect = gen_op_call(code, v->inferred_type, v->loc, {}, builtin_sym, "(bool-const)");
  return transition_to_target_type(std::move(rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_null_keyword(V<ast_null_keyword> v, CodeBlob& code, TypePtr target_type) {
  FunctionPtr builtin_sym = lookup_function("__null");
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
  // `var rhs: int lateinit` is ast_local_var_lhs
  tolk_assert(false);
}

static std::vector<var_idx_t> process_underscore(V<ast_underscore> v, CodeBlob& code) {
  // when _ is used as left side of assignment, like `(cs, _) = cs.loadAndReturn()`
  return code.create_tmp_var(v->inferred_type, v->loc, "(underscore)");
}

static std::vector<var_idx_t> process_empty_expression(V<ast_empty_expression> v, CodeBlob& code, TypePtr target_type) {
  std::vector<var_idx_t> empty_rvect;
  return transition_to_target_type(std::move(empty_rvect), code, target_type, v);
}

static std::vector<var_idx_t> process_artificial_aux_vertex(V<ast_artificial_aux_vertex> v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  if (const auto* data = dynamic_cast<const AuxData_ForceFiftLocation*>(v->aux_data)) {
    SrcLocation backup = code.forced_loc;
    code.forced_loc = data->forced_loc;
    std::vector<var_idx_t> rvect = pre_compile_expr(v->get_wrapped_expr(), code, target_type, lval_ctx);
    code.forced_loc = backup;
    return rvect;
  }

  tolk_assert(false);
}

std::vector<var_idx_t> pre_compile_expr(AnyExprV v, CodeBlob& code, TypePtr target_type, LValContext* lval_ctx) {
  switch (v->kind) {
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
    case ast_is_type_operator:
      return process_is_type_operator(v->as<ast_is_type_operator>(), code, target_type);
    case ast_not_null_operator:
      return process_not_null_operator(v->as<ast_not_null_operator>(), code, target_type, lval_ctx);
    case ast_match_expression:
      return process_match_expression(v->as<ast_match_expression>(), code, target_type);
    case ast_dot_access:
      return process_dot_access(v->as<ast_dot_access>(), code, target_type, lval_ctx);
    case ast_function_call:
      return process_function_call(v->as<ast_function_call>(), code, target_type);
    case ast_parenthesized_expression:
      return pre_compile_expr(v->as<ast_parenthesized_expression>()->get_expr(), code, target_type, lval_ctx);
    case ast_braced_expression:
      return process_braced_expression(v->as<ast_braced_expression>(), code, target_type);
    case ast_tensor:
      return process_tensor(v->as<ast_tensor>(), code, target_type, lval_ctx);
    case ast_bracket_tuple:
      return process_typed_tuple(v->as<ast_bracket_tuple>(), code, target_type, lval_ctx);
    case ast_object_literal:
      return process_object_literal(v->as<ast_object_literal>(), code, target_type, lval_ctx);
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
      return process_artificial_aux_vertex(v->as<ast_artificial_aux_vertex>(), code, target_type, lval_ctx);
    default:
      throw UnexpectedASTNodeKind(v, "pre_compile_expr");
  }
}


static void process_block_statement(V<ast_block_statement> v, CodeBlob& code) {
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

  FunctionPtr builtin_sym = lookup_function("__throw_if_unless");
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
  } else if (cond->inferred_type->unwrap_alias() == TypeDataBool::create()) {
    until_cond = createV<ast_unary_operator>(cond->loc, "!b", tok_logical_not, cond);
  } else {
    until_cond = createV<ast_unary_operator>(cond->loc, "!", tok_logical_not, cond);
  }
  until_cond->mutate()->assign_inferred_type(TypeDataInt::create());
  if (auto v_bin = until_cond->try_as<ast_binary_operator>(); v_bin && !v_bin->fun_ref) {
    v_bin->mutate()->assign_fun_ref(lookup_function("_" + static_cast<std::string>(v_bin->operator_name) + "_"));
  } else if (auto v_un = until_cond->try_as<ast_unary_operator>(); v_un && !v_un->fun_ref) {
    v_un->mutate()->assign_fun_ref(lookup_function(static_cast<std::string>(v_un->operator_name) + "_"));
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
    FunctionPtr builtin_sym = lookup_function("__throw_arg");
    std::vector<var_idx_t> args_vars = pre_compile_tensor(code, {v->get_thrown_arg(), v->get_thrown_code()});
    gen_op_call(code, TypeDataVoid::create(), v->loc, std::move(args_vars), builtin_sym, "(throw-call)");
  } else {
    FunctionPtr builtin_sym = lookup_function("__throw");
    std::vector<var_idx_t> args_vars = pre_compile_tensor(code, {v->get_thrown_code()});
    gen_op_call(code, TypeDataVoid::create(), v->loc, std::move(args_vars), builtin_sym, "(throw-call)");
  }
}

static void process_return_statement(V<ast_return_statement> v, CodeBlob& code) {
  TypePtr child_target_type = code.fun_ref->inferred_return_type;
  if (code.fun_ref->does_return_self()) {
    child_target_type = code.fun_ref->parameters[0].declared_type;
  }
  std::vector<var_idx_t> return_vars = pre_compile_expr(v->get_return_value(), code, child_target_type);

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
          asm_ops.push_back(AsmOp::Parse({}, op, cnt, width));
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
      asm_ops.push_back(AsmOp::Parse({}, op, cnt, width));
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
