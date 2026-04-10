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
#include "tolk.h"
#include "compilation-errors.h"
#include "compiler-settings.h"
#include "type-system.h"

namespace tolk {

// functions returning "never" are assumed to interrupt flow
// for instance, variables after their call aren't considered used
// its main purpose is `throw` statement, it's a call to a built-in `__throw` function
static bool does_function_always_throw(FunctionPtr fun_ref) {
  return fun_ref->inferred_return_type == TypeDataNever::create();
}

/*
 *  
 *   ANALYZE AND PREPROCESS ABSTRACT CODE
 * 
 */

// Backward dataflow analysis: computes var_info for every Op in the code tree.
// After this pass, each op's var_info describes which variables are alive (needed) right before that op.
// The terminal Nop's var_info becomes the "exit" info of the block.
bool CodeBlob::compute_used_code_vars() {
  VarDescrList empty_var_info;
  // pass edit=true: ops whose results are entirely unused get disabled
  return ops.compute_used_code_vars(empty_var_info, true);
}

bool OpList::compute_used_code_vars(const VarDescrList& var_info, bool edit) {
  // the last element is always a trailing _Nop that receives the outgoing var_info
  tolk_assert(!empty() && back()->cl == Op::_Nop);

  bool changed = back()->set_var_info(var_info);
  for (int i = static_cast<int>(size()) - 2; i >= 0; --i) {
    const VarDescrList& next_var_info = list[i + 1]->var_info;
    // bitwise | is used to execute both left and right parts
    changed = static_cast<int>(changed) | static_cast<int>(list[i]->compute_used_vars(edit, next_var_info));
  }
  return changed;
}

bool operator==(const VarDescrList& x, const VarDescrList& y) {
  if (x.size() != y.size()) {
    return false;
  }
  for (std::size_t i = 0; i < x.size(); i++) {
    if (x.list[i].idx != y.list[i].idx || x.list[i].flags != y.list[i].flags) {
      return false;
    }
  }
  return true;
}

bool same_values(const VarDescr& x, const VarDescr& y) {
  if (x.val != y.val || x.is_int_const() != y.is_int_const()) {
    return false;
  }
  if (x.is_int_const() && *x.int_const != *y.int_const) {
    return false;
  }
  return true;
}

bool same_values(const VarDescrList& x, const VarDescrList& y) {
  if (x.size() != y.size()) {
    return false;
  }
  for (std::size_t i = 0; i < x.size(); i++) {
    if (x.list[i].idx != y.list[i].idx || !same_values(x.list[i], y.list[i])) {
      return false;
    }
  }
  return true;
}

bool Op::set_var_info(const VarDescrList& new_var_info) {
  if (var_info == new_var_info) {
    return false;
  }
  var_info = new_var_info;
  return true;
}

bool Op::set_var_info(VarDescrList&& new_var_info) {
  if (var_info == new_var_info) {
    return false;
  }
  var_info = std::move(new_var_info);
  return true;
}

bool Op::set_var_info_except(const VarDescrList& new_var_info, const std::vector<var_idx_t>& var_list) {
  if (!var_list.size()) {
    return set_var_info(new_var_info);
  }
  VarDescrList tmp_info{new_var_info};
  tmp_info -= var_list;
  return set_var_info(tmp_info);
}

bool Op::set_var_info_except(VarDescrList&& new_var_info, const std::vector<var_idx_t>& var_list) {
  if (var_list.size()) {
    new_var_info -= var_list;
  }
  return set_var_info(std::move(new_var_info));
}
std::vector<var_idx_t> sort_unique_vars(const std::vector<var_idx_t>& var_list) {
  std::vector<var_idx_t> vars{var_list}, unique_vars;
  std::sort(vars.begin(), vars.end());
  vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
  return vars;
}

VarDescr* VarDescrList::operator[](var_idx_t idx) {
  auto it = std::lower_bound(list.begin(), list.end(), idx);
  return it != list.end() && it->idx == idx ? &*it : nullptr;
}

const VarDescr* VarDescrList::operator[](var_idx_t idx) const {
  auto it = std::lower_bound(list.begin(), list.end(), idx);
  return it != list.end() && it->idx == idx ? &*it : nullptr;
}

std::size_t VarDescrList::count(const std::vector<var_idx_t> idx_list) const {
  std::size_t res = 0;
  for (var_idx_t idx : idx_list) {
    if (operator[](idx)) {
      ++res;
    }
  }
  return res;
}

std::size_t VarDescrList::count_used(const std::vector<var_idx_t> idx_list) const {
  std::size_t res = 0;
  for (var_idx_t idx : idx_list) {
    auto v = operator[](idx);
    if (v && !v->is_unused()) {
      ++res;
    }
  }
  return res;
}

VarDescrList& VarDescrList::operator-=(var_idx_t idx) {
  auto it = std::lower_bound(list.begin(), list.end(), idx);
  if (it != list.end() && it->idx == idx) {
    list.erase(it);
  }
  return *this;
}

VarDescrList& VarDescrList::operator-=(const std::vector<var_idx_t>& idx_list) {
  for (var_idx_t idx : idx_list) {
    *this -= idx;
  }
  return *this;
}

VarDescrList& VarDescrList::add_var(var_idx_t idx, bool unused) {
  auto it = std::lower_bound(list.begin(), list.end(), idx);
  if (it == list.end() || it->idx != idx) {
    list.emplace(it, idx, VarDescr::_Last | (unused ? VarDescr::_Unused : 0));
  } else if (it->is_unused() && !unused) {
    it->clear_unused();
  }
  return *this;
}

VarDescrList& VarDescrList::add_vars(const std::vector<var_idx_t>& idx_list, bool unused) {
  for (var_idx_t idx : idx_list) {
    add_var(idx, unused);
  }
  return *this;
}

VarDescr& VarDescrList::add(var_idx_t idx) {
  auto it = std::lower_bound(list.begin(), list.end(), idx);
  if (it == list.end() || it->idx != idx) {
    it = list.emplace(it, idx);
  }
  return *it;
}

VarDescr& VarDescrList::add_newval(var_idx_t idx) {
  auto it = std::lower_bound(list.begin(), list.end(), idx);
  if (it == list.end() || it->idx != idx) {
    return *list.emplace(it, idx);
  } else {
    it->clear_value();
    return *it;
  }
}

VarDescrList& VarDescrList::clear_last() {
  for (auto& var : list) {
    if (var.flags & VarDescr::_Last) {
      var.flags &= ~VarDescr::_Last;
    }
  }
  return *this;
}

VarDescrList VarDescrList::operator+(const VarDescrList& y) const {
  VarDescrList res;
  auto it1 = list.cbegin();
  auto it2 = y.list.cbegin();
  while (it1 != list.cend() && it2 != y.list.cend()) {
    if (it1->idx < it2->idx) {
      res.list.push_back(*it1++);
    } else if (it1->idx > it2->idx) {
      res.list.push_back(*it2++);
    } else {
      res.list.push_back(*it1++);
      res.list.back() += *it2++;
    }
  }
  while (it1 != list.cend()) {
    res.list.push_back(*it1++);
  }
  while (it2 != y.list.cend()) {
    res.list.push_back(*it2++);
  }
  return res;
}

VarDescrList& VarDescrList::operator+=(const VarDescrList& y) {
  return *this = *this + y;
}

VarDescrList VarDescrList::operator|(const VarDescrList& y) const {
  if (y.unreachable) {
    return *this;
  }
  if (unreachable) {
    return y;
  }
  VarDescrList res;
  auto it1 = list.cbegin();
  auto it2 = y.list.cbegin();
  while (it1 != list.cend() && it2 != y.list.cend()) {
    if (it1->idx < it2->idx) {
      it1++;
    } else if (it1->idx > it2->idx) {
      it2++;
    } else {
      res.list.push_back(*it1++);
      res.list.back() |= *it2++;
    }
  }
  return res;
}

VarDescrList& VarDescrList::operator|=(const VarDescrList& y) {
  if (y.unreachable) {
    return *this;
  } else {
    return *this = *this | y;
  }
}

VarDescrList& VarDescrList::import_values(const VarDescrList& values) {
  if (values.unreachable) {
    set_unreachable();
  } else
    for (auto& vd : list) {
      auto new_vd = values[vd.idx];
      if (new_vd) {
        vd.set_value(*new_vd);
      } else {
        vd.clear_value();
      }
    }
  return *this;
}

// Standard backward propagation of used variables through `left = OP right`:
// starts from next_var_info (what's needed after this op), removes left (defined here), adds right (consumed here).
// Returns true if this op's var_info changed (meaning the backward pass needs to continue).
bool Op::std_compute_used_vars(const VarDescrList& next_var_info, bool disabled) {
  // left = OP right
  // var_info := (var_info - left) + right
  VarDescrList new_var_info{next_var_info};
  new_var_info -= left;
  new_var_info.clear_last();
  if (args.size() == right.size() && !disabled) {
    for (const VarDescr& arg : args) {
      new_var_info.add_var(arg.idx, arg.is_unused());
    }
  } else {
    new_var_info.add_vars(right, disabled);
  }
  return set_var_info(std::move(new_var_info));
}

// Backward dataflow analysis for a single op: computes which variables are needed before this op,
// given which are needed after it (next_var_info). When edit=true, may disable ops whose results are unused.
// Returns true if this op's var_info changed.
bool Op::compute_used_vars(bool edit, const VarDescrList& next_var_info) {
  switch (cl) {
    case _Nop:
      return set_var_info_except(next_var_info, left);
    case _IntConst:
    case _SliceConst:
    case _SnakeStringConst:
    case _GlobVar:
    case _Call:
    case _CallInd:
    case _Tuple:
    case _UnTuple: {
      // left = EXEC right;
      if (!next_var_info.count_used(left) && !impure()) {
        // all variables in `left` are not needed
        if (edit) {
          set_disabled();
        }
        return std_compute_used_vars(next_var_info, true);
      }
      if (cl == _Call && does_function_always_throw(f_sym)) {
        VarDescrList new_var_info;    // empty, not next_var_info
        if (args.size() == right.size()) {
          for (const VarDescr& arg : args) {
            new_var_info.add_var(arg.idx, arg.is_unused());
          }
        } else {
          new_var_info.add_vars(right, false);
        }
        return set_var_info(std::move(new_var_info));
      }
      return std_compute_used_vars(next_var_info);
    }
    case _SetGlob: {
      // GLOB = right
      if (right.empty() && edit) {
        set_disabled();
      }
      return std_compute_used_vars(next_var_info, right.empty());
    }
    case _Let: {
      // left = right
      std::size_t cnt = next_var_info.count_used(left);
      tolk_assert(left.size() == right.size());
      auto l_it = left.cbegin(), r_it = right.cbegin();
      VarDescrList new_var_info{next_var_info};
      new_var_info -= left;
      new_var_info.clear_last();
      std::vector<var_idx_t> new_left, new_right;
      for (; l_it < left.cend(); ++l_it, ++r_it) {
        if (std::find(l_it + 1, left.cend(), *l_it) == left.cend()) {
          auto p = next_var_info[*l_it];
          new_var_info.add_var(*r_it, edit && (!p || p->is_unused()));
          new_left.push_back(*l_it);
          new_right.push_back(*r_it);
        }
      }
      if (new_left.size() < left.size()) {
        left = std::move(new_left);
        right = std::move(new_right);
      }
      if (!cnt && edit) {
        // all variables in `left` are not needed
        set_disabled();
      }
      return set_var_info(std::move(new_var_info));
    }
    case _Return: {
      // return left
      if (var_info.count(left) == left.size()) {
        return false;
      }
      std::vector<var_idx_t> unique_vars = sort_unique_vars(left);
      var_info.list.clear();
      for (var_idx_t i : unique_vars) {
        var_info.list.emplace_back(i, VarDescr::_Last);
      }
      return true;
    }
    case _Import: {
      // import left
      std::vector<var_idx_t> unique_vars = sort_unique_vars(left);
      var_info.list.clear();
      for (var_idx_t i : unique_vars) {
        var_info.list.emplace_back(i, next_var_info[i] ? 0 : VarDescr::_Last);
      }
      return true;
    }
    case _If: {
      // if (left) then block0 else block1
      block0.compute_used_code_vars(next_var_info, edit);
      VarDescrList merge_info;
      if (!block1.empty()) {
        block1.compute_used_code_vars(next_var_info, edit);
        merge_info = block0.entry_var_info() + block1.entry_var_info();
      } else {
        merge_info = block0.entry_var_info() + next_var_info;
      }
      merge_info.clear_last();
      merge_info += left;
      return set_var_info(std::move(merge_info));
    }
    case _While: {
      // while (block0 || left) block1;
      // ... block0 left { block1 block0 left } next
      VarDescrList new_var_info{next_var_info};
      bool changes = false;
      do {
        VarDescrList after_cond{new_var_info};
        after_cond += left;
        block0.compute_used_code_vars(after_cond, changes);
        block1.compute_used_code_vars(block0.entry_var_info(), changes);
        std::size_t n = new_var_info.size();
        new_var_info += block1.entry_var_info();
        new_var_info.clear_last();
        if (changes) {
          break;
        }
        changes = (new_var_info.size() == n);
      } while (changes <= edit);
      new_var_info += left;
      block0.compute_used_code_vars(new_var_info, edit);
      return set_var_info(block0.entry_var_info());
    }
    case _Until: {
      // until (block0 || left);
      // .. { block0 left } block0 left next
      VarDescrList after_cond_first{next_var_info};
      after_cond_first += left;
      block0.compute_used_code_vars(after_cond_first, false);
      VarDescrList new_var_info{block0.entry_var_info()};
      bool changes = false;
      do {
        VarDescrList after_cond{new_var_info};
        after_cond += next_var_info;
        after_cond += left;
        block0.compute_used_code_vars(after_cond, changes);
        std::size_t n = new_var_info.size();
        new_var_info += block0.entry_var_info();
        new_var_info.clear_last();
        if (changes) {
          break;
        }
        changes = (new_var_info.size() == n);
      } while (changes <= edit);
      return set_var_info(std::move(new_var_info) + next_var_info);
    }
    case _Repeat: {
      // repeat (left) block0
      // left { block0 } next
      VarDescrList new_var_info{next_var_info};
      bool changes = false;
      do {
        block0.compute_used_code_vars(new_var_info, changes);
        std::size_t n = new_var_info.size();
        new_var_info += block0.entry_var_info();
        new_var_info.clear_last();
        if (changes) {
          break;
        }
        changes = (new_var_info.size() == n);
      } while (changes <= edit);
      tolk_assert(left.size() == 1);
      bool last = new_var_info.count_used(left) == 0;
      new_var_info += left;
      if (last) {
        new_var_info[left[0]]->flags |= VarDescr::_Last;
      }
      return set_var_info(std::move(new_var_info));
    }
    case _Again: {
      // for(;;) block0
      // { block0 }
      VarDescrList new_var_info;
      bool changes = false;
      do {
        block0.compute_used_code_vars(new_var_info, changes);
        std::size_t n = new_var_info.size();
        new_var_info += block0.entry_var_info();
        new_var_info.clear_last();
        if (changes) {
          break;
        }
        changes = (new_var_info.size() == n);
      } while (changes <= edit);
      return set_var_info(std::move(new_var_info));
    }
    case _TryCatch: {
      block0.compute_used_code_vars(next_var_info, edit);
      block1.compute_used_code_vars(next_var_info, edit);
      VarDescrList merge_info = block0.entry_var_info() + block1.entry_var_info() + next_var_info;
      merge_info -= left;
      merge_info.clear_last();
      return set_var_info(std::move(merge_info));
    }
    default:
      err("unknown operation in compute_used_vars()").fire(origin);
  }
}

// helper: inline block's ops (excluding trailing Nop) into `result`
static void inline_block_content(OpList& result, OpList& block) {
  for (size_t j = 0; j < block.size() - 1; j++) {
    result.push_back(std::move(block[j]));
  }
}

// helper: merge two blocks into one (first content + second content + trailing Nop)
static OpList merge_blocks(OpList& first, OpList& second) {
  OpList merged;
  inline_block_content(merged, first);
  for (auto& o : second) {
    merged.push_back(std::move(o));
  }
  return merged;
}

// returns true if the op list reaches its end (code is reachable at the bottom)
bool OpList::prune_unreachable() {
  OpList& ops = *this;
  OpList result;

  for (size_t i = 0; i < ops.size(); i++) {
    Op& op = *ops[i];

    bool reach;
    switch (op.cl) {
      case Op::_Nop:
        // skip non-terminal Nops; keep terminal Nop (last element)
        if (i == ops.size() - 1) {
          result.push_back(std::move(ops[i]));
        }
        continue;
      case Op::_IntConst:
      case Op::_SliceConst:
      case Op::_SnakeStringConst:
      case Op::_GlobVar:
      case Op::_SetGlob:
      case Op::_CallInd:
      case Op::_Tuple:
      case Op::_UnTuple:
      case Op::_Import:
      case Op::_Let:
        reach = true;
        break;
      case Op::_Return:
        reach = false;
        break;
      case Op::_Call:
        reach = !does_function_always_throw(op.f_sym);
        break;
      case Op::_If: {
        // if left then block0 else block1; ...
        VarDescr* c_var = op.var_info[op.left[0]];
        if (c_var && c_var->always_true()) {
          // condition always true: inline block0, discard block1
          bool block_reaches = op.block0.prune_unreachable();
          inline_block_content(result, op.block0);
          if (!block_reaches) {
            result.push_back(make_terminal_nop(op.origin));
            ops = std::move(result);
            return false;
          }
          continue;  // remaining ops processed in next iterations
        }
        if (c_var && c_var->always_false()) {
          // condition always false: inline block1, discard block0
          bool block_reaches = op.block1.prune_unreachable();
          inline_block_content(result, op.block1);
          if (!block_reaches) {
            result.push_back(make_terminal_nop(op.origin));
            ops = std::move(result);
            return false;
          }
          continue;
        }
        reach = static_cast<int>(op.block0.prune_unreachable()) | static_cast<int>(op.block1.prune_unreachable());
        break;
      }
      case Op::_While: {
        // while (block0 || left) block1;
        if (!op.block0.prune_unreachable()) {
          // block0 (condition computation) never returns — inline block0
          inline_block_content(result, op.block0);
          result.push_back(make_terminal_nop(op.origin));
          ops = std::move(result);
          return false;
        }
        const VarDescr* c_var = op.block0.exit_var_info()[op.left[0]];
        if (c_var && c_var->always_false()) {
          // condition always false — loop body never executed, inline block0 (condition part)
          inline_block_content(result, op.block0);
          continue;  // remaining ops processed in next iterations
        }
        if (c_var && c_var->always_true()) {
          if (!op.block1.prune_unreachable()) {
            // block1 never returns — combine block0+block1, unreachable
            for (auto& o : merge_blocks(op.block0, op.block1)) {
              result.push_back(std::move(o));
            }
            ops = std::move(result);
            return false;
          }
          // infinite loop: transform while → again, merge block0+block1 into block0
          op.cl = Op::_Again;
          op.block0 = merge_blocks(op.block0, op.block1);
          op.block1.clear();
          op.left.clear();
          reach = false;
        } else {
          if (!op.block1.prune_unreachable()) {
            // block1 never returns
            // transform: while(block0; cond) block1; next → block0_content; if(cond) block1 else {}; next
            inline_block_content(result, op.block0);
            op.cl = Op::_If;
            op.block0 = std::move(op.block1);   // if-then = old while-body
            op.block1.clear();
            op.block1.push_back(make_terminal_nop(op.origin));  // else = empty
          }
          // keep the (possibly transformed) op
          reach = true;
        }
        break;
      }
      case Op::_Repeat: {
        // repeat (left) block0
        VarDescr* c_var = op.var_info[op.left[0]];
        if (c_var && c_var->always_nonpos()) {
          // loop never executed — skip the repeat op
          continue;
        }
        if (c_var && c_var->always_pos()) {
          if (!op.block0.prune_unreachable()) {
            // block0 executed at least once, and it never returns
            inline_block_content(result, op.block0);
            result.push_back(make_terminal_nop(op.origin));
            ops = std::move(result);
            return false;
          }
        } else {
          op.block0.prune_unreachable();
        }
        reach = true;
        break;
      }
      case Op::_Until:
      case Op::_Again: {
        // do block0 until left; ...
        if (!op.block0.prune_unreachable()) {
          // block0 never returns, replace loop by block0
          inline_block_content(result, op.block0);
          result.push_back(make_terminal_nop(op.origin));
          ops = std::move(result);
          return false;
        }
        reach = (op.cl != Op::_Again);
        break;
      }
      case Op::_TryCatch: {
        reach = static_cast<int>(op.block0.prune_unreachable()) | static_cast<int>(op.block1.prune_unreachable());
        break;
      }
      default:
        err("unknown operation in prune_unreachable()").fire(op.origin);
    }

    result.push_back(std::move(ops[i]));
    if (!reach) {
      // remaining ops are unreachable, append terminal Nop
      result.push_back(make_terminal_nop(op.origin));
      ops = std::move(result);
      return false;
    }
  }

  ops = std::move(result);
  return true;
}

void CodeBlob::prune_unreachable_code() {
  if (ops.prune_unreachable()) {
    err("control reaches end of function (stack is malformed, a compiler bug)").fire(fun_ref->ident_anchor, fun_ref);
  }
}

void CodeBlob::fwd_analyze() {
  VarDescrList values;
  tolk_assert(!ops.empty() && ops.front()->cl == Op::_Import);
  for (var_idx_t i : ops.front()->left) {
    values += i;
    if (vars[i].v_type == TypeDataInt::create()) {
      values[i]->val |= VarDescr::_Int;
    }
  }
  ops.fwd_analyze(values);
}

void Op::prepare_args(VarDescrList values) {
  if (args.size() != right.size()) {
    args.clear();
    for (var_idx_t i : right) {
      args.emplace_back(i);
    }
  }
  for (std::size_t i = 0; i < right.size(); i++) {
    const VarDescr* val = values[right[i]];
    if (val) {
      args[i].set_value(*val);
      // args[i].clear_unused();
    } else {
      args[i].clear_value();
    }
    args[i].clear_unused();
  }
}

void Op::maybe_swap_builtin_args_to_compile() {
  // in builtins.cpp, where optimizing constants are done, implementations assume that args are passed ltr (as declared);
  // if a function has arg_order, called arguments might have been put on a stack not ltr, but in asm order;
  // here we swap them back before calling FunctionBodyBuiltin compile, and also swap after
  tolk_assert(arg_order_already_equals_asm());
  if (f_sym->method_name == "storeUint" || f_sym->method_name == "storeInt" || f_sym->method_name == "storeBool") {
    std::swap(args[0], args[1]);
  }
}

// Forward dataflow analysis over an entire OpList: sequentially propagates known values
// (constants, types) through each op.
// Returns the resulting values at the end of the block.
VarDescrList OpList::fwd_analyze(VarDescrList values) const {
  for (auto& op_ptr : list) {
    values = op_ptr->fwd_analyze(std::move(values));
  }
  return values;
}

// Forward dataflow analysis for a single op: imports incoming values into var_info,
// then updates them with new values produced by this op (constants, call results, etc.).
// Returns the updated values to be passed to the next op.
VarDescrList Op::fwd_analyze(VarDescrList values) {
  var_info.import_values(values);
  switch (cl) {
    case _Nop:
    case _Import:
      break;
    case _Return:
      values.set_unreachable();
      break;
    case _IntConst: {
      values.add_newval(left[0]).set_const(int_const);
      break;
    }
    case _SliceConst: {
      values.add_newval(left[0]).set_const(str_const);
      break;
    }
    case _SnakeStringConst: {
      values.add_newval(left[0]).set_const(str_const);
      break;
    }
    case _Call: {
      prepare_args(values);
      if (!f_sym->is_code_function()) {
        std::vector<VarDescr> res;
        res.reserve(left.size());
        for (var_idx_t i : left) {
          res.emplace_back(i);
        }
        AsmOpList tmp;
        if (!f_sym->is_asm_function()) {
          if (arg_order_already_equals_asm()) {
            maybe_swap_builtin_args_to_compile();
          }
          std::get<FunctionBodyBuiltinAsmOp*>(f_sym->body)->compile(tmp, res, args, origin);
          if (arg_order_already_equals_asm()) {
            maybe_swap_builtin_args_to_compile();
          }
        }
        int j = 0;
        for (var_idx_t i : left) {
          values.add_newval(i).set_value(res[j++]);
        }
      } else {
        for (var_idx_t i : left) {
          values.add_newval(i);
        }
      }
      if (does_function_always_throw(f_sym)) {
        values.set_unreachable();
      }
      break;
    }
    case _Tuple:
    case _UnTuple:
    case _GlobVar:
    case _CallInd: {
      for (var_idx_t i : left) {
        values.add_newval(i);
      }
      break;
    }
    case _SetGlob:
      break;
    case _Let: {
      std::vector<VarDescr> old_val;
      tolk_assert(left.size() == right.size());
      for (std::size_t i = 0; i < right.size(); i++) {
        const VarDescr* ov = values[right[i]];
        if (!ov && G_settings.verbosity >= 5) {
          std::cerr << "FATAL: error in assignment at right component #" << i << " (no value for _" << right[i] << ")"
                    << std::endl;
          for (auto x : left) {
            std::cerr << '_' << x << " ";
          }
          std::cerr << "= ";
          for (auto x : right) {
            std::cerr << '_' << x << " ";
          }
          std::cerr << std::endl;
        }
        // tolk_assert(ov);
        if (ov) {
          old_val.push_back(*ov);
        } else {
          old_val.emplace_back();
        }
      }
      for (std::size_t i = 0; i < left.size(); i++) {
        values.add_newval(left[i]).set_value(std::move(old_val[i]));
      }
      break;
    }
    case _If: {
      VarDescrList val1 = block0.fwd_analyze( values);
      VarDescrList val2 = !block1.empty() ? block1.fwd_analyze( std::move(values)) : std::move(values);
      values = val1 | val2;
      break;
    }
    case _Repeat: {
      bool atl1 = (values[left[0]] && values[left[0]]->always_pos());
      VarDescrList next_values = block0.fwd_analyze( values);
      while (true) {
        VarDescrList new_values = values | next_values;
        if (same_values(new_values, values)) {
          break;
        }
        values = std::move(new_values);
        next_values = block0.fwd_analyze( values);
      }
      if (atl1) {
        values = std::move(next_values);
      }
      break;
    }
    case _While: {
      auto values0 = values;
      values = block0.fwd_analyze(values);
      if (values[left[0]] && values[left[0]]->always_false()) {
        // block1 never executed
        block1.fwd_analyze(values);
        break;
      }
      while (true) {
        VarDescrList next_values = values | block0.fwd_analyze( values0 | block1.fwd_analyze( values));
        if (same_values(next_values, values)) {
          break;
        }
        values = std::move(next_values);
      }
      break;
    }
    case _Until:
    case _Again: {
      while (true) {
        VarDescrList next_values = values | block0.fwd_analyze(values);
        if (same_values(next_values, values)) {
          break;
        }
        values = std::move(next_values);
      }
      values = block0.fwd_analyze(values);
      break;
    }
    case _TryCatch: {
      VarDescrList val1 = block0.fwd_analyze(values);
      VarDescrList val2 = block1.fwd_analyze(std::move(values));
      values = val1 | val2;
      break;
    }
    default:
      err("unknown operation in fwd_analyze()").fire(origin);
  }
  return values;
}

void Op::set_disabled(bool flag) {
  if (flag) {
    flags |= _Disabled;
  } else {
    flags &= ~_Disabled;
  }
}


bool Op::set_noreturn(bool flag) {
  if (flag) {
    flags |= _NoReturn;
  } else {
    flags &= ~_NoReturn;
  }
  return flag;
}

void Op::set_impure_flag() {
  flags |= _Impure;
}

void Op::set_arg_order_already_equals_asm_flag() {
  flags |= _ArgOrderAlreadyEqualsAsm;
}

// Two-phase mark_noreturn for OpList:
// Phase 1 (forward): recursively process blocks, perform _If restructuring
//   (move else-branch into continuation when if-branch always returns)
// Phase 2 (backward): compute noreturn flags knowing the continuation's noreturn status
bool OpList::mark_noreturn() {
  OpList& ops = *this;
  // Phase 1: forward pass — process sub-blocks and restructure _If
  for (size_t i = 0; i < ops.size(); i++) {
    Op& op = *ops[i];
    switch (op.cl) {
      case Op::_If: {
        op.block0.mark_noreturn();
        // replace `if (cond) { return; } else { block1; } next;` with `if (cond) { return; } block1; next`
        // purpose: avoid unnecessary RETALT instructions in generated code
        bool block1_nontrivial = !op.block1.is_empty_block();
        if (op.block0.is_noreturn() && block1_nontrivial) {
          VarDescrList block1_var_info = op.block1.entry_var_info();  // important to keep it
          // take block1's content (minus trailing Nop), insert into parent after this _If op
          size_t block1_content_count = op.block1.size() - 1;  // minus trailing Nop
          size_t insert_pos = i + 1;
          for (size_t j = 0; j < block1_content_count; j++) {
            ops.insert(ops.begin() + static_cast<long>(insert_pos + j), std::move(op.block1[j]));
          }
          // reset block1 to just a Nop
          op.block1.clear();
          op.block1.push_back(make_terminal_nop(op.origin));
          op.block1.set_entry_var_info(std::move(block1_var_info));
          // newly inserted ops will be processed in subsequent iterations of this loop
        } else {
          op.block1.mark_noreturn();
        }
        break;
      }
      case Op::_TryCatch:
        op.block0.mark_noreturn();
        if (!op.block1.empty()) {
          op.block1.mark_noreturn();
        }
        break;
      case Op::_Again:
      case Op::_Until:
      case Op::_Repeat:
        op.block0.mark_noreturn();
        break;
      case Op::_While:
        op.block0.mark_noreturn();
        op.block1.mark_noreturn();
        break;
      default:
        break;
    }
  }

  // Phase 2: backward pass — compute noreturn flags
  for (int i = static_cast<int>(ops.size()) - 1; i >= 0; i--) {
    Op& op = *ops[i];
    bool next_noreturn = (i < static_cast<int>(ops.size()) - 1) ? ops[i + 1]->noreturn() : false;

    switch (op.cl) {
      case Op::_Nop:
        op.set_noreturn(false);
        break;
      case Op::_Import:
      case Op::_IntConst:
      case Op::_SliceConst:
      case Op::_SnakeStringConst:
      case Op::_Let:
      case Op::_Tuple:
      case Op::_UnTuple:
      case Op::_SetGlob:
      case Op::_GlobVar:
      case Op::_CallInd:
        op.set_noreturn(next_noreturn);
        break;
      case Op::_Return:
        op.set_noreturn();
        break;
      case Op::_Call:
        op.set_noreturn(next_noreturn || does_function_always_throw(op.f_sym));
        break;
      case Op::_If:
        op.set_noreturn((op.block0.is_noreturn() && op.block1.is_noreturn()) || next_noreturn);
        break;
      case Op::_TryCatch:
        op.set_noreturn((op.block0.is_noreturn() && op.block1.is_noreturn()) || next_noreturn);
        break;
      case Op::_Again:
        op.set_noreturn();  // infinite loop = always noreturn
        break;
      case Op::_Until:
        op.set_noreturn(op.block0.is_noreturn() || next_noreturn);
        break;
      case Op::_While:
        op.set_noreturn(op.block0.is_noreturn() || next_noreturn);
        break;
      case Op::_Repeat:
        op.set_noreturn(next_noreturn);
        break;
      default:
        err("unknown operation in mark_noreturn()").fire(op.origin);
    }
  }

  return ops.empty() ? false : ops.front()->noreturn();
}

void CodeBlob::mark_noreturn() {
  ops.mark_noreturn();
}

}  // namespace tolk
