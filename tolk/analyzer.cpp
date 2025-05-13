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
#include "compiler-state.h"
#include "type-system.h"

namespace tolk {

// functions returning "never" are assumed to interrupt flow
// for instance, variables after their call aren't considered used
// its main purpose is `throw` statement, it's a call to a built-in `__throw` function
static bool does_function_always_throw(FunctionPtr fun_ref) {
  return fun_ref->declared_return_type == TypeDataNever::create();
}

/*
 *  
 *   ANALYZE AND PREPROCESS ABSTRACT CODE
 * 
 */

bool CodeBlob::compute_used_code_vars() {
  VarDescrList empty_var_info;
  return compute_used_code_vars(ops, empty_var_info, true);
}

bool CodeBlob::compute_used_code_vars(std::unique_ptr<Op>& ops_ptr, const VarDescrList& var_info, bool edit) const {
  tolk_assert(ops_ptr);
  if (!ops_ptr->next) {
    tolk_assert(ops_ptr->cl == Op::_Nop);
    return ops_ptr->set_var_info(var_info);
  }
  // here and below, bitwise | (not logical ||) are used to execute both left and right parts
  return static_cast<int>(compute_used_code_vars(ops_ptr->next, var_info, edit)) |
         static_cast<int>(ops_ptr->compute_used_vars(*this, edit));
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

bool Op::std_compute_used_vars(bool disabled) {
  // left = OP right
  // var_info := (var_info - left) + right
  VarDescrList new_var_info{next->var_info};
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

bool Op::compute_used_vars(const CodeBlob& code, bool edit) {
  tolk_assert(next);
  const VarDescrList& next_var_info = next->var_info;
  if (cl == _Nop) {
    return set_var_info_except(next_var_info, left);
  }
  switch (cl) {
    case _IntConst:
    case _SliceConst:
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
        return std_compute_used_vars(true);
      }
      if (cl == _Call && does_function_always_throw(f_sym)) {
        VarDescrList new_var_info;    // empty, not next->var_info
        if (args.size() == right.size()) {
          for (const VarDescr& arg : args) {
            new_var_info.add_var(arg.idx, arg.is_unused());
          }
        } else {
          new_var_info.add_vars(right, false);
        }
        return set_var_info(std::move(new_var_info));
      }
      return std_compute_used_vars();
    }
    case _SetGlob: {
      // GLOB = right
      if (right.empty() && edit) {
        set_disabled();
      }
      return std_compute_used_vars(right.empty());
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
      //  VarDescrList nx_var_info = next_var_info;
      //  nx_var_info.clear_last();
      code.compute_used_code_vars(block0, next_var_info, edit);
      VarDescrList merge_info;
      if (block1) {
        code.compute_used_code_vars(block1, next_var_info, edit);
        merge_info = block0->var_info + block1->var_info;
      } else {
        merge_info = block0->var_info + next_var_info;
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
        code.compute_used_code_vars(block0, after_cond, changes);
        code.compute_used_code_vars(block1, block0->var_info, changes);
        std::size_t n = new_var_info.size();
        new_var_info += block1->var_info;
        new_var_info.clear_last();
        if (changes) {
          break;
        }
        changes = (new_var_info.size() == n);
      } while (changes <= edit);
      new_var_info += left;
      code.compute_used_code_vars(block0, new_var_info, edit);
      return set_var_info(block0->var_info);
    }
    case _Until: {
      // until (block0 || left);
      // .. { block0 left } block0 left next
      VarDescrList after_cond_first{next_var_info};
      after_cond_first += left;
      code.compute_used_code_vars(block0, after_cond_first, false);
      VarDescrList new_var_info{block0->var_info};
      bool changes = false;
      do {
        VarDescrList after_cond{new_var_info};
        after_cond += next_var_info;
        after_cond += left;
        code.compute_used_code_vars(block0, after_cond, changes);
        std::size_t n = new_var_info.size();
        new_var_info += block0->var_info;
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
        code.compute_used_code_vars(block0, new_var_info, changes);
        std::size_t n = new_var_info.size();
        new_var_info += block0->var_info;
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
        code.compute_used_code_vars(block0, new_var_info, changes);
        std::size_t n = new_var_info.size();
        new_var_info += block0->var_info;
        new_var_info.clear_last();
        if (changes) {
          break;
        }
        changes = (new_var_info.size() == n);
      } while (changes <= edit);
      return set_var_info(std::move(new_var_info));
    }
    case _TryCatch: {
      code.compute_used_code_vars(block0, next_var_info, edit);
      code.compute_used_code_vars(block1, next_var_info, edit);
      VarDescrList merge_info = block0->var_info + block1->var_info + next_var_info;
      merge_info -= left;
      merge_info.clear_last();
      return set_var_info(std::move(merge_info));
    }
    default:
      std::cerr << "fatal: unknown operation <??" << cl << "> in compute_used_vars()\n";
      throw ParseError(loc, "unknown operation");
  }
}

bool prune_unreachable(std::unique_ptr<Op>& ops) {
  if (!ops) {
    return true;
  }
  Op& op = *ops;
  if (op.cl == Op::_Nop) {
    if (op.next) {
      ops = std::move(op.next);
      return prune_unreachable(ops);
    }
    return true;
  }
  bool reach;
  switch (op.cl) {
    case Op::_IntConst:
    case Op::_SliceConst:
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
        op.block0->last().next = std::move(op.next);
        ops = std::move(op.block0);
        return prune_unreachable(ops);
      } else if (c_var && c_var->always_false()) {
        op.block1->last().next = std::move(op.next);
        ops = std::move(op.block1);
        return prune_unreachable(ops);
      } else {
        reach = static_cast<int>(prune_unreachable(op.block0)) | static_cast<int>(prune_unreachable(op.block1));
      }
      break;
    }
    case Op::_While: {
      // while (block0 || left) block1;
      if (!prune_unreachable(op.block0)) {
        // computation of block0 never returns
        ops = std::move(op.block0);
        return prune_unreachable(ops);
      }
      VarDescr* c_var = op.block0->last().var_info[op.left[0]];
      if (c_var && c_var->always_false()) {
        // block1 never executed
        op.block0->last().next = std::move(op.next);
        ops = std::move(op.block0);
        return prune_unreachable(ops);
      } else if (c_var && c_var->always_true()) {
        if (!prune_unreachable(op.block1)) {
          // block1 never returns
          op.block0->last().next = std::move(op.block1);
          ops = std::move(op.block0);
          return false;
        }
        // infinite loop
        op.cl = Op::_Again;
        op.block0->last().next = std::move(op.block1);
        op.left.clear();
        reach = false;
      } else {
        if (!prune_unreachable(op.block1)) {
          // block1 never returns, while equivalent to block0 ; if left then block1 else next
          op.cl = Op::_If;
          std::unique_ptr<Op> new_op = std::move(op.block0);
          op.block0 = std::move(op.block1);
          op.block1 = std::make_unique<Op>(op.next->loc, Op::_Nop);
          new_op->last().next = std::move(ops);
          ops = std::move(new_op);
        }
        reach = true;  // block1 may be never executed
      }
      break;
    }
    case Op::_Repeat: {
      // repeat (left) block0
      VarDescr* c_var = op.var_info[op.left[0]];
      if (c_var && c_var->always_nonpos()) {
        // loop never executed
        ops = std::move(op.next);
        return prune_unreachable(ops);
      }
      if (c_var && c_var->always_pos()) {
        if (!prune_unreachable(op.block0)) {
          // block0 executed at least once, and it never returns
          // replace code with block0
          ops = std::move(op.block0);
          return false;
        }
      } else {
        prune_unreachable(op.block0);
      }
      reach = true;
      break;
    }
    case Op::_Until:
    case Op::_Again: {
      // do block0 until left; ...
      if (!prune_unreachable(op.block0)) {
        // block0 never returns, replace loop by block0
        ops = std::move(op.block0);
        return false;
      }
      reach = (op.cl != Op::_Again);
      break;
    }
    case Op::_TryCatch: {
      reach = static_cast<int>(prune_unreachable(op.block0)) | static_cast<int>(prune_unreachable(op.block1));
      break;
    }
    default:
      std::cerr << "fatal: unknown operation <??" << op.cl << ">\n";
      throw ParseError(op.loc, "unknown operation in prune_unreachable()");
  }
  if (reach) {
    return prune_unreachable(op.next);
  } else {
    while (op.next->next) {
      op.next = std::move(op.next->next);
    }
    return false;
  }
}

void CodeBlob::prune_unreachable_code() {
  if (prune_unreachable(ops)) {
    throw ParseError(fun_ref->loc, "control reaches end of function");
  }
}

void CodeBlob::fwd_analyze() {
  VarDescrList values;
  tolk_assert(ops && ops->cl == Op::_Import);
  for (var_idx_t i : ops->left) {
    values += i;
    if (vars[i].v_type == TypeDataInt::create()) {
      values[i]->val |= VarDescr::_Int;
    }
  }
  ops->fwd_analyze(values);
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
          std::get<FunctionBodyBuiltin*>(f_sym->body)->compile(tmp, res, args, loc);
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
        if (!ov && G.is_verbosity(5)) {
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
      VarDescrList val1 = block0->fwd_analyze(values);
      VarDescrList val2 = block1 ? block1->fwd_analyze(std::move(values)) : std::move(values);
      values = val1 | val2;
      break;
    }
    case _Repeat: {
      bool atl1 = (values[left[0]] && values[left[0]]->always_pos());
      VarDescrList next_values = block0->fwd_analyze(values);
      while (true) {
        VarDescrList new_values = values | next_values;
        if (same_values(new_values, values)) {
          break;
        }
        values = std::move(new_values);
        next_values = block0->fwd_analyze(values);
      }
      if (atl1) {
        values = std::move(next_values);
      }
      break;
    }
    case _While: {
      auto values0 = values;
      values = block0->fwd_analyze(values);
      if (values[left[0]] && values[left[0]]->always_false()) {
        // block1 never executed
        block1->fwd_analyze(values);
        break;
      }
      while (true) {
        VarDescrList next_values = values | block0->fwd_analyze(values0 | block1->fwd_analyze(values));
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
        VarDescrList next_values = values | block0->fwd_analyze(values);
        if (same_values(next_values, values)) {
          break;
        }
        values = std::move(next_values);
      }
      values = block0->fwd_analyze(values);
      break;
    }
    case _TryCatch: {
      VarDescrList val1 = block0->fwd_analyze(values);
      VarDescrList val2 = block1->fwd_analyze(std::move(values));
      values = val1 | val2;
      break;
    }
    default:
      std::cerr << "fatal: unknown operation <??" << cl << ">\n";
      throw ParseError(loc, "unknown operation in fwd_analyze()");
  }
  if (next) {
    return next->fwd_analyze(std::move(values));
  } else {
    return values;
  }
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

bool Op::mark_noreturn() {
  switch (cl) {
    case _Nop:
      if (!next) {
        return set_noreturn(false);
      }
      // fallthrough
    case _Import:
    case _IntConst:
    case _SliceConst:
    case _Let:
    case _Tuple:
    case _UnTuple:
    case _SetGlob:
    case _GlobVar:
    case _CallInd:
      return set_noreturn(next->mark_noreturn());
    case _Return:
      return set_noreturn();
    case _Call:
      return set_noreturn(next->mark_noreturn() || does_function_always_throw(f_sym));
    case _If: {
      // this very-not-beautiful code does the following:
      // replace `if (cond) { return; } else { block1; } next;` with `if (cond) { return; } block1; next`
      // purpose: to make code like `if (...) { ... return; } else if (...) { ... return; } ...` act like without else
      // similarly, `match (...) { v1 => { ... return; } ...}` is internally transformed to IF-ELSE
      // (that's why such transformation is done at IR level, not in AST)
      // without these code (without else removed), extra RETALT instructions are inserted, not necessary actually
      // implementation is UGLY, because currently there is no way to perform IR replacements
      // in the future, anyway, IR implementation should be rewritten, for easier traversing and replacement
      // btw, now it doesn't work with `if (!...)` (v->is_ifnot), else "keyword" is not removed
      if (block0->mark_noreturn() && !block1->is_empty()) {
        VarDescrList block1_var_info = block1->var_info;  // important to keep it
        Op* last_in_block1 = block1.get();
        while (last_in_block1->next->cl != Op::_Nop) {    // find the tail of a forward list of Ops
          last_in_block1 = last_in_block1->next.get();
        }
        last_in_block1->next = std::move(next);
        next = std::move(block1);
        block1 = std::make_unique<Op>(loc, Op::_Nop);
        block1->var_info = std::move(block1_var_info);
      } else {
        block1->mark_noreturn();
      }
      bool next_noreturn = next->mark_noreturn();
      return set_noreturn((block0->noreturn() && block1->noreturn()) || next_noreturn);
    }
    case _TryCatch:
      // note, that & | (not && ||) here and below is mandatory to invoke both left and right calls
      return set_noreturn((static_cast<int>(block0->mark_noreturn()) & static_cast<int>(block1 && block1->mark_noreturn())) | static_cast<int>(next->mark_noreturn()));
    case _Again:
      block0->mark_noreturn();
      return set_noreturn();
    case _Until:
      return set_noreturn(static_cast<int>(block0->mark_noreturn()) | static_cast<int>(next->mark_noreturn()));
    case _While:
      block1->mark_noreturn();
      return set_noreturn(static_cast<int>(block0->mark_noreturn()) | static_cast<int>(next->mark_noreturn()));
    case _Repeat:
      block0->mark_noreturn();
      return set_noreturn(next->mark_noreturn());
    default:
      std::cerr << "fatal: unknown operation <??" << cl << ">\n";
      throw ParseError(loc, "unknown operation in mark_noreturn()");
  }
}

void CodeBlob::mark_noreturn() {
  ops->mark_noreturn();
}

}  // namespace tolk
