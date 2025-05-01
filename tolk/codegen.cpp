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

/*
 * 
 *   GENERATE TVM STACK CODE
 * 
 */

StackLayout Stack::vars() const {
  StackLayout res;
  res.reserve(s.size());
  for (auto x : s) {
    res.push_back(x.first);
  }
  return res;
}

int Stack::find(var_idx_t var, int from) const {
  for (int i = from; i < depth(); i++) {
    if (at(i).first == var) {
      return i;
    }
  }
  return -1;
}

// finds var in [from .. to)
int Stack::find(var_idx_t var, int from, int to) const {
  for (int i = from; i < depth() && i < to; i++) {
    if (at(i).first == var) {
      return i;
    }
  }
  return -1;
}

// finds var outside [from .. to)
int Stack::find_outside(var_idx_t var, int from, int to) const {
  from = std::max(from, 0);
  if (from >= to) {
    return find(var);
  } else {
    int t = find(var, 0, from);
    return t >= 0 ? t : find(var, to);
  }
}

int Stack::find_const(const_idx_t cst, int from) const {
  for (int i = from; i < depth(); i++) {
    if (at(i).second == cst) {
      return i;
    }
  }
  return -1;
}

void Stack::forget_const() {
  for (auto& vc : s) {
    if (vc.second != not_const) {
      vc.second = not_const;
    }
  }
}

void Stack::issue_pop(SrcLocation loc, int i) {
  validate(i);
  if (output_enabled()) {
    o << AsmOp::Pop(loc, i);
  }
  at(i) = get(0);
  s.pop_back();
  modified();
  opt_show();
}

void Stack::issue_push(SrcLocation loc, int i) {
  validate(i);
  if (output_enabled()) {
    o << AsmOp::Push(loc, i);
  }
  s.push_back(get(i));
  modified();
  opt_show();
}

void Stack::issue_xchg(SrcLocation loc, int i, int j) {
  validate(i);
  validate(j);
  if (i != j && get(i) != get(j)) {
    if (output_enabled()) {
      o << AsmOp::Xchg(loc, i, j);
    }
    std::swap(at(i), at(j));
    modified();
    opt_show();
  }
}

int Stack::drop_vars_except(SrcLocation loc, const VarDescrList& var_info, int excl_var) {
  int dropped = 0, changes;
  do {
    changes = 0;
    int n = depth();
    for (int i = 0; i < n; i++) {
      var_idx_t idx = at(i).first;
      if (((!var_info[idx] || var_info[idx]->is_unused()) && idx != excl_var) || find(idx, 0, i - 1) >= 0) {
        // unneeded
        issue_pop(loc, i);
        changes = 1;
        break;
      }
    }
    dropped += changes;
  } while (changes);
  return dropped;
}

void Stack::show() {
  std::ostringstream os;
  for (auto i : s) {
    os << ' ';
    o.show_var_ext(os, i);
  }
  o << AsmOp::Comment({}, os.str());
  mode |= _Shown;
}

void Stack::forget_var(var_idx_t idx) {
  for (auto& x : s) {
    if (x.first == idx) {
      x = std::make_pair(_Garbage, not_const);
      modified();
    }
  }
}

void Stack::push_new_var(var_idx_t idx) {
  forget_var(idx);
  s.emplace_back(idx, not_const);
  modified();
}

void Stack::push_new_const(var_idx_t idx, const_idx_t cidx) {
  forget_var(idx);
  s.emplace_back(idx, cidx);
  modified();
}

void Stack::assign_var(var_idx_t new_idx, var_idx_t old_idx) {
  int i = find(old_idx);
  tolk_assert(i >= 0 && "variable not found in stack");
  if (new_idx != old_idx) {
    at(i).first = new_idx;
    modified();
  }
}

void Stack::do_copy_var(SrcLocation loc, var_idx_t new_idx, var_idx_t old_idx) {
  int i = find(old_idx);
  tolk_assert(i >= 0 && "variable not found in stack");
  if (find(old_idx, i + 1) < 0) {
    issue_push(loc, i);
    tolk_assert(at(0).first == old_idx);
  }
  assign_var(new_idx, old_idx);
}

void Stack::enforce_state(SrcLocation loc, const StackLayout& req_stack) {
  int k = (int)req_stack.size();
  for (int i = 0; i < k; i++) {
    var_idx_t x = req_stack[i];
    if (i < depth() && s[i].first == x) {
      continue;
    }
    while (depth() > 0 && std::find(req_stack.cbegin(), req_stack.cend(), get(0).first) == req_stack.cend()) {
      // current TOS entry is unused in req_stack, drop it
      issue_pop(loc, 0);
    }
    int j = find(x);
    if (j >= depth() - i) {
      issue_push(loc, j);
      j = 0;
    }
    issue_xchg(loc, j, depth() - i - 1);
    tolk_assert(s[i].first == x);
  }
  while (depth() > k) {
    issue_pop(loc, 0);
  }
  tolk_assert(depth() == k);
  for (int i = 0; i < k; i++) {
    tolk_assert(s[i].first == req_stack[i]);
  }
}

void Stack::merge_const(const Stack& req_stack) {
  tolk_assert(s.size() == req_stack.s.size());
  for (std::size_t i = 0; i < s.size(); i++) {
    tolk_assert(s[i].first == req_stack.s[i].first);
    if (s[i].second != req_stack.s[i].second) {
      s[i].second = not_const;
    }
  }
}

void Stack::merge_state(SrcLocation loc, const Stack& req_stack) {
  enforce_state(loc, req_stack.vars());
  merge_const(req_stack);
}

void Stack::rearrange_top(SrcLocation loc, const StackLayout& top, std::vector<bool> last) {
  while (last.size() < top.size()) {
    last.push_back(false);
  }
  int k = (int)top.size();
  for (int i = 0; i < k; i++) {
    for (int j = i + 1; j < k; j++) {
      if (top[i] == top[j]) {
        last[i] = false;
        break;
      }
    }
  }
  int ss = 0;
  for (int i = 0; i < k; i++) {
    if (last[i]) {
      ++ss;
    }
  }
  for (int i = 0; i < k; i++) {
    var_idx_t x = top[i];
    // find s(j) containing x with j not in [ss, ss+i)
    int j = find_outside(x, ss, ss + i);
    if (last[i]) {
      // rearrange x to be at s(ss-1)
      issue_xchg(loc, --ss, j);
      tolk_assert(get(ss).first == x);
    } else {
      // create a new copy of x
      issue_push(loc, j);
      issue_xchg(loc, 0, ss);
      tolk_assert(get(ss).first == x);
    }
  }
  tolk_assert(!ss);
}

void Stack::rearrange_top(SrcLocation loc, var_idx_t top, bool last) {
  int i = find(top);
  if (last) {
    issue_xchg(loc, 0, i);
  } else {
    issue_push(loc, i);
  }
  tolk_assert(get(0).first == top);
}

bool Op::generate_code_step(Stack& stack) {
  stack.opt_show();

  // detect `throw 123` (actually _IntConst 123 + _Call __throw)
  // don't clear the stack, since dropping unused elements make no sense, an exception is thrown anyway
  bool will_now_immediate_throw = (cl == _Call && f_sym->is_builtin_function() && f_sym->name == "__throw")
      || (cl == _IntConst && next->cl == _Call && next->f_sym->is_builtin_function() && next->f_sym->name == "__throw");
  if (!will_now_immediate_throw) {
    stack.drop_vars_except(loc, var_info);
    stack.opt_show();
  }

  bool inline_func = stack.mode & Stack::_InlineFunc;
  switch (cl) {
    case _Nop:
    case _Import:
      return true;
    case _Return: {
      stack.enforce_state(loc, left);
      if (stack.o.retalt_ && (stack.mode & Stack::_NeedRetAlt)) {
        stack.o << AsmOp::Custom(loc, "RETALT");
        stack.o.retalt_inserted_ = true;
      }
      stack.opt_show();
      return false;
    }
    case _IntConst: {
      auto p = next->var_info[left[0]];
      if (!p || p->is_unused()) {
        return true;
      }
      auto cidx = stack.o.register_const(int_const);
      int i = stack.find_const(cidx);
      if (i < 0) {
        stack.o << push_const(loc, int_const);
        stack.push_new_const(left[0], cidx);
      } else {
        tolk_assert(stack.at(i).second == cidx);
        stack.do_copy_var(loc, left[0], stack[i]);
      }
      return true;
    }
    case _SliceConst: {
      auto p = next->var_info[left[0]];
      if (!p || p->is_unused()) {
        return true;
      }
      stack.o << AsmOp::Const(loc, "x{" + str_const + "} PUSHSLICE");
      stack.push_new_var(left[0]);
      return true;
    }
    case _GlobVar:
      if (g_sym) {
        bool used = false;
        for (auto i : left) {
          auto p = next->var_info[i];
          if (p && !p->is_unused()) {
            used = true;
          }
        }
        if (!used || disabled()) {
          return true;
        }
        stack.o << AsmOp::Custom(loc, g_sym->name + " GETGLOB", 0, 1);
        if (left.size() != 1) {
          tolk_assert(left.size() <= 15);
          stack.o << AsmOp::UnTuple(loc, (int)left.size());
        }
        for (auto i : left) {
          stack.push_new_var(i);
        }
        return true;
      } else {
        tolk_assert(left.size() == 1);
        auto p = next->var_info[left[0]];
        if (!p || p->is_unused() || disabled()) {
          return true;
        }
        stack.o << AsmOp::Custom(loc, "CONT:<{");
        stack.o.indent();
        if (f_sym->is_asm_function() || f_sym->is_builtin_function()) {
          // TODO: create and compile a true lambda instead of this (so that arg_order and ret_order would work correctly)
          std::vector<VarDescr> args0, res;
          int w_arg = 0;
          for (const LocalVarData& param : f_sym->parameters) {
            w_arg += param.declared_type->get_width_on_stack();
          }
          int w_ret = f_sym->inferred_return_type->get_width_on_stack();
          tolk_assert(w_ret >= 0 && w_arg >= 0);
          for (int i = 0; i < w_ret; i++) {
            res.emplace_back(0);
          }
          for (int i = 0; i < w_arg; i++) {
            args0.emplace_back(0);
          }
          if (f_sym->is_asm_function()) {
            std::get<FunctionBodyAsm*>(f_sym->body)->compile(stack.o, loc);  // compile res := f (args0)
          } else {
            std::get<FunctionBodyBuiltin*>(f_sym->body)->compile(stack.o, res, args0, loc);  // compile res := f (args0)
          }
        } else {
          stack.o << AsmOp::Custom(loc, f_sym->name + " CALLDICT", (int)right.size(), (int)left.size());
        }
        stack.o.undent();
        stack.o << AsmOp::Custom({}, "}>");
        stack.push_new_var(left.at(0));
        return true;
      }
    case _Let: {
      tolk_assert(left.size() == right.size());
      int i = 0;
      std::vector<bool> active;
      active.reserve(left.size());
      for (std::size_t k = 0; k < left.size(); k++) {
        var_idx_t y = left[k];  // "y" = "x"
        auto p = next->var_info[y];
        active.push_back(p && !p->is_unused());
      }
      for (std::size_t k = 0; k < left.size(); k++) {
        if (!active[k]) {
          continue;
        }
        var_idx_t x = right[k];  // "y" = "x"
        bool is_last = true;
        for (std::size_t l = k + 1; l < right.size(); l++) {
          if (right[l] == x && active[l]) {
            is_last = false;
          }
        }
        if (is_last) {
          auto info = var_info[x];
          is_last = (info && info->is_last());
        }
        if (is_last) {
          stack.assign_var(--i, x);
        } else {
          stack.do_copy_var(loc, --i, x);
        }
      }
      i = 0;
      for (std::size_t k = 0; k < left.size(); k++) {
        if (active[k]) {
          stack.assign_var(left[k], --i);
        }
      }
      return true;
    }
    case _Tuple:
    case _UnTuple: {
      if (disabled()) {
        return true;
      }
      std::vector<bool> last;
      for (var_idx_t x : right) {
        last.push_back(var_info[x] && var_info[x]->is_last());
      }
      stack.rearrange_top(loc, right, std::move(last));
      stack.opt_show();
      int k = (int)stack.depth() - (int)right.size();
      tolk_assert(k >= 0);
      if (cl == _Tuple) {
        stack.o << AsmOp::Tuple(loc, (int)right.size());
        tolk_assert(left.size() == 1);
      } else {
        stack.o << AsmOp::UnTuple(loc, (int)left.size());
        tolk_assert(right.size() == 1);
      }
      stack.s.resize(k);
      for (int i = 0; i < (int)left.size(); i++) {
        stack.push_new_var(left.at(i));
      }
      return true;
    }
    case _Call:
    case _CallInd: {
      if (disabled()) {
        return true;
      }
      // f_sym can be nullptr for Op::_CallInd (invoke a variable, not a function);
      // if f has arg_order, when it's safe, the compiler evaluates arguments in that order in advance (for fewer stack manipulations);
      // when it's unsafe, arguments are evaluated left-to-right, and we need to match asm arg_order here
      const std::vector<int>* arg_order = f_sym && !arg_order_already_equals_asm() ? f_sym->get_arg_order() : nullptr;
      const std::vector<int>* ret_order = f_sym ? f_sym->get_ret_order() : nullptr;
      tolk_assert(!arg_order || arg_order->size() == right.size());
      tolk_assert(!ret_order || ret_order->size() == left.size());
      std::vector<var_idx_t> right1;
      if (args.size()) {
        tolk_assert(args.size() == right.size());
        for (int i = 0; i < (int)right.size(); i++) {
          int j = arg_order ? arg_order->at(i) : i;
          const VarDescr& arg = args.at(j);
          if (!arg.is_unused()) {
            tolk_assert(var_info[arg.idx] && !var_info[arg.idx]->is_unused());
            right1.push_back(arg.idx);
          }
        }
      } else {
        tolk_assert(!arg_order);
        right1 = right;
      }
      std::vector<bool> last;
      last.reserve(right1.size());
      for (var_idx_t x : right1) {
        last.push_back(var_info[x] && var_info[x]->is_last());
      }
      stack.rearrange_top(loc, right1, std::move(last));
      stack.opt_show();
      int k = (int)stack.depth() - (int)right1.size();
      tolk_assert(k >= 0);
      for (int i = 0; i < (int)right1.size(); i++) {
        tolk_assert(stack.s[k + i].first == right1[i]);
      }
      auto exec_callxargs = [&](int args, int ret) {
        if (args <= 15 && ret <= 15) {
          stack.o << exec_arg2_op(loc, "CALLXARGS", args, ret, args + 1, ret);
        } else {
          tolk_assert(args <= 254 && ret <= 254);
          stack.o << AsmOp::Const(loc, PSTRING() << args << " PUSHINT");
          stack.o << AsmOp::Const(loc, PSTRING() << ret << " PUSHINT");
          stack.o << AsmOp::Custom(loc, "CALLXVARARGS", args + 3, ret);
        }
      };
      if (cl == _CallInd) {
        exec_callxargs((int)right.size() - 1, (int)left.size());
      } else if (!f_sym->is_code_function()) {
        std::vector<VarDescr> res;
        res.reserve(left.size());
        for (var_idx_t i : left) {
          res.emplace_back(i);
        }
        if (f_sym->is_asm_function()) {
          std::get<FunctionBodyAsm*>(f_sym->body)->compile(stack.o, loc);  // compile res := f (args)
        } else {
          std::get<FunctionBodyBuiltin*>(f_sym->body)->compile(stack.o, res, args, loc);  // compile res := f (args)
        }
      } else {
        if (f_sym->is_inline() || f_sym->is_inline_ref()) {
          stack.o << AsmOp::Custom(loc, f_sym->name + " INLINECALLDICT", (int)right.size(), (int)left.size());
        } else if (f_sym->is_code_function() && std::get<FunctionBodyCode*>(f_sym->body)->code->require_callxargs) {
          stack.o << AsmOp::Custom(loc, f_sym->name + (" PREPAREDICT"), 0, 2);
          exec_callxargs((int)right.size() + 1, (int)left.size());
        } else {
          stack.o << AsmOp::Custom(loc, f_sym->name + " CALLDICT", (int)right.size(), (int)left.size());
        }
      }
      stack.modified();
      stack.s.resize(k);
      for (int i = 0; i < (int)left.size(); i++) {
        int j = ret_order ? ret_order->at(i) : i;
        stack.push_new_var(left.at(j));
      }
      return !f_sym || f_sym->declared_return_type != TypeDataNever::create();
    }
    case _SetGlob: {
      tolk_assert(g_sym);
      std::vector<bool> last;
      for (var_idx_t x : right) {
        last.push_back(var_info[x] && var_info[x]->is_last());
      }
      stack.rearrange_top(loc, right, std::move(last));
      stack.opt_show();
      int k = (int)stack.depth() - (int)right.size();
      tolk_assert(k >= 0);
      for (int i = 0; i < (int)right.size(); i++) {
        tolk_assert(stack.s[k + i].first == right[i]);
      }
      if (right.size() > 1) {
        stack.o << AsmOp::Tuple(loc, (int)right.size());
      }
      if (!right.empty()) {
        stack.o << AsmOp::Custom(loc, g_sym->name + " SETGLOB", 1, 0);
        stack.modified();
      }
      stack.s.resize(k);
      return true;
    }
    case _If: {
      if (block0->is_empty() && block1->is_empty()) {
        return true;
      }
      if (!next->noreturn() && (block0->noreturn() != block1->noreturn())) {
        stack.o.retalt_ = true;
      }
      var_idx_t x = left[0];
      stack.rearrange_top(loc, x, var_info[x] && var_info[x]->is_last());
      tolk_assert(stack[0] == x);
      stack.opt_show();
      stack.s.pop_back();
      stack.modified();
      if (inline_func && (block0->noreturn() || block1->noreturn())) {
        bool is0 = block0->noreturn();
        Op* block_noreturn = is0 ? block0.get() : block1.get();
        Op* block_other = is0 ? block1.get() : block0.get();
        stack.mode &= ~Stack::_InlineFunc;
        stack.o << AsmOp::Custom(loc, is0 ? "IF:<{" : "IFNOT:<{");
        stack.o.indent();
        Stack stack_copy{stack};
        block_noreturn->generate_code_all(stack_copy);
        stack.o.undent();
        stack.o << AsmOp::Custom({}, "}>ELSE<{");
        stack.o.indent();
        block_other->generate_code_all(stack);
        if (!block_other->noreturn()) {
          next->generate_code_all(stack);
        }
        stack.o.undent();
        stack.o << AsmOp::Custom({}, "}>");
        return false;
      }
      if (block1->is_empty() || block0->is_empty()) {
        bool is0 = block1->is_empty();
        Op* block = is0 ? block0.get() : block1.get();
        // if (left) block0; ...
        // if (!left) block1; ...
        if (block->noreturn()) {
          stack.o << AsmOp::Custom(loc, is0 ? "IFJMP:<{" : "IFNOTJMP:<{");
          stack.o.indent();
          Stack stack_copy{stack};
          stack_copy.mode &= ~Stack::_InlineFunc;
          stack_copy.mode |= next->noreturn() ? 0 : Stack::_NeedRetAlt;
          block->generate_code_all(stack_copy);
          stack.o.undent();
          stack.o << AsmOp::Custom({}, "}>");
          return true;
        }
        stack.o << AsmOp::Custom(loc, is0 ? "IF:<{" : "IFNOT:<{");
        stack.o.indent();
        Stack stack_copy{stack}, stack_target{stack};
        stack_target.disable_output();
        stack_target.drop_vars_except(loc, next->var_info);
        stack_copy.mode &= ~Stack::_InlineFunc;
        block->generate_code_all(stack_copy);
        stack_copy.drop_vars_except(loc, var_info);
        stack_copy.opt_show();
        if ((is0 && stack_copy == stack) || (!is0 && stack_copy.vars() == stack.vars())) {
          stack.o.undent();
          stack.o << AsmOp::Custom({}, "}>");
          if (!is0) {
            stack.merge_const(stack_copy);
          }
          return true;
        }
        // stack_copy.drop_vars_except(next->var_info);
        stack_copy.enforce_state(loc, stack_target.vars());
        stack_copy.opt_show();
        if (stack_copy.vars() == stack.vars()) {
          stack.o.undent();
          stack.o << AsmOp::Custom({}, "}>");
          stack.merge_const(stack_copy);
          return true;
        }
        stack.o.undent();
        stack.o << AsmOp::Custom({}, "}>ELSE<{");
        stack.o.indent();
        stack.merge_state(loc, stack_copy);
        stack.opt_show();
        stack.o.undent();
        stack.o << AsmOp::Custom({}, "}>");
        return true;
      }
      if (block0->noreturn() || block1->noreturn()) {
        bool is0 = block0->noreturn();
        Op* block_noreturn = is0 ? block0.get() : block1.get();
        Op* block_other = is0 ? block1.get() : block0.get();
        stack.o << AsmOp::Custom(loc, is0 ? "IFJMP:<{" : "IFNOTJMP:<{");
        stack.o.indent();
        Stack stack_copy{stack};
        stack_copy.mode &= ~Stack::_InlineFunc;
        stack_copy.mode |= (block_other->noreturn() || next->noreturn()) ? 0 : Stack::_NeedRetAlt;
        block_noreturn->generate_code_all(stack_copy);
        stack.o.undent();
        stack.o << AsmOp::Custom({}, "}>");
        block_other->generate_code_all(stack);
        return !block_other->noreturn();
      }
      stack.o << AsmOp::Custom(loc, "IF:<{");
      stack.o.indent();
      Stack stack_copy{stack};
      stack_copy.mode &= ~Stack::_InlineFunc;
      block0->generate_code_all(stack_copy);
      stack_copy.drop_vars_except(loc, next->var_info);
      stack_copy.opt_show();
      stack.o.undent();
      stack.o << AsmOp::Custom({}, "}>ELSE<{");
      stack.o.indent();
      stack.mode &= ~Stack::_InlineFunc;
      block1->generate_code_all(stack);
      stack.merge_state(loc, stack_copy);
      stack.opt_show();
      stack.o.undent();
      stack.o << AsmOp::Custom({}, "}>");
      return true;
    }
    case _Repeat: {
      var_idx_t x = left[0];
      //stack.drop_vars_except(block0->var_info, x);
      stack.rearrange_top(loc, x, var_info[x] && var_info[x]->is_last());
      tolk_assert(stack[0] == x);
      stack.opt_show();
      stack.s.pop_back();
      stack.modified();
      if (block0->noreturn()) {
        stack.o.retalt_ = true;
      }
      if (true || !next->is_empty()) {
        stack.o << AsmOp::Custom(loc, "REPEAT:<{");
        stack.o.indent();
        stack.forget_const();
        if (block0->noreturn()) {
          Stack stack_copy{stack};
          StackLayout layout1 = stack.vars();
          stack_copy.mode &= ~Stack::_InlineFunc;
          stack_copy.mode |= Stack::_NeedRetAlt;
          block0->generate_code_all(stack_copy);
        } else {
          StackLayout layout1 = stack.vars();
          stack.mode &= ~Stack::_InlineFunc;
          stack.mode |= Stack::_NeedRetAlt;
          block0->generate_code_all(stack);
          stack.enforce_state(loc, std::move(layout1));
          stack.opt_show();
        }
        stack.o.undent();
        stack.o << AsmOp::Custom({}, "}>");
        return true;
      } else {
        stack.o << AsmOp::Custom(loc, "REPEATEND");
        stack.forget_const();
        StackLayout layout1 = stack.vars();
        block0->generate_code_all(stack);
        stack.enforce_state(loc, std::move(layout1));
        stack.opt_show();
        return false;
      }
    }
    case _Again: {
      stack.drop_vars_except(loc, block0->var_info);
      stack.opt_show();
      if (block0->noreturn()) {
        stack.o.retalt_ = true;
      }
      if (!next->is_empty() || inline_func) {
        stack.o << AsmOp::Custom(loc, "AGAIN:<{");
        stack.o.indent();
        stack.forget_const();
        StackLayout layout1 = stack.vars();
        stack.mode &= ~Stack::_InlineFunc;
        stack.mode |= Stack::_NeedRetAlt;
        block0->generate_code_all(stack);
        stack.enforce_state(loc, std::move(layout1));
        stack.opt_show();
        stack.o.undent();
        stack.o << AsmOp::Custom({}, "}>");
        return true;
      } else {
        stack.o << AsmOp::Custom(loc, "AGAINEND");
        stack.forget_const();
        StackLayout layout1 = stack.vars();
        block0->generate_code_all(stack);
        stack.enforce_state(loc, std::move(layout1));
        stack.opt_show();
        return false;
      }
    }
    case _Until: {
      // stack.drop_vars_except(block0->var_info);
      // stack.opt_show();
      if (block0->noreturn()) {
        stack.o.retalt_ = true;
      }
      if (true || !next->is_empty()) {
        stack.o << AsmOp::Custom(loc, "UNTIL:<{");
        stack.o.indent();
        stack.forget_const();
        auto layout1 = stack.vars();
        stack.mode &= ~Stack::_InlineFunc;
        stack.mode |= Stack::_NeedRetAlt;
        block0->generate_code_all(stack);
        layout1.push_back(left[0]);
        stack.enforce_state(loc, std::move(layout1));
        stack.opt_show();
        stack.o.undent();
        stack.o << AsmOp::Custom({}, "}>");
        stack.s.pop_back();
        stack.modified();
        return true;
      } else {
        stack.o << AsmOp::Custom(loc, "UNTILEND");
        stack.forget_const();
        StackLayout layout1 = stack.vars();
        block0->generate_code_all(stack);
        layout1.push_back(left[0]);
        stack.enforce_state(loc, std::move(layout1));
        stack.opt_show();
        return false;
      }
    }
    case _While: {
      // while (block0 | left) block1; ...next
      var_idx_t x = left[0];
      stack.drop_vars_except(loc, block0->var_info);
      stack.opt_show();
      StackLayout layout1 = stack.vars();
      bool next_empty = false && next->is_empty();
      if (block0->noreturn()) {
        stack.o.retalt_ = true;
      }
      stack.o << AsmOp::Custom(loc, "WHILE:<{");
      stack.o.indent();
      stack.forget_const();
      stack.mode &= ~Stack::_InlineFunc;
      stack.mode |= Stack::_NeedRetAlt;
      block0->generate_code_all(stack);
      stack.rearrange_top(loc, x, !next->var_info[x] && !block1->var_info[x]);
      stack.opt_show();
      stack.s.pop_back();
      stack.modified();
      stack.o.undent();
      Stack stack_copy{stack};
      stack.o << AsmOp::Custom(loc, next_empty ? "}>DO:" : "}>DO<{");
      if (!next_empty) {
        stack.o.indent();
      }
      stack_copy.opt_show();
      block1->generate_code_all(stack_copy);
      stack_copy.enforce_state(loc, std::move(layout1));
      stack_copy.opt_show();
      if (!next_empty) {
        stack.o.undent();
        stack.o << AsmOp::Custom({}, "}>");
        return true;
      } else {
        return false;
      }
    }
    case _TryCatch: {
      if (block0->is_empty() && block1->is_empty()) {
        return true;
      }
      if (block0->noreturn() || block1->noreturn()) {
        stack.o.retalt_ = true;
      }
      Stack catch_stack{stack.o, 0};
      std::vector<var_idx_t> catch_vars;
      std::vector<bool> catch_last;
      for (const VarDescr& var : block1->var_info.list) {
        if (stack.find(var.idx) >= 0) {
          catch_vars.push_back(var.idx);
          catch_last.push_back(!block0->var_info[var.idx]);
        }
      }
      const size_t block_size = 255;
      for (size_t begin = catch_vars.size(), end = begin; end > 0; end = begin) {
        begin = end >= block_size ? end - block_size : 0;
        for (size_t i = begin; i < end; ++i) {
          catch_stack.push_new_var(catch_vars[i]);
        }
      }
      catch_stack.push_new_var(left[0]);
      catch_stack.push_new_var(left[1]);
      stack.rearrange_top(loc, catch_vars, catch_last);
      stack.opt_show();
      stack.o << AsmOp::Custom(loc, "c1 PUSH");
      stack.o << AsmOp::Custom(loc, "c3 PUSH");
      stack.o << AsmOp::Custom(loc, "c4 PUSH");
      stack.o << AsmOp::Custom(loc, "c5 PUSH");
      stack.o << AsmOp::Custom(loc, "c7 PUSH");
      stack.o << AsmOp::Custom(loc, "<{");
      stack.o.indent();
      if (block1->noreturn()) {
        catch_stack.mode |= Stack::_NeedRetAlt;
      }
      block1->generate_code_all(catch_stack);
      catch_stack.drop_vars_except(loc, next->var_info);
      catch_stack.opt_show();
      stack.o.undent();
      stack.o << AsmOp::Custom({}, "}>CONT");
      stack.o << AsmOp::Custom(loc, "c7 SETCONT");
      stack.o << AsmOp::Custom(loc, "c5 SETCONT");
      stack.o << AsmOp::Custom(loc, "c4 SETCONT");
      stack.o << AsmOp::Custom(loc, "c3 SETCONT");
      stack.o << AsmOp::Custom(loc, "c1 SETCONT");
      for (size_t begin = catch_vars.size(), end = begin; end > 0; end = begin) {
        begin = end >= block_size ? end - block_size : 0;
        stack.o << AsmOp::Custom(loc, std::to_string(end - begin) + " PUSHINT");
        stack.o << AsmOp::Custom(loc, "-1 PUSHINT");
        stack.o << AsmOp::Custom(loc, "SETCONTVARARGS");
      }
      stack.s.erase(stack.s.end() - catch_vars.size(), stack.s.end());
      stack.modified();
      stack.o << AsmOp::Custom(loc, "<{");
      stack.o.indent();
      if (block0->noreturn()) {
        stack.mode |= Stack::_NeedRetAlt;
      }
      block0->generate_code_all(stack);
      if (block0->noreturn()) {
        stack.s = std::move(catch_stack.s);
      } else if (!block1->noreturn()) {
        stack.merge_state(loc, catch_stack);
      }
      stack.opt_show();
      stack.o.undent();
      stack.o << AsmOp::Custom({}, "}>CONT");
      stack.o << AsmOp::Custom(loc, "c1 PUSH");
      stack.o << AsmOp::Custom(loc, "COMPOSALT");
      stack.o << AsmOp::Custom(loc, "SWAP");
      stack.o << AsmOp::Custom(loc, "TRY");
      return true;
    }
    default:
      std::cerr << "fatal: unknown operation <??" << cl << ">\n";
      throw ParseError(loc, "unknown operation in generate_code()");
  }
}

void Op::generate_code_all(Stack& stack) {
  int saved_mode = stack.mode;
  auto cont = generate_code_step(stack);
  stack.mode = (stack.mode & ~Stack::_ModeSave) | (saved_mode & Stack::_ModeSave);
  if (cont && next) {
    next->generate_code_all(stack);
  }
}

void CodeBlob::generate_code(std::ostream& os, int mode, int indent) const {
  AsmOpList out_list(indent, &vars);
  Stack stack{out_list, mode};
  tolk_assert(ops && ops->cl == Op::_Import);
  int n_import_width = static_cast<int>(ops->left.size());
  for (var_idx_t x : ops->left) {
    stack.push_new_var(x);
  }
  ops->generate_code_all(stack);
  stack.apply_wrappers(fun_ref->loc, require_callxargs && (mode & Stack::_InlineAny) ? n_import_width : -1);
  if (G.settings.optimization_level >= 2) {
    optimize_code(out_list);
  }
  out_list.out(os, mode);
}

}  // namespace tolk
