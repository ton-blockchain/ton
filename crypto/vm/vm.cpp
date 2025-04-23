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

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "vm/dispatch.h"
#include "vm/continuation.h"
#include "vm/dict.h"
#include "vm/log.h"
#include "vm/vm.h"
#include "cp0.h"
#include "memo.h"

#include <sodium.h>

namespace vm {

VmState::VmState() : cp(-1), dispatch(&dummy_dispatch_table), quit0(true, 0), quit1(true, 1) {
  ensure_throw(init_cp(0));
  init_cregs();
}

VmState::VmState(Ref<CellSlice> _code, int global_version, Ref<Stack> _stack, const GasLimits& gas, int flags,
                 Ref<Cell> _data, VmLog log, std::vector<Ref<Cell>> _libraries, Ref<Tuple> init_c7)
    : code(std::move(_code))
    , stack(std::move(_stack))
    , cp(-1)
    , dispatch(&dummy_dispatch_table)
    , quit0(true, 0)
    , quit1(true, 1)
    , log(log)
    , gas(gas)
    , libraries(std::move(_libraries))
    , stack_trace((flags >> 2) & 1)
    , global_version(global_version) {
  ensure_throw(init_cp(0));
  set_c4(std::move(_data));
  if (init_c7.not_null()) {
    set_c7(std::move(init_c7));
  }
  init_cregs(flags & 1, flags & 2);
}

void VmState::init_cregs(bool same_c3, bool push_0) {
  cr.set_c0(quit0);
  cr.set_c1(quit1);
  cr.set_c2(Ref<ExcQuitCont>{true});
  if (same_c3) {
    cr.set_c3(Ref<OrdCont>{true, code, cp});
    if (push_0) {
      VM_LOG(this) << "implicit PUSH 0 at start\n";
      get_stack().push_smallint(0);
    }
  } else {
    cr.set_c3(Ref<QuitCont>{true, 11});
  }
  if (cr.d[0].is_null() || cr.d[1].is_null()) {
    auto empty_cell = CellBuilder{}.finalize();
    for (int i = 0; i < ControlRegs::dreg_num; i++) {
      if (cr.d[i].is_null()) {
        cr.d[i] = empty_cell;
      }
    }
  }
  if (cr.c7.is_null()) {
    cr.set_c7(Ref<Tuple>{true});
  }
}

Ref<CellSlice> VmState::convert_code_cell(Ref<Cell> code_cell, int global_version,
                                          const std::vector<Ref<Cell>>& libraries) {
  if (code_cell.is_null()) {
    return {};
  }
  Ref<CellSlice> csr;
  if (global_version >= 9) {
    // Use DummyVmState instead of this to avoid consuming gas for cell loading
    DummyVmState dummy{libraries, global_version};
    Guard guard(&dummy);
    try {
      csr = load_cell_slice_ref(code_cell);
    } catch (VmError&) {  // NOLINT(*-empty-catch)
    }
  } else {
    csr = td::Ref<CellSlice>{true, NoVmOrd(), code_cell};
  }
  if (csr.not_null() && csr->is_valid()) {
    return csr;
  }
  return load_cell_slice_ref(CellBuilder{}.store_ref(std::move(code_cell)).finalize());
}

bool VmState::init_cp(int new_cp) {
  const DispatchTable* dt = DispatchTable::get_table(new_cp);
  if (dt) {
    cp = new_cp;
    dispatch = dt;
    return true;
  } else {
    return false;
  }
}

bool VmState::set_cp(int new_cp) {
  return new_cp == cp || init_cp(new_cp);
}

void VmState::force_cp(int new_cp) {
  if (!set_cp(new_cp)) {
    throw VmError{Excno::inv_opcode, "unsupported codepage"};
  }
}

// simple call to a continuation cont
int VmState::call(Ref<Continuation> cont) {
  const ControlData* cont_data = cont->get_cdata();
  if (cont_data) {
    if (cont_data->save.c[0].not_null()) {
      // call reduces to a jump
      return jump(std::move(cont));
    }
    if (cont_data->stack.not_null() || cont_data->nargs >= 0) {
      // if cont has non-empty stack or expects fixed number of arguments, call is not simple
      return call(std::move(cont), -1, -1);
    }
    // create return continuation, to be stored into new c0
    Ref<OrdCont> ret = Ref<OrdCont>{true, std::move(code), cp};
    ret.unique_write().get_cdata()->save.set_c0(std::move(cr.c[0]));
    cr.set_c0(
        std::move(ret));  // set c0 to its final value before switching to cont; notice that cont.save.c0 is not set
    return jump_to(std::move(cont));
  }
  // create return continuation, to be stored into new c0
  Ref<OrdCont> ret = Ref<OrdCont>{true, std::move(code), cp};
  ret.unique_write().get_cdata()->save.set_c0(std::move(cr.c[0]));
  // general implementation of a simple call
  cr.set_c0(std::move(ret));  // set c0 to its final value before switching to cont; notice that cont.save.c0 is not set
  return jump_to(std::move(cont));
}

// call with parameters to continuation cont
int VmState::call(Ref<Continuation> cont, int pass_args, int ret_args) {
  const ControlData* cont_data = cont->get_cdata();
  if (cont_data) {
    if (cont_data->save.c[0].not_null()) {
      // call reduces to a jump
      return jump(std::move(cont), pass_args);
    }
    int depth = stack->depth();
    if (pass_args > depth || cont_data->nargs > depth) {
      throw VmError{Excno::stk_und, "stack underflow while calling a continuation: not enough arguments on stack"};
    }
    if (cont_data->nargs > pass_args && pass_args >= 0) {
      throw VmError{Excno::stk_und,
                    "stack underflow while calling a closure continuation: not enough arguments passed"};
    }
    auto old_c0 = std::move(cr.c[0]);
    // optimization(?): decrease refcnts of unused continuations in c[i] as early as possible
    preclear_cr(cont_data->save);
    // no exceptions should be thrown after this point
    int copy = cont_data->nargs, skip = 0;
    if (pass_args >= 0) {
      if (copy >= 0) {
        skip = pass_args - copy;
      } else {
        copy = pass_args;
      }
    }
    // copy=-1 : pass whole stack, else pass top `copy` elements, drop next `skip` elements.
    Ref<Stack> new_stk;
    if (cont_data->stack.not_null() && !cont_data->stack->is_empty()) {
      // `cont` already has a stack, create resulting stack from it
      if (copy < 0) {
        copy = stack->depth();
      }
      if (cont->is_unique()) {
        // optimization: avoid copying stack if we hold the only copy of `cont`
        new_stk = std::move(cont.unique_write().get_cdata()->stack);
      } else {
        new_stk = cont_data->stack;
      }
      new_stk.write().move_from_stack(get_stack(), copy);
      if (skip > 0) {
        get_stack().pop_many(skip);
      }
      consume_stack_gas(new_stk);
    } else if (copy >= 0) {
      new_stk = get_stack().split_top(copy, skip);
      consume_stack_gas(new_stk);
    } else {
      new_stk = std::move(stack);
      stack.clear();
    }
    // create return continuation using the remainder of current stack
    Ref<OrdCont> ret = Ref<OrdCont>{true, std::move(code), cp, std::move(stack), ret_args};
    ret.unique_write().get_cdata()->save.set_c0(std::move(old_c0));
    Ref<OrdCont> ord_cont = static_cast<Ref<OrdCont>>(cont);
    set_stack(std::move(new_stk));
    cr.set_c0(std::move(ret));  // ??? if codepage of code in ord_cont is unknown, will end up with incorrect c0
    return jump_to(std::move(cont));
  } else {
    // have no continuation data, situation is somewhat simpler
    int depth = stack->depth();
    if (pass_args > depth) {
      throw VmError{Excno::stk_und, "stack underflow while calling a continuation: not enough arguments on stack"};
    }
    // create new stack from the top `pass_args` elements of the current stack
    Ref<Stack> new_stk;
    if (pass_args >= 0) {
      new_stk = get_stack().split_top(pass_args);
      consume_stack_gas(new_stk);
    } else {
      new_stk = std::move(stack);
    }
    // create return continuation using the remainder of the current stack
    Ref<OrdCont> ret = Ref<OrdCont>{true, std::move(code), cp, std::move(stack), ret_args};
    ret.unique_write().get_cdata()->save.set_c0(std::move(cr.c[0]));
    set_stack(std::move(new_stk));
    cr.set_c0(std::move(ret));  // ??? if codepage of code in ord_cont is unknown, will end up with incorrect c0
    return jump_to(std::move(cont));
  }
}

// simple jump to continuation cont
int VmState::jump(Ref<Continuation> cont) {
  const ControlData* cont_data = cont->get_cdata();
  if (cont_data && (cont_data->stack.not_null() || cont_data->nargs >= 0)) {
    // if cont has non-empty stack or expects fixed number of arguments, jump is not simple
    return jump(std::move(cont), -1);
  } else {
    return jump_to(std::move(cont));
  }
}

// general jump to continuation cont
int VmState::jump(Ref<Continuation> cont, int pass_args) {
  cont = adjust_jump_cont(std::move(cont), pass_args);
  return jump_to(std::move(cont));
}

Ref<Continuation> VmState::adjust_jump_cont(Ref<Continuation> cont, int pass_args) {
  const ControlData* cont_data = cont->get_cdata();
  if (cont_data) {
    // first do the checks
    int depth = stack->depth();
    if (pass_args > depth || cont_data->nargs > depth) {
      throw VmError{Excno::stk_und, "stack underflow while jumping to a continuation: not enough arguments on stack"};
    }
    if (cont_data->nargs > pass_args && pass_args >= 0) {
      throw VmError{Excno::stk_und,
                    "stack underflow while jumping to closure continuation: not enough arguments passed"};
    }
    // optimization(?): decrease refcnts of unused continuations in c[i] as early as possible
    preclear_cr(cont_data->save);
    // no exceptions should be thrown after this point
    int copy = cont_data->nargs;
    if (pass_args >= 0 && copy < 0) {
      copy = pass_args;
    }
    // copy=-1 : pass whole stack, else pass top `copy` elements, drop the remainder.
    if (cont_data->stack.not_null() && !cont_data->stack->is_empty()) {
      // `cont` already has a stack, create resulting stack from it
      if (copy < 0) {
        copy = get_stack().depth();
      }
      Ref<Stack> new_stk;
      if (cont->is_unique()) {
        // optimization: avoid copying the stack if we hold the only copy of `cont`
        new_stk = std::move(cont.unique_write().get_cdata()->stack);
      } else {
        new_stk = cont_data->stack;
      }
      new_stk.write().move_from_stack(get_stack(), copy);
      consume_stack_gas(new_stk);
      set_stack(std::move(new_stk));
    } else {
      if (copy >= 0 && copy < stack->depth()) {
        get_stack().drop_bottom(stack->depth() - copy);
        consume_stack_gas(copy);
      }
    }
    return cont;
  } else {
    // have no continuation data, situation is somewhat simpler
    if (pass_args >= 0) {
      int depth = get_stack().depth();
      if (pass_args > depth) {
        throw VmError{Excno::stk_und, "stack underflow while jumping to a continuation: not enough arguments on stack"};
      } else if (pass_args < depth) {
        get_stack().drop_bottom(depth - pass_args);
        consume_stack_gas(pass_args);
      }
    }
    return cont;
  }
}

int VmState::ret() {
  Ref<Continuation> cont = quit0;
  cont.swap(cr.c[0]);
  return jump(std::move(cont));
}

int VmState::ret(int ret_args) {
  Ref<Continuation> cont = quit0;
  cont.swap(cr.c[0]);
  return jump(std::move(cont), ret_args);
}

int VmState::ret_alt() {
  Ref<Continuation> cont = quit1;
  cont.swap(cr.c[1]);
  return jump(std::move(cont));
}

int VmState::ret_alt(int ret_args) {
  Ref<Continuation> cont = quit1;
  cont.swap(cr.c[1]);
  return jump(std::move(cont), ret_args);
}

Ref<Continuation> VmState::c1_envelope(Ref<Continuation> cont, bool save) {
  if (save) {
    force_cregs(cont)->define_c1(cr.c[1]);
    force_cregs(cont)->define_c0(cr.c[0]);
  }
  set_c1(cont);
  return cont;
}

void VmState::c1_save_set(bool save) {
  if (save) {
    force_cregs(cr.c[0])->define_c1(cr.c[1]);
  }
  set_c1(cr.c[0]);
}

Ref<OrdCont> VmState::extract_cc(int save_cr, int stack_copy, int cc_args) {
  Ref<Stack> new_stk;
  if (stack_copy < 0 || stack_copy == stack->depth()) {
    new_stk = std::move(stack);
    stack.clear();
  } else if (stack_copy > 0) {
    stack->check_underflow(stack_copy);
    new_stk = get_stack().split_top(stack_copy);
    consume_stack_gas(new_stk);
  } else {
    new_stk = Ref<Stack>{true};
  }
  Ref<OrdCont> cc = Ref<OrdCont>{true, std::move(code), cp, std::move(stack), cc_args};
  stack = std::move(new_stk);
  if (save_cr & 7) {
    ControlData* cdata = cc.unique_write().get_cdata();
    if (save_cr & 1) {
      cdata->save.set_c0(std::move(cr.c[0]));
      cr.set_c0(quit0);
    }
    if (save_cr & 2) {
      cdata->save.set_c1(std::move(cr.c[1]));
      cr.set_c1(quit1);
    }
    if (save_cr & 4) {
      cdata->save.set_c2(std::move(cr.c[2]));
      // cr.set_c2(Ref<ExcQuitCont>{true});
    }
  }
  return cc;
}

int VmState::throw_exception(int excno) {
  Stack& stack_ref = get_stack();
  stack_ref.clear();
  stack_ref.push_smallint(0);
  stack_ref.push_smallint(excno);
  code.clear();
  consume_gas_chk(exception_gas_price);
  return jump(get_c2());
}

int VmState::throw_exception(int excno, StackEntry&& arg) {
  Stack& stack_ref = get_stack();
  stack_ref.clear();
  stack_ref.push(std::move(arg));
  stack_ref.push_smallint(excno);
  code.clear();
  consume_gas_chk(exception_gas_price);
  return jump(get_c2());
}

void GasLimits::gas_exception() const {
  throw VmNoGas{};
}

void GasLimits::set_limits(long long _max, long long _limit, long long _credit) {
  gas_max = _max;
  gas_limit = _limit;
  gas_credit = _credit;
  change_base(_limit + _credit);
}

void GasLimits::change_limit(long long _limit) {
  _limit = std::min(std::max(_limit, 0LL), gas_max);
  gas_credit = 0;
  gas_limit = _limit;
  change_base(_limit);
}

bool VmState::set_gas_limits(long long _max, long long _limit, long long _credit) {
  gas.set_limits(_max, _limit, _credit);
  return true;
}

void VmState::change_gas_limit(long long new_limit) {
  VM_LOG(this) << "changing gas limit to " << std::min(new_limit, gas.gas_max);
  gas.change_limit(new_limit);
}

int VmState::step() {
  CHECK(code.not_null() && stack.not_null());
  if (log.log_mask & vm::VmLog::DumpStack) {
    std::stringstream ss;
    int mode = 3;
    if (log.log_mask & vm::VmLog::DumpStackVerbose) {
      mode += 4;
    }
    std::unique_ptr<VmStateInterface> tmp_ctx;
    // install temporary dummy vm state interface to prevent charging for cell load operations during dump
    VmStateInterface::Guard guard(tmp_ctx.get());
    stack->dump(ss, mode);
    VM_LOG(this) << "stack:" << ss.str();
  }
  if (stack_trace) {
    std::unique_ptr<VmStateInterface> tmp_ctx;
    // install temporary dummy vm state interface to prevent charging for cell load operations during dump
    VmStateInterface::Guard guard(tmp_ctx.get());
    stack->dump(std::cerr, 3);
  }
  ++steps;
  if (code->size()) {
    VM_LOG_MASK(this, vm::VmLog::ExecLocation) << "code cell hash: " << code->get_base_cell()->get_hash().to_hex() << " offset: " << code->cur_pos();
    return dispatch->dispatch(this, code.write());
  } else if (code->size_refs()) {
    VM_LOG(this) << "execute implicit JMPREF";
    auto ref_cell = code->prefetch_ref();
    VM_LOG_MASK(this, vm::VmLog::ExecLocation) << "code cell hash: " << ref_cell->get_hash().to_hex() << " offset: 0";
    consume_gas_chk(implicit_jmpref_gas_price);
    Ref<Continuation> cont = Ref<OrdCont>{true, load_cell_slice_ref(std::move(ref_cell)), get_cp()};
    return jump(std::move(cont));
  } else {
    VM_LOG(this) << "execute implicit RET";
    consume_gas_chk(implicit_ret_gas_price);
    return ret();
  }
}

int VmState::run_inner() {
  int res;
  Guard guard(this);
  do {
    try {
      try {
        res = step();
        VM_LOG_MASK(this, vm::VmLog::GasRemaining) << "gas remaining: " << gas.gas_remaining;
        gas.check();
      } catch (vm::CellBuilder::CellWriteError) {
        throw VmError{Excno::cell_ov};
      } catch (vm::CellBuilder::CellCreateError) {
        throw VmError{Excno::cell_ov};
      } catch (vm::CellSlice::CellReadError) {
        throw VmError{Excno::cell_und};
      }
    } catch (const VmError& vme) {
      VM_LOG(this) << "handling exception code " << vme.get_errno() << ": " << vme.get_msg();
      try {
        ++steps;
        res = throw_exception(vme.get_errno());
      } catch (const VmError& vme2) {
        VM_LOG(this) << "exception " << vme2.get_errno() << " while handling exception: " << vme.get_msg();
        return ~vme2.get_errno();
      }
    }
  } while (!res);
  if ((res | 1) == -1 && !try_commit()) {
    VM_LOG(this) << "automatic commit failed (new data or action cells too deep)";
    get_stack().clear();
    get_stack().push_smallint(0);
    return ~(int)Excno::cell_ov;
  }
  return res;
}

int VmState::run() {
  if (code.is_null() || stack.is_null()) {
    // throw VmError{Excno::fatal, "cannot run an uninitialized VM"};
    return (int)Excno::fatal;  // no ~ for unhandled exceptions
  }
  int res = 0;
  bool restore_parent = false;
  while (true) {
    try {
      if (restore_parent) {
        restore_parent_vm(~res);
      }
      res = run_inner();
    } catch (VmNoGas &vmoog) {
      ++steps;
      VM_LOG(this) << "unhandled out-of-gas exception: gas consumed=" << gas.gas_consumed()
                   << ", limit=" << gas.gas_limit;
      get_stack().clear();
      get_stack().push_smallint(gas.gas_consumed());
      res = vmoog.get_errno();  // no ~ for unhandled exceptions (to make their faking impossible)
    }
    if (!parent) {
      if ((log.log_mask & VmLog::DumpC5) && cstate.committed) {
        std::stringstream ss;
        ss << "final c5: ";
        StackEntry::maybe<Cell>(cstate.c5).dump(ss, true);
        ss << "\n";
        VM_LOG(this) << ss.str();
      }
      return res;
    }
    restore_parent = true;
  }
}

bool VmState::try_commit() {
  if (cr.d[0].not_null() && cr.d[1].not_null() && cr.d[0]->get_depth() <= max_data_depth &&
      cr.d[1]->get_depth() <= max_data_depth && cr.d[0]->get_level() == 0 && cr.d[1]->get_level() == 0) {
    cstate.c4 = cr.d[0];
    cstate.c5 = cr.d[1];
    cstate.committed = true;
    return true;
  } else {
    return false;
  }
}

void VmState::force_commit() {
  if (!try_commit()) {
    throw VmError{Excno::cell_ov, "cannot commit too deep cells as new data/actions"};
  }
}

ControlData* force_cdata(Ref<Continuation>& cont) {
  if (!cont->get_cdata()) {
    cont = Ref<ArgContExt>{true, cont};
    return cont.unique_write().get_cdata();
  } else {
    return cont.write().get_cdata();
  }
}

ControlRegs* force_cregs(Ref<Continuation>& cont) {
  return &force_cdata(cont)->save;
}

int run_vm_code(Ref<CellSlice> code, Ref<Stack>& stack, int flags, Ref<Cell>* data_ptr, VmLog log, long long* steps,
                GasLimits* gas_limits, std::vector<Ref<Cell>> libraries, Ref<Tuple> init_c7, Ref<Cell>* actions_ptr,
                int global_version) {
  VmState vm{code,
             global_version,
             std::move(stack),
             gas_limits ? *gas_limits : GasLimits{},
             flags,
             data_ptr ? *data_ptr : Ref<Cell>{},
             log,
             std::move(libraries),
             std::move(init_c7)};
  int res = vm.run();
  stack = vm.get_stack_ref();
  if (vm.committed() && data_ptr) {
    *data_ptr = vm.get_committed_state().c4;
  }
  if (vm.committed() && actions_ptr) {
    *actions_ptr = vm.get_committed_state().c5;
  }
  if (steps) {
    *steps = vm.get_steps_count();
  }
  if (gas_limits) {
    *gas_limits = vm.get_gas_limits();
    LOG(INFO) << "steps: " << vm.get_steps_count() << " gas: used=" << gas_limits->gas_consumed()
              << ", max=" << gas_limits->gas_max << ", limit=" << gas_limits->gas_limit
              << ", credit=" << gas_limits->gas_credit;
  }
  if ((vm.get_log().log_mask & vm::VmLog::DumpStack) != 0) {
    VM_LOG(&vm) << "BEGIN_STACK_DUMP";
    for (int i = stack->depth(); i > 0; i--) {
      VM_LOG(&vm) << (*stack)[i - 1].to_string();
    }
    VM_LOG(&vm) << "END_STACK_DUMP";
  }

  return ~res;
}

int run_vm_code(Ref<CellSlice> code, Stack& stack, int flags, Ref<Cell>* data_ptr, VmLog log, long long* steps,
                GasLimits* gas_limits, std::vector<Ref<Cell>> libraries, Ref<Tuple> init_c7, Ref<Cell>* actions_ptr,
                int global_version) {
  Ref<Stack> stk{true};
  stk.unique_write().set_contents(std::move(stack));
  stack.clear();
  int res = run_vm_code(code, stk, flags, data_ptr, log, steps, gas_limits, std::move(libraries), std::move(init_c7),
                        actions_ptr, global_version);
  CHECK(stack.is_unique());
  if (stk.is_null()) {
    stack.clear();
  } else if (&(*stk) != &stack) {
    VmState* st = nullptr;
    if (stk->is_unique()) {
      VM_LOG(st) << "move resulting stack (" << stk->depth() << " entries)";
      stack.set_contents(std::move(stk.unique_write()));
    } else {
      VM_LOG(st) << "copying resulting stack (" << stk->depth() << " entries)";
      stack.set_contents(*stk);
    }
  }
  return res;
}

// may throw a dictionary exception; returns nullptr if library is not found in context
Ref<Cell> VmState::load_library(td::ConstBitPtr hash) {
  std::unique_ptr<VmStateInterface> tmp_ctx;
  // install temporary dummy vm state interface to prevent charging for cell load operations during library lookup
  VmStateInterface::Guard guard{global_version >= 4 ? tmp_ctx.get() : VmStateInterface::get()};
  for (const auto& lib_collection : libraries) {
    auto lib = lookup_library_in(hash, lib_collection);
    if (lib.not_null()) {
      return lib;
    }
  }
  missing_library = td::Bits256{hash};
  return {};
}

bool VmState::register_library_collection(Ref<Cell> lib) {
  if (lib.is_null()) {
    return true;
  }
  libraries.push_back(std::move(lib));
  return true;
}

void VmState::register_cell_load(const CellHash& cell_hash) {
  auto new_cell = loaded_cells.insert(cell_hash).second;  // check whether this is the first time this cell is loaded
  consume_gas(new_cell ? cell_load_gas_price : cell_reload_gas_price);
}

bool VmState::register_cell_load_free(const CellHash& cell_hash) {
  return loaded_cells.insert(cell_hash).second;
}

void VmState::register_cell_create() {
  consume_gas(cell_create_gas_price);
}

td::BitArray<256> VmState::get_state_hash() const {
  // TODO: implement properly, by serializing the stack etc, and computing the Merkle hash
  td::BitArray<256> res;
  res.clear();
  return res;
}

td::BitArray<256> VmState::get_final_state_hash(int exit_code) const {
  // TODO: implement properly, by serializing the stack etc, and computing the Merkle hash
  td::BitArray<256> res;
  res.clear();
  return res;
}

Ref<vm::Cell> lookup_library_in(td::ConstBitPtr key, vm::Dictionary& dict) {
  try {
    auto val = dict.lookup(key, 256);
    if (val.is_null() || !val->have_refs()) {
      return {};
    }
    auto root = val->prefetch_ref();
    if (root.not_null() && !root->get_hash().bits().compare(key, 256)) {
      return root;
    }
    return {};
  } catch (vm::VmError) {
    return {};
  }
}

Ref<vm::Cell> lookup_library_in(td::ConstBitPtr key, Ref<vm::Cell> lib_root) {
  if (lib_root.is_null()) {
    return lib_root;
  }
  vm::Dictionary dict{std::move(lib_root), 256};
  return lookup_library_in(key, dict);
}

void VmState::run_child_vm(VmState&& new_state, bool return_data, bool return_actions, bool return_gas,
                           bool isolate_gas, int ret_vals) {
  if (global_version < 10) {
    new_state.log = std::move(log);
    new_state.libraries = std::move(libraries);
  }
  new_state.stack_trace = stack_trace;
  new_state.max_data_depth = max_data_depth;
  if (!isolate_gas) {
    new_state.loaded_cells = std::move(loaded_cells);
  } else {
    consume_gas(free_gas_consumed);
    chksgn_counter = 0;
    get_extra_balance_counter = 0;
    free_gas_consumed = 0;
  }
  if (global_version >= 10) {
    new_state.log = std::move(log);
    new_state.libraries = std::move(libraries);
  }
  new_state.chksgn_counter = chksgn_counter;
  new_state.free_gas_consumed = free_gas_consumed;
  new_state.get_extra_balance_counter = get_extra_balance_counter;
  if (global_version >= 10) {
    new_state.gas = GasLimits{std::min(new_state.gas.gas_limit, gas.gas_remaining),
                              std::min(new_state.gas.gas_max, gas.gas_remaining)};
  }

  auto new_parent = std::make_unique<ParentVmState>();
  new_parent->return_data = return_data;
  new_parent->return_actions = return_actions;
  new_parent->return_gas = return_gas;
  new_parent->isolate_gas = isolate_gas;
  new_parent->ret_vals = ret_vals;
  new_parent->state = std::move(*this);
  new_state.parent = std::move(new_parent);
  *this = std::move(new_state);
}

void VmState::restore_parent_vm(int res) {
  auto parent = std::move(this->parent);
  CHECK(parent);
  VmState child_state = std::move(*this);
  *this = std::move(parent->state);
  log = std::move(child_state.log);
  libraries = std::move(child_state.libraries);
  steps += child_state.steps;
  if (!parent->isolate_gas) {
    loaded_cells = std::move(child_state.loaded_cells);
  }
  chksgn_counter = child_state.chksgn_counter;
  get_extra_balance_counter = child_state.get_extra_balance_counter;
  free_gas_consumed = child_state.free_gas_consumed;
  VM_LOG(this) << "Child VM finished. res: " << res << ", steps: " << child_state.steps
               << ", gas: " << child_state.gas_consumed();

  consume_gas(std::min(child_state.gas_consumed(), child_state.gas.gas_limit + 1));
  Stack& cur_stack = get_stack();
  int ret_cnt;
  if (res == 0 || res == 1) {
    if (parent->ret_vals >= 0) {
      if (child_state.stack->depth() >= parent->ret_vals) {
        ret_cnt = parent->ret_vals;
      } else {
        ret_cnt = 0;
        res = ~(int)Excno::stk_und;
        cur_stack.push(td::zero_refint());
      }
    } else {
      ret_cnt = child_state.stack->depth();
    }
  } else {
    ret_cnt = std::min(child_state.stack->depth(), 1);
  }
  consume_stack_gas(ret_cnt);
  for (int i = ret_cnt - 1; i >= 0; --i) {
    cur_stack.push(std::move(child_state.stack->at(i)));
  }
  cur_stack.push_smallint(res);
  if (parent->return_data) {
    cur_stack.push_cell(child_state.get_committed_state().c4);
  }
  if (parent->return_actions) {
    cur_stack.push_cell(child_state.get_committed_state().c5);
  }
  if (parent->return_gas) {
    cur_stack.push_smallint(child_state.gas.gas_consumed());
  }
}

td::Status init_vm(bool enable_debug) {
  if (!init_op_cp0(enable_debug)) {
    return td::Status::Error("Failed to init TVM: failed to init cp0");
  }
  auto code = sodium_init();
  if (code < 0) {
    return td::Status::Error(PSTRING() << "Failed to init TVM: sodium_init, code=" << code);
  }
  return td::Status::OK();
}

}  // namespace vm
