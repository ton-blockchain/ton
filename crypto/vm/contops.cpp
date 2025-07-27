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
#include <functional>
#include "vm/contops.h"
#include "vm/log.h"
#include "vm/opctable.h"
#include "vm/stack.hpp"
#include "vm/continuation.h"
#include "vm/cellops.h"
#include "vm/excno.hpp"
#include "vm/vm.h"

using namespace std::literals::string_literals;

namespace vm {

int exec_execute(VmState* st) {
  VM_LOG(st) << "execute EXECUTE";
  auto cont = st->get_stack().pop_cont();
  return st->call(std::move(cont));
}

int exec_callx_args(VmState* st, unsigned args) {
  unsigned params = (args >> 4) & 15, retvals = (args & 15);
  VM_LOG(st) << "execute CALLXARGS " << params << ',' << retvals;
  auto cont = st->get_stack().pop_cont();
  return st->call(std::move(cont), params, retvals);
}

int exec_callx_args_p(VmState* st, unsigned args) {
  unsigned params = (args & 15);
  VM_LOG(st) << "execute CALLXARGS " << params << ",-1\n";
  auto cont = st->get_stack().pop_cont();
  return st->call(std::move(cont), params, -1);
}

int exec_jmpx(VmState* st) {
  VM_LOG(st) << "execute JMPX\n";
  auto cont = st->get_stack().pop_cont();
  return st->jump(std::move(cont));
}

int exec_jmpx_args(VmState* st, unsigned args) {
  unsigned params = args & 15;
  VM_LOG(st) << "execute JMPXARGS " << params;
  auto cont = st->get_stack().pop_cont();
  return st->jump(std::move(cont), params);
}

int exec_ret(VmState* st) {
  VM_LOG(st) << "execute RET\n";
  return st->ret();
}

int exec_ret_args(VmState* st, unsigned args) {
  unsigned params = args & 15;
  VM_LOG(st) << "execute RETARGS " << params;
  return st->ret(params);
}

int exec_ret_alt(VmState* st) {
  VM_LOG(st) << "execute RETALT\n";
  return st->ret_alt();
}

int exec_ret_bool(VmState* st) {
  VM_LOG(st) << "execute RETBOOL\n";
  return st->get_stack().pop_bool() ? st->ret() : st->ret_alt();
}

int exec_callcc(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute CALLCC\n";
  auto cont = stack.pop_cont();
  auto cc = st->extract_cc(3);
  st->get_stack().push_cont(std::move(cc));
  return st->jump(std::move(cont));
}

int exec_jmpx_data(VmState* st) {
  VM_LOG(st) << "execute JMPXDATA\n";
  auto cont = st->get_stack().pop_cont();
  st->push_code();
  return st->jump(std::move(cont));
}

int exec_callcc_args(VmState* st, unsigned args) {
  int params = (args >> 4) & 15, retvals = ((args + 1) & 15) - 1;
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute CALLCCARGS " << params << ',' << retvals;
  stack.check_underflow(params + 1);
  auto cont = stack.pop_cont();
  auto cc = st->extract_cc(3, params, retvals);
  st->get_stack().push_cont(std::move(cc));
  return st->jump(std::move(cont));
}

int exec_callx_varargs(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute CALLXVARARGS\n";
  stack.check_underflow(3);
  int retvals = stack.pop_smallint_range(254, -1);
  int params = stack.pop_smallint_range(254, -1);
  return st->call(stack.pop_cont(), params, retvals);
}

int exec_ret_varargs(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute RETVARARGS\n";
  int retvals = stack.pop_smallint_range(254, -1);
  return st->ret(retvals);
}

int exec_jmpx_varargs(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute JMPXVARARGS\n";
  stack.check_underflow(2);
  int params = stack.pop_smallint_range(254, -1);
  stack.check_underflow(params + 1);
  return st->jump(stack.pop_cont(), params);
}

int exec_callcc_varargs(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute CALLCCVARARGS\n";
  stack.check_underflow(3);
  int retvals = stack.pop_smallint_range(254, -1);
  int params = stack.pop_smallint_range(254, -1);
  stack.check_underflow(params + 1);
  auto cont = stack.pop_cont();
  auto cc = st->extract_cc(3, params, retvals);
  st->get_stack().push_cont(std::move(cc));
  return st->jump(std::move(cont));
}

int exec_do_with_ref(VmState* st, CellSlice& cs, int pfx_bits, const std::function<int(VmState*, Ref<OrdCont>)>& func,
                     const char* name) {
  if (!cs.have_refs(1)) {
    throw VmError{Excno::inv_opcode, "no references left for a "s + name + " instruction"};
  }
  cs.advance(pfx_bits);
  auto cell = cs.fetch_ref();
  VM_LOG(st) << "execute " << name << " (" << cell->get_hash().to_hex() << ")";
  return func(st, st->ref_to_cont(std::move(cell)));
}

int exec_do_with_cell(VmState* st, CellSlice& cs, int pfx_bits, const std::function<int(VmState*, Ref<Cell>)>& func,
                      const char* name) {
  if (!cs.have_refs(1)) {
    throw VmError{Excno::inv_opcode, "no references left for a "s + name + " instruction"};
  }
  cs.advance(pfx_bits);
  auto cell = cs.fetch_ref();
  VM_LOG(st) << "execute " << name << " (" << cell->get_hash().to_hex() << ")";
  return func(st, std::move(cell));
}

int exec_ifelse_ref(VmState* st, CellSlice& cs, int pfx_bits, bool mode) {
  const char* name = mode ? "IFREFELSE" : "IFELSEREF";
  if (!cs.have_refs(1)) {
    throw VmError{Excno::inv_opcode, "no references left for a "s + name + " instruction"};
  }
  cs.advance(pfx_bits);
  auto cell = cs.fetch_ref();
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute " << name << " (" << cell->get_hash().to_hex() << ")";
  stack.check_underflow(2);
  auto cont = stack.pop_cont();
  if (stack.pop_bool() == mode) {
    cont = st->ref_to_cont(std::move(cell));
  } else {
    cell.clear();
  }
  return st->call(std::move(cont));
}

int exec_ifref_elseref(VmState* st, CellSlice& cs, unsigned args, int pfx_bits) {
  if (!cs.have_refs(2)) {
    throw VmError{Excno::inv_opcode, "no references left for a IFREFELSEREF instruction"};
  }
  cs.advance(pfx_bits);
  auto cell1 = cs.fetch_ref(), cell2 = cs.fetch_ref();
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute IFREFELSEREF (" << cell1->get_hash().to_hex() << ") (" << cell2->get_hash().to_hex() << ")";
  if (!stack.pop_bool()) {
    cell1 = std::move(cell2);
  } else {
    cell2.clear();
  }
  return st->call(st->ref_to_cont(std::move(cell1)));
}

int exec_ret_data(VmState* st) {
  VM_LOG(st) << "execute RETDATA\n";
  st->push_code();
  return st->ret();
}

// Mode:
// +1 = same_c3 (set c3 to code)
// +2 = push_0 (push an implicit 0 before running the code); only works with +1 enabled
// +4 = load c4 (persistent data) from stack and return its final value
// +8 = load gas limit from stack and return consumed gas
// +16 = load c7 (smart-contract context)
// +32 = return c5 (actions)
// +64 = pop hard gas limit (enabled by ACCEPT) from stack as well
// +128 = isolated gas consumption (separate set of visited cells, reset chksgn counter)
// +256 = pop number N, return exactly N values from stack (only if res=0 or 1; if not enough then res=stk_und)
int exec_runvm_common(VmState* st, unsigned mode) {
  if (mode >= 512) {
    throw VmError{Excno::range_chk, "invalid flags"};
  }
  st->consume_gas(VmState::runvm_gas_price);
  Stack& stack = st->get_stack();
  bool with_data = mode & 4;
  Ref<vm::Tuple> c7;
  Ref<vm::Cell> data, actions;
  long long gas_max = (mode & 64) ? stack.pop_long_range(vm::GasLimits::infty) : vm::GasLimits::infty;
  long long gas_limit = (mode & 8) ? stack.pop_long_range(vm::GasLimits::infty) : vm::GasLimits::infty;
  if (!(mode & 64)) {
    gas_max = gas_limit;
  } else {
    gas_max = std::max(gas_max, gas_limit);
  }
  if (mode & 16) {
    c7 = stack.pop_tuple();
  }
  if (with_data) {
    data = stack.pop_cell();
  }
  int ret_vals = -1;
  if (mode & 256) {
    ret_vals = stack.pop_smallint_range(1 << 30);
  }
  auto code = stack.pop_cellslice();
  int stack_size = stack.pop_smallint_range(stack.depth() - 1);
  std::vector<StackEntry> new_stack_entries(stack_size);
  for (int i = 0; i < stack_size; ++i) {
    new_stack_entries[stack_size - 1 - i] = stack.pop();
  }
  td::Ref<Stack> new_stack{true, std::move(new_stack_entries)};
  st->consume_stack_gas(new_stack);
  gas_max = std::min(gas_max, st->get_gas_limits().gas_remaining);
  gas_limit = std::min(gas_limit, st->get_gas_limits().gas_remaining);
  vm::GasLimits gas{gas_limit, gas_max};

  VmStateInterface::Guard guard{nullptr}; // Don't consume gas for creating/loading cells during VM init
  VmState new_state{
      std::move(code), st->get_global_version(), std::move(new_stack), gas, (int)mode & 3, std::move(data),
      VmLog{},         std::vector<Ref<Cell>>{}, std::move(c7)};
  new_state.set_chksig_always_succeed(st->get_chksig_always_succeed());
  st->run_child_vm(std::move(new_state), with_data, mode & 32, mode & 8, mode & 128, ret_vals);
  return 0;
}

int exec_runvm(VmState* st, unsigned args) {
  VM_LOG(st) << "execute RUNVM " << (args & 4095) << "\n";
  return exec_runvm_common(st, args & 4095);
}

int exec_runvmx(VmState* st) {
  VM_LOG(st) << "execute RUNVMX\n";
  return exec_runvm_common(st, st->get_stack().pop_smallint_range(4095));
}

std::string dump_runvm(CellSlice&, unsigned args) {
  return PSTRING() << "RUNVM " << (args & 4095);
}

void register_continuation_jump_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xd8, 8, "EXECUTE", exec_execute))
      .insert(OpcodeInstr::mksimple(0xd9, 8, "JMPX", exec_jmpx))
      .insert(OpcodeInstr::mkfixed(0xda, 8, 8, instr::dump_2c("CALLXARGS ", ","), exec_callx_args))
      .insert(OpcodeInstr::mkfixed(0xdb0, 12, 4, instr::dump_1c("CALLXARGS ", ",-1"), exec_callx_args_p))
      .insert(OpcodeInstr::mkfixed(0xdb1, 12, 4, instr::dump_1c("JMPXARGS "), exec_jmpx_args))
      .insert(OpcodeInstr::mkfixed(0xdb2, 12, 4, instr::dump_1c("RETARGS "), exec_ret_args))
      .insert(OpcodeInstr::mksimple(0xdb30, 16, "RET", exec_ret))
      .insert(OpcodeInstr::mksimple(0xdb31, 16, "RETALT", exec_ret_alt))
      .insert(OpcodeInstr::mksimple(0xdb32, 16, "RETBOOL", exec_ret_bool))
      .insert(OpcodeInstr::mksimple(0xdb34, 16, "CALLCC", exec_callcc))
      .insert(OpcodeInstr::mksimple(0xdb35, 16, "JMPXDATA", exec_jmpx_data))
      .insert(OpcodeInstr::mkfixed(0xdb36, 16, 8, instr::dump_2c("CALLCCARGS ", ","), exec_callcc_args))
      .insert(OpcodeInstr::mksimple(0xdb38, 16, "CALLXVARARGS", exec_callx_varargs))
      .insert(OpcodeInstr::mksimple(0xdb39, 16, "RETVARARGS", exec_ret_varargs))
      .insert(OpcodeInstr::mksimple(0xdb3a, 16, "JMPXVARARGS", exec_jmpx_varargs))
      .insert(OpcodeInstr::mksimple(0xdb3b, 16, "CALLCCVARARGS", exec_callcc_varargs))
      .insert(OpcodeInstr::mkext(0xdb3c, 16, 0, std::bind(dump_push_ref, _1, _2, _3, "CALLREF"),
                                 std::bind(exec_do_with_ref, _1, _2, _4,
                                           [](auto st, auto cont) { return st->call(std::move(cont)); }, "CALLREF"),
                                 compute_len_push_ref))
      .insert(OpcodeInstr::mkext(0xdb3d, 16, 0, std::bind(dump_push_ref, _1, _2, _3, "JMPREF"),
                                 std::bind(exec_do_with_ref, _1, _2, _4,
                                           [](auto st, auto cont) { return st->jump(std::move(cont)); }, "JMPREF"),
                                 compute_len_push_ref))
      .insert(OpcodeInstr::mkext(0xdb3e, 16, 0, std::bind(dump_push_ref, _1, _2, _3, "JMPREFDATA"),
                                 std::bind(exec_do_with_ref, _1, _2, _4,
                                           [](auto st, auto cont) {
                                             st->push_code();
                                             return st->jump(std::move(cont));
                                           },
                                           "JMPREFDATA"),
                                 compute_len_push_ref))
      .insert(OpcodeInstr::mksimple(0xdb3f, 16, "RETDATA", exec_ret_data))
      .insert(OpcodeInstr::mkfixed(0xdb4, 12, 12, dump_runvm, exec_runvm)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xdb50, 16, "RUNVMX ", exec_runvmx)->require_version(4));
}

int exec_if(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute IF\n";
  stack.check_underflow(2);
  auto cont = stack.pop_cont();
  if (stack.pop_bool()) {
    return st->call(std::move(cont));
  }
  return 0;
}

int exec_ifnot(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute IFNOT\n";
  stack.check_underflow(2);
  auto cont = stack.pop_cont();
  if (!stack.pop_bool()) {
    return st->call(std::move(cont));
  }
  return 0;
}

int exec_if_jmp(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute IFJMP\n";
  stack.check_underflow(2);
  auto cont = stack.pop_cont();
  if (stack.pop_bool()) {
    return st->jump(std::move(cont));
  }
  return 0;
}

int exec_ifnot_jmp(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute IFNOTJMP\n";
  stack.check_underflow(2);
  auto cont = stack.pop_cont();
  if (!stack.pop_bool()) {
    return st->jump(std::move(cont));
  }
  return 0;
}

int exec_ifret(VmState* st) {
  VM_LOG(st) << "execute IFRET\n";
  if (st->get_stack().pop_bool()) {
    return st->ret();
  }
  return 0;
}

int exec_ifnotret(VmState* st) {
  VM_LOG(st) << "execute IFNOTRET\n";
  if (!st->get_stack().pop_bool()) {
    return st->ret();
  }
  return 0;
}

int exec_if_else(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute IFELSE\n";
  stack.check_underflow(3);
  auto cont0 = stack.pop_cont();
  auto cont1 = stack.pop_cont();
  if (stack.pop_bool()) {
    std::swap(cont0, cont1);
  }
  cont1.clear();
  return st->call(std::move(cont0));
}

int exec_condsel(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute CONDSEL\n";
  stack.check_underflow(3);
  auto y = stack.pop();
  auto x = stack.pop();
  stack.push(stack.pop_bool() ? std::move(x) : std::move(y));
  return 0;
}

int exec_condsel_chk(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute CONDSELCHK\n";
  stack.check_underflow(3);
  auto y = stack.pop();
  auto x = stack.pop();
  if (x.type() != y.type()) {
    throw VmError{Excno::type_chk, "two arguments of CONDSELCHK have different type"};
  }
  stack.push(stack.pop_bool() ? std::move(x) : std::move(y));
  return 0;
}

int exec_ifretalt(VmState* st) {
  VM_LOG(st) << "execute IFRETALT\n";
  if (st->get_stack().pop_bool()) {
    return st->ret_alt();
  }
  return 0;
}

int exec_ifnotretalt(VmState* st) {
  VM_LOG(st) << "execute IFNOTRETALT\n";
  if (!st->get_stack().pop_bool()) {
    return st->ret_alt();
  }
  return 0;
}

int exec_if_bit_jmp(VmState* st, unsigned args) {
  bool negate = args & 0x20;
  unsigned bit = args & 0x1f;
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute IF" << (negate ? "NBITJMP " : "BITJMP ") << bit;
  stack.check_underflow(2);
  auto cont = stack.pop_cont();
  auto x = stack.pop_int_finite();
  bool val = x->get_bit(bit);
  stack.push_int(std::move(x));
  if (val ^ negate) {
    return st->jump(std::move(cont));
  }
  return 0;
}

std::string dump_if_bit_jmp(CellSlice& cs, unsigned args) {
  std::ostringstream os;
  os << "IF" << (args & 0x20 ? "N" : "") << "BITJMP " << (args & 0x1f);
  return os.str();
}

int exec_if_bit_jmpref(VmState* st, CellSlice& cs, unsigned args, int pfx_bits) {
  if (!cs.have_refs()) {
    throw VmError{Excno::inv_opcode, "no references left for a IFBITJMPREF instruction"};
  }
  cs.advance(pfx_bits);
  auto cell = cs.fetch_ref();
  bool negate = args & 0x20;
  unsigned bit = args & 0x1f;
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute IF" << (negate ? "N" : "") << "BITJMPREF " << bit << " (" << cell->get_hash().to_hex() << ")";
  auto x = stack.pop_int_finite();
  bool val = x->get_bit(bit);
  stack.push_int(std::move(x));
  if (val ^ negate) {
    return st->jump(st->ref_to_cont(std::move(cell)));
  }
  return 0;
}

std::string dump_if_bit_jmpref(CellSlice& cs, unsigned args, int pfx_bits) {
  if (!cs.have_refs()) {
    return "";
  }
  cs.advance(pfx_bits);
  cs.advance_refs(1);
  std::ostringstream os;
  os << "IF" << (args & 0x20 ? "N" : "") << "BITJMPREF " << (args & 0x1f);
  return os.str();
}

int exec_repeat(VmState* st, bool brk) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute REPEAT" << (brk ? "BRK" : "");
  stack.check_underflow(2);
  auto cont = stack.pop_cont();
  int c = stack.pop_smallint_range(0x7fffffff, 0x80000000);
  if (c <= 0) {
    return 0;
  }
  return st->repeat(std::move(cont), st->c1_envelope_if(brk, st->extract_cc(1)), c);
}

int exec_repeat_end(VmState* st, bool brk) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute REPEATEND" << (brk ? "BRK" : "");
  stack.check_underflow(1);
  int c = stack.pop_smallint_range(0x7fffffff, 0x80000000);
  if (c <= 0) {
    return st->ret();
  }
  auto cont = st->extract_cc(0);
  return st->repeat(std::move(cont), st->c1_envelope_if(brk, st->get_c0()), c);
}

int exec_until(VmState* st, bool brk) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute UNTIL" << (brk ? "BRK" : "");
  auto cont = stack.pop_cont();
  return st->until(std::move(cont), st->c1_envelope_if(brk, st->extract_cc(1)));
}

int exec_until_end(VmState* st, bool brk) {
  VM_LOG(st) << "execute UNTILEND" << (brk ? "BRK" : "");
  auto cont = st->extract_cc(0);
  return st->until(std::move(cont), st->c1_envelope_if(brk, st->get_c0()));
}

int exec_while(VmState* st, bool brk) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute WHILE" << (brk ? "BRK" : "");
  stack.check_underflow(2);
  auto body = stack.pop_cont();
  auto cond = stack.pop_cont();
  return st->loop_while(std::move(cond), std::move(body), st->c1_envelope_if(brk, st->extract_cc(1)));
}

int exec_while_end(VmState* st, bool brk) {
  VM_LOG(st) << "execute WHILEEND" << (brk ? "BRK" : "");
  auto cond = st->get_stack().pop_cont();
  auto body = st->extract_cc(0);
  return st->loop_while(std::move(cond), std::move(body), st->c1_envelope_if(brk, st->get_c0()));
}

int exec_again(VmState* st, bool brk) {
  VM_LOG(st) << "execute AGAIN" << (brk ? "BRK" : "");
  if (brk) {
    st->set_c1(st->extract_cc(3));
  }
  return st->again(st->get_stack().pop_cont());
}

int exec_again_end(VmState* st, bool brk) {
  VM_LOG(st) << "execute AGAINEND" << (brk ? "BRK" : "");
  if (brk) {
    st->c1_save_set();
  }
  return st->again(st->extract_cc(0));
}

void register_continuation_cond_loop_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xdc, 8, "IFRET", exec_ifret))
      .insert(OpcodeInstr::mksimple(0xdd, 8, "IFNOTRET", exec_ifnotret))
      .insert(OpcodeInstr::mksimple(0xde, 8, "IF", exec_if))
      .insert(OpcodeInstr::mksimple(0xdf, 8, "IFNOT", exec_ifnot))
      .insert(OpcodeInstr::mksimple(0xe0, 8, "IFJMP", exec_if_jmp))
      .insert(OpcodeInstr::mksimple(0xe1, 8, "IFNOTJMP", exec_ifnot_jmp))
      .insert(OpcodeInstr::mksimple(0xe2, 8, "IFELSE", exec_if_else))
      .insert(OpcodeInstr::mkext(0xe300, 16, 0, std::bind(dump_push_ref, _1, _2, _3, "IFREF"),
                                 std::bind(exec_do_with_cell, _1, _2, _4,
                                           [](auto st, auto cell) {
                                             return st->get_stack().pop_bool()
                                                        ? st->call(st->ref_to_cont(std::move(cell)))
                                                        : 0;
                                           },
                                           "IFREF"),
                                 compute_len_push_ref))
      .insert(OpcodeInstr::mkext(0xe301, 16, 0, std::bind(dump_push_ref, _1, _2, _3, "IFNOTREF"),
                                 std::bind(exec_do_with_cell, _1, _2, _4,
                                           [](auto st, auto cell) {
                                             return st->get_stack().pop_bool()
                                                        ? 0
                                                        : st->call(st->ref_to_cont(std::move(cell)));
                                           },
                                           "IFNOTREF"),
                                 compute_len_push_ref))
      .insert(OpcodeInstr::mkext(0xe302, 16, 0, std::bind(dump_push_ref, _1, _2, _3, "IFJMPREF"),
                                 std::bind(exec_do_with_cell, _1, _2, _4,
                                           [](auto st, auto cell) {
                                             return st->get_stack().pop_bool()
                                                        ? st->jump(st->ref_to_cont(std::move(cell)))
                                                        : 0;
                                           },
                                           "IFJMPREF"),
                                 compute_len_push_ref))
      .insert(OpcodeInstr::mkext(0xe303, 16, 0, std::bind(dump_push_ref, _1, _2, _3, "IFNOTJMPREF"),
                                 std::bind(exec_do_with_cell, _1, _2, _4,
                                           [](auto st, auto cell) {
                                             return st->get_stack().pop_bool()
                                                        ? 0
                                                        : st->jump(st->ref_to_cont(std::move(cell)));
                                           },
                                           "IFNOTJMPREF"),
                                 compute_len_push_ref))
      .insert(OpcodeInstr::mksimple(0xe304, 16, "CONDSEL", exec_condsel))
      .insert(OpcodeInstr::mksimple(0xe305, 16, "CONDSELCHK", exec_condsel_chk))
      .insert(OpcodeInstr::mksimple(0xe308, 16, "IFRETALT", exec_ifretalt))
      .insert(OpcodeInstr::mksimple(0xe309, 16, "IFNOTRETALT", exec_ifnotretalt))
      .insert(OpcodeInstr::mkext(0xe30d, 16, 0, std::bind(dump_push_ref, _1, _2, _3, "IFREFELSE"),
                                 std::bind(exec_ifelse_ref, _1, _2, _4, true), compute_len_push_ref))
      .insert(OpcodeInstr::mkext(0xe30e, 16, 0, std::bind(dump_push_ref, _1, _2, _3, "IFELSEREF"),
                                 std::bind(exec_ifelse_ref, _1, _2, _4, false), compute_len_push_ref))
      .insert(OpcodeInstr::mkext(0xe30f, 16, 0, std::bind(dump_push_ref2, _1, _2, _3, "IFREFELSEREF"),
                                 exec_ifref_elseref, compute_len_push_ref2))
      .insert(OpcodeInstr::mkfixed(0xe380 >> 6, 10, 6, dump_if_bit_jmp, exec_if_bit_jmp))
      .insert(OpcodeInstr::mkext(0xe3c0 >> 6, 10, 6, dump_if_bit_jmpref, exec_if_bit_jmpref, compute_len_push_ref))
      .insert(OpcodeInstr::mksimple(0xe4, 8, "REPEAT", std::bind(exec_repeat, _1, false)))
      .insert(OpcodeInstr::mksimple(0xe5, 8, "REPEATEND", std::bind(exec_repeat_end, _1, false)))
      .insert(OpcodeInstr::mksimple(0xe6, 8, "UNTIL", std::bind(exec_until, _1, false)))
      .insert(OpcodeInstr::mksimple(0xe7, 8, "UNTILEND", std::bind(exec_until_end, _1, false)))
      .insert(OpcodeInstr::mksimple(0xe8, 8, "WHILE", std::bind(exec_while, _1, false)))
      .insert(OpcodeInstr::mksimple(0xe9, 8, "WHILEEND", std::bind(exec_while_end, _1, false)))
      .insert(OpcodeInstr::mksimple(0xea, 8, "AGAIN", std::bind(exec_again, _1, false)))
      .insert(OpcodeInstr::mksimple(0xeb, 8, "AGAINEND", std::bind(exec_again_end, _1, false)))
      .insert(OpcodeInstr::mksimple(0xe314, 16, "REPEATBRK", std::bind(exec_repeat, _1, true)))
      .insert(OpcodeInstr::mksimple(0xe315, 16, "REPEATENDBRK", std::bind(exec_repeat_end, _1, true)))
      .insert(OpcodeInstr::mksimple(0xe316, 16, "UNTILBRK", std::bind(exec_until, _1, true)))
      .insert(OpcodeInstr::mksimple(0xe317, 16, "UNTILENDBRK", std::bind(exec_until_end, _1, true)))
      .insert(OpcodeInstr::mksimple(0xe318, 16, "WHILEBRK", std::bind(exec_while, _1, true)))
      .insert(OpcodeInstr::mksimple(0xe319, 16, "WHILEENDBRK", std::bind(exec_while_end, _1, true)))
      .insert(OpcodeInstr::mksimple(0xe31a, 16, "AGAINBRK", std::bind(exec_again, _1, true)))
      .insert(OpcodeInstr::mksimple(0xe31b, 16, "AGAINENDBRK", std::bind(exec_again_end, _1, true)));
}

int exec_setcontargs_common(VmState* st, int copy, int more) {
  Stack& stack = st->get_stack();
  stack.check_underflow(copy + 1);
  auto cont = stack.pop_cont();
  if (copy || more >= 0) {
    ControlData* cdata = force_cdata(cont);
    if (copy > 0) {
      if (cdata->nargs >= 0 && cdata->nargs < copy) {
        throw VmError{Excno::stk_ov, "too many arguments copied into a closure continuation"};
      }
      if (cdata->stack.is_null()) {
        cdata->stack = stack.split_top(copy);
      } else {
        cdata->stack.write().move_from_stack(stack, copy);
      }
      st->consume_stack_gas(cdata->stack);
      if (cdata->nargs >= 0) {
        cdata->nargs -= copy;
      }
    }
    if (more >= 0) {
      if (cdata->nargs > more) {
        cdata->nargs = 0x40000000;  // will throw an exception if run
      } else if (cdata->nargs < 0) {
        cdata->nargs = more;
      }
    }
  }
  st->get_stack().push_cont(std::move(cont));
  return 0;
}

int exec_setcontargs(VmState* st, unsigned args) {
  int copy = (args >> 4) & 15, more = ((args + 1) & 15) - 1;
  VM_LOG(st) << "execute SETCONTARGS " << copy << ',' << more;
  return exec_setcontargs_common(st, copy, more);
}

std::string dump_setcontargs(CellSlice& cs, unsigned args, const char* name) {
  int copy = (args >> 4) & 15, more = ((args + 1) & 15) - 1;
  std::ostringstream os;
  os << name << ' ' << copy << ',' << more;
  return os.str();
}

int exec_setcont_varargs(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute SETCONTVARARGS\n";
  stack.check_underflow(2);
  int more = stack.pop_smallint_range(255, -1);
  int copy = stack.pop_smallint_range(255);
  return exec_setcontargs_common(st, copy, more);
}

int exec_setnum_varargs(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute SETNUMVARARGS\n";
  stack.check_underflow(2);
  int more = stack.pop_smallint_range(255, -1);
  return exec_setcontargs_common(st, 0, more);
}

int exec_return_args_common(VmState* st, int count) {
  Stack& stack = st->get_stack();
  stack.check_underflow(count);
  if (stack.depth() == count) {
    return 0;
  }
  int copy = stack.depth() - count;
  Ref<Stack> alt_stk = st->swap_stack(stack.split_top(count));
  auto cont = st->get_c0();
  ControlData* cdata = force_cdata(cont);
  if (cdata->nargs >= 0 && cdata->nargs < copy) {
    throw VmError{Excno::stk_ov, "too many arguments copied into a closure continuation"};
  }
  if (cdata->stack.is_null()) {
    cdata->stack = std::move(alt_stk);
  } else {
    cdata->stack.write().move_from_stack(alt_stk.write(), copy);
    alt_stk.clear();
  }
  st->consume_stack_gas(cdata->stack);
  if (cdata->nargs >= 0) {
    cdata->nargs -= copy;
  }
  st->set_c0(std::move(cont));
  return 0;
}

int exec_return_args(VmState* st, unsigned args) {
  int count = args & 15;
  VM_LOG(st) << "execute RETURNARGS " << count;
  return exec_return_args_common(st, count);
}

int exec_return_varargs(VmState* st) {
  VM_LOG(st) << "execute RETURNVARARGS\n";
  return exec_return_args_common(st, st->get_stack().pop_smallint_range(255));
}

int exec_bless(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute BLESS\n";
  stack.push_cont(Ref<OrdCont>{true, stack.pop_cellslice(), st->get_cp()});
  return 0;
}

int exec_bless_args_common(VmState* st, int copy, int more) {
  Stack& stack = st->get_stack();
  stack.check_underflow(copy + 1);
  auto cs = stack.pop_cellslice();
  auto new_stk = stack.split_top(copy);
  st->consume_stack_gas(new_stk);
  stack.push_cont(Ref<OrdCont>{true, std::move(cs), st->get_cp(), std::move(new_stk), more});
  return 0;
}

int exec_bless_varargs(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute BLESSVARARGS\n";
  stack.check_underflow(2);
  int more = stack.pop_smallint_range(255, -1);
  int copy = stack.pop_smallint_range(255);
  return exec_bless_args_common(st, copy, more);
}

int exec_bless_args(VmState* st, unsigned args) {
  int copy = (args >> 4) & 15, more = ((args + 1) & 15) - 1;
  VM_LOG(st) << "execute BLESSARGS " << copy << ',' << more;
  return exec_bless_args_common(st, copy, more);
}

int exec_push_ctr(VmState* st, unsigned args) {
  unsigned idx = args & 15;
  VM_LOG(st) << "execute PUSH c" << idx;
  st->get_stack().push(st->get(idx));
  return 0;
}

namespace {
inline void throw_typechk(bool ok) {
  if (!ok) {
    throw VmError{Excno::type_chk, "invalid value type for control register"};
  }
}

inline void throw_rangechk(bool ok) {
  if (!ok) {
    throw VmError{Excno::range_chk, "control register index out of range"};
  }
}
}  // namespace

int exec_bless_pop_c3(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute CTOSBLESSPOPc3";
  stack.check_underflow(1);
  throw_typechk(st->set_c(3, Ref<OrdCont>{true, vm::load_cell_slice_ref(stack.pop_cell()), st->get_cp()}));
  return 0;
}

int exec_pop_ctr(VmState* st, unsigned args) {
  unsigned idx = args & 15;
  VM_LOG(st) << "execute POP c" << idx;
  /*
  if (idx == 3 && st->get_stack().depth() > 0 && st->get_stack().tos().is(StackEntry::t_cell)) {
    // temp hack: accept cell argument for POP c3 and do auto-BLESSing
    return exec_bless_pop_c3(st);
  }
  */
  throw_typechk(st->set(idx, st->get_stack().pop_chk()));
  return 0;
}

int exec_setcont_ctr(VmState* st, unsigned args) {
  unsigned idx = args & 15;
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute SETCONTCTR c" << idx;
  stack.check_underflow(2);
  auto cont = stack.pop_cont();
  throw_typechk(force_cregs(cont)->define(idx, stack.pop_chk()));
  st->get_stack().push_cont(std::move(cont));
  return 0;
}

int exec_setret_ctr(VmState* st, unsigned args) {
  unsigned idx = args & 15;
  VM_LOG(st) << "execute SETRETCTR c" << idx;
  auto cont = st->get_c0();
  throw_typechk(force_cregs(cont)->define(idx, st->get_stack().pop_chk()));
  st->set_c0(std::move(cont));
  return 0;
}

int exec_setalt_ctr(VmState* st, unsigned args) {
  unsigned idx = args & 15;
  VM_LOG(st) << "execute SETALTCTR c" << idx;
  auto cont = st->get_c1();
  throw_typechk(force_cregs(cont)->define(idx, st->get_stack().pop_chk()));
  st->set_c1(std::move(cont));
  return 0;
}

int exec_popsave_ctr(VmState* st, unsigned args) {
  unsigned idx = args & 15;
  VM_LOG(st) << "execute POPSAVE c" << idx;
  auto val = st->get_stack().pop_chk();
  auto c0 = st->get_c0();
  throw_typechk(idx || val.is(StackEntry::t_vmcont));
  force_cregs(c0)->define(idx, st->get(idx));
  if (!idx) {
    st->set_c0(std::move(c0));
    throw_typechk(st->set(idx, std::move(val)));
  } else {
    throw_typechk(st->set(idx, std::move(val)));
    st->set_c0(std::move(c0));
  }
  return 0;
}

int exec_save_ctr(VmState* st, unsigned args) {
  unsigned idx = args & 15;
  VM_LOG(st) << "execute SAVECTR c" << idx;
  auto c0 = st->get_c0();
  throw_typechk(force_cregs(c0)->define(idx, st->get(idx)));
  st->set_c0(std::move(c0));
  return 0;
}

int exec_samealt(VmState* st, bool save) {
  VM_LOG(st) << "execute SAMEALT" << (save ? "SAVE" : "");
  auto c0 = st->get_c0();
  if (save) {
    force_cregs(c0)->define_c1(st->get_c1());
    st->set_c0(c0);
  }
  st->set_c1(std::move(c0));
  return 0;
}

int exec_savealt_ctr(VmState* st, unsigned args) {
  unsigned idx = args & 15;
  VM_LOG(st) << "execute SAVEALTCTR c" << idx;
  auto c1 = st->get_c1();
  throw_typechk(force_cregs(c1)->define(idx, st->get(idx)));
  st->set_c1(std::move(c1));
  return 0;
}

int exec_saveboth_ctr(VmState* st, unsigned args) {
  unsigned idx = args & 15;
  VM_LOG(st) << "execute SAVEBOTHCTR c" << idx;
  auto c0 = st->get_c0(), c1 = st->get_c1();
  auto val = st->get(idx);
  force_cregs(c0)->define(idx, val);
  force_cregs(c1)->define(idx, std::move(val));
  st->set_c0(std::move(c0));
  st->set_c1(std::move(c1));
  return 0;
}

int exec_push_ctr_var(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute PUSHCTRX\n";
  unsigned idx = stack.pop_smallint_range(16);
  auto val = st->get(idx);
  throw_rangechk(!val.empty());
  stack.push(std::move(val));
  return 0;
}

int exec_pop_ctr_var(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute POPCTRX\n";
  stack.check_underflow(2);
  unsigned idx = stack.pop_smallint_range(16);
  throw_rangechk(ControlRegs::valid_idx(idx));
  throw_typechk(st->set(idx, stack.pop_chk()));
  return 0;
}

int exec_setcont_ctr_var(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute SETCONTCTRX\n";
  stack.check_underflow(3);
  unsigned idx = stack.pop_smallint_range(16);
  throw_rangechk(ControlRegs::valid_idx(idx));
  auto cont = stack.pop_cont();
  throw_typechk(force_cregs(cont)->define(idx, stack.pop_chk()));
  st->get_stack().push_cont(std::move(cont));
  return 0;
}

int exec_setcont_ctr_many(VmState* st, unsigned args) {
  unsigned mask = args & 255;
  VM_LOG(st) << "execute SETCONTCTRMANY " << mask;
  if (mask & (1 << 6)) {
    throw VmError{Excno::range_chk, "no control register c6"};
  }
  Stack& stack = st->get_stack();
  auto cont = stack.pop_cont();
  for (int i = 0; i < 8; ++i) {
    if (mask & (1 << i)) {
      throw_typechk(force_cregs(cont)->define(i, st->get(i)));
    }
  }
  st->get_stack().push_cont(std::move(cont));
  return 0;
}

int exec_setcont_ctr_many_var(VmState* st) {
  VM_LOG(st) << "execute SETCONTCTRMANYX";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  int mask = stack.pop_smallint_range(255);
  if (mask & (1 << 6)) {
    throw VmError{Excno::range_chk, "no control register c6"};
  }
  auto cont = stack.pop_cont();
  for (int i = 0; i < 8; ++i) {
    if (mask & (1 << i)) {
      throw_typechk(force_cregs(cont)->define(i, st->get(i)));
    }
  }
  st->get_stack().push_cont(std::move(cont));
  return 0;
}

int exec_compos(VmState* st, unsigned mask, const char* name) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute " << name;
  stack.check_underflow(2);
  auto val = stack.pop_cont();
  auto cont = stack.pop_cont();
  ControlRegs* regs = force_cregs(cont);
  switch (mask) {
    case 1:
      regs->define_c0(std::move(val));
      break;
    case 3:
      regs->define_c0(val);
      // fallthrough
    case 2:
      regs->define_c1(std::move(val));
      break;
  }
  st->get_stack().push_cont(std::move(cont));
  return 0;
}

int exec_atexit(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute ATEXIT\n";
  auto cont = stack.pop_cont();
  force_cregs(cont)->define_c0(st->get_c0());
  st->set_c0(std::move(cont));
  return 0;
}

int exec_atexit_alt(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute ATEXITALT\n";
  auto cont = stack.pop_cont();
  force_cregs(cont)->define_c1(st->get_c1());
  st->set_c1(std::move(cont));
  return 0;
}

int exec_setexit_alt(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute SETEXITALT\n";
  auto cont = stack.pop_cont();
  force_cregs(cont)->define_c0(st->get_c0());
  force_cregs(cont)->define_c1(st->get_c1());
  st->set_c1(std::move(cont));
  return 0;
}

int exec_thenret(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute THENRET\n";
  auto cont = stack.pop_cont();
  force_cregs(cont)->define_c0(st->get_c0());
  stack.push_cont(std::move(cont));
  return 0;
}

int exec_thenret_alt(VmState* st) {
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute THENRETALT\n";
  auto cont = stack.pop_cont();
  force_cregs(cont)->define_c0(st->get_c1());
  stack.push_cont(std::move(cont));
  return 0;
}

int exec_invert(VmState* st) {
  VM_LOG(st) << "execute INVERT\n";
  auto c0 = st->get_c0(), c1 = st->get_c1();
  st->set_c0(std::move(c1));
  st->set_c1(std::move(c0));
  return 0;
}

int exec_booleval(VmState* st) {
  VM_LOG(st) << "execute BOOLEVAL\n";
  auto cont = st->get_stack().pop_cont();
  auto cc = st->extract_cc(3);
  st->set_c0(Ref<PushIntCont>{true, -1, cc});
  st->set_c1(Ref<PushIntCont>{true, 0, std::move(cc)});
  return st->jump(std::move(cont));
}

void reg_ctr_oprange(OpcodeTable& cp, unsigned opcode, std::string name, exec_arg_instr_func_t exec_ctr) {
  cp.insert(OpcodeInstr::mkfixedrange(opcode, opcode + 4, 16, 4, instr::dump_1c(name + " c"), exec_ctr))
      .insert(OpcodeInstr::mkfixedrange(opcode + 4, opcode + 6, 16, 4, instr::dump_1c(name + " c"), exec_ctr))
      .insert(OpcodeInstr::mkfixedrange(opcode + 7, opcode + 8, 16, 4, instr::dump_1c(name + " c"), exec_ctr));
}

void register_continuation_change_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mkfixed(0xec, 8, 8, std::bind(dump_setcontargs, _1, _2, "SETCONTARGS"), exec_setcontargs))
      .insert(OpcodeInstr::mkfixed(0xed0, 12, 4, instr::dump_1c("RETURNARGS "), exec_return_args))
      .insert(OpcodeInstr::mksimple(0xed10, 16, "RETURNVARARGS", exec_return_varargs))
      .insert(OpcodeInstr::mksimple(0xed11, 16, "SETCONTVARARGS", exec_setcont_varargs))
      .insert(OpcodeInstr::mksimple(0xed12, 16, "SETNUMVARARGS", exec_setnum_varargs))
      .insert(OpcodeInstr::mksimple(0xed1e, 16, "BLESS", exec_bless))
      .insert(OpcodeInstr::mksimple(0xed1f, 16, "BLESSVARARGS", exec_bless_varargs));

  reg_ctr_oprange(cp0, 0xed40, "PUSH", exec_push_ctr);
  reg_ctr_oprange(cp0, 0xed50, "POP", exec_pop_ctr);
  reg_ctr_oprange(cp0, 0xed60, "SETCONTCTR", exec_setcont_ctr);
  reg_ctr_oprange(cp0, 0xed70, "SETRETCTR", exec_setret_ctr);
  reg_ctr_oprange(cp0, 0xed80, "SETALTCTR", exec_setalt_ctr);
  reg_ctr_oprange(cp0, 0xed90, "POPSAVE", exec_popsave_ctr);
  reg_ctr_oprange(cp0, 0xeda0, "SAVECTR", exec_save_ctr);
  reg_ctr_oprange(cp0, 0xedb0, "SAVEALTCTR", exec_savealt_ctr);
  reg_ctr_oprange(cp0, 0xedc0, "SAVEBOTHCTR", exec_saveboth_ctr);

  cp0.insert(OpcodeInstr::mksimple(0xede0, 16, "PUSHCTRX", exec_push_ctr_var))
      .insert(OpcodeInstr::mksimple(0xede1, 16, "POPCTRX", exec_pop_ctr_var))
      .insert(OpcodeInstr::mksimple(0xede2, 16, "SETCONTCTRX", exec_setcont_ctr_var))
      .insert(OpcodeInstr::mkfixed(0xede3, 16, 8, instr::dump_1c_l_add(1, "SETCONTCTRMANY "), exec_setcont_ctr_many)->require_version(9))
      .insert(OpcodeInstr::mksimple(0xede4, 16, "SETCONTCTRMANYX", exec_setcont_ctr_many_var)->require_version(9))
      .insert(OpcodeInstr::mksimple(0xedf0, 16, "BOOLAND", std::bind(exec_compos, _1, 1, "BOOLAND")))
      .insert(OpcodeInstr::mksimple(0xedf1, 16, "BOOLOR", std::bind(exec_compos, _1, 2, "BOOLOR")))
      .insert(OpcodeInstr::mksimple(0xedf2, 16, "COMPOSBOTH", std::bind(exec_compos, _1, 3, "COMPOSBOTH")))
      .insert(OpcodeInstr::mksimple(0xedf3, 16, "ATEXIT", exec_atexit))
      .insert(OpcodeInstr::mksimple(0xedf4, 16, "ATEXITALT", exec_atexit_alt))
      .insert(OpcodeInstr::mksimple(0xedf5, 16, "SETEXITALT", exec_setexit_alt))
      .insert(OpcodeInstr::mksimple(0xedf6, 16, "THENRET", exec_thenret))
      .insert(OpcodeInstr::mksimple(0xedf7, 16, "THENRETALT", exec_thenret_alt))
      .insert(OpcodeInstr::mksimple(0xedf8, 16, "INVERT", exec_invert))
      .insert(OpcodeInstr::mksimple(0xedf9, 16, "BOOLEVAL", exec_booleval))
      .insert(OpcodeInstr::mksimple(0xedfa, 16, "SAMEALT", std::bind(exec_samealt, _1, false)))
      .insert(OpcodeInstr::mksimple(0xedfb, 16, "SAMEALTSAVE", std::bind(exec_samealt, _1, true)))
      .insert(OpcodeInstr::mkfixed(0xee, 8, 8, std::bind(dump_setcontargs, _1, _2, "BLESSARGS"), exec_bless_args));
}

int exec_calldict_short(VmState* st, unsigned args) {
  args &= 0xff;
  VM_LOG(st) << "execute CALLDICT " << args;
  st->get_stack().push_smallint(args);
  return st->call(st->get_c3());
}

int exec_calldict(VmState* st, unsigned args) {
  args &= 0x3fff;
  VM_LOG(st) << "execute CALLDICT " << args;
  st->get_stack().push_smallint(args);
  return st->call(st->get_c3());
}

int exec_jmpdict(VmState* st, unsigned args) {
  args &= 0x3fff;
  VM_LOG(st) << "execute JMPDICT " << args;
  st->get_stack().push_smallint(args);
  return st->jump(st->get_c3());
}

int exec_preparedict(VmState* st, unsigned args) {
  args &= 0x3fff;
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute PREPAREDICT " << args;
  stack.push_smallint(args);
  stack.push_cont(st->get_c3());
  return 0;
}

void register_continuation_dict_jump_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mkfixed(0xf0, 8, 8, instr::dump_1c_and(255, "CALLDICT "), exec_calldict_short))
      .insert(OpcodeInstr::mkfixed(0xf100 >> 6, 10, 14, instr::dump_1c_and(0x3fff, "CALLDICT "), exec_calldict))
      .insert(OpcodeInstr::mkfixed(0xf140 >> 6, 10, 14, instr::dump_1c_and(0x3fff, "JMPDICT "), exec_jmpdict))
      .insert(OpcodeInstr::mkfixed(0xf180 >> 6, 10, 14, instr::dump_1c_and(0x3fff, "PREPAREDICT "), exec_preparedict));
}

int exec_throw_fixed(VmState* st, unsigned args, unsigned mask, int mode) {
  unsigned excno = args & mask;
  VM_LOG(st) << "execute THROW" << (mode ? "IF" : "") << (mode == 2 ? "NOT " : " ") << excno;
  if (mode && st->get_stack().pop_bool() != (bool)(mode & 1)) {
    return 0;
  } else {
    return st->throw_exception(excno);
  }
}

int exec_throw_arg_fixed(VmState* st, unsigned args, unsigned mask, int mode) {
  unsigned excno = args & mask;
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute THROWARG" << (mode ? "IF" : "") << (mode == 2 ? "NOT " : " ") << excno;
  stack.check_underflow(mode ? 2 : 1);
  if (mode && stack.pop_bool() != (bool)(mode & 1)) {
    stack.pop();
    return 0;
  } else {
    return st->throw_exception(excno, stack.pop());
  }
}

int exec_throw_any(VmState* st, unsigned args) {
  bool has_param = args & 1;
  bool has_cond = args & 6;
  bool throw_cond = args & 2;
  Stack& stack = st->get_stack();
  VM_LOG(st) << "execute THROW" << (has_param ? "ARG" : "") << "ANY" << (has_cond ? (throw_cond ? "IF" : "IFNOT") : "");
  stack.check_underflow(1 + (int)has_cond + (int)has_param);
  bool flag = has_cond ? stack.pop_bool() : throw_cond;
  int excno = stack.pop_smallint_range(0xffff);
  if (flag != throw_cond) {
    if (has_param) {
      stack.pop();
    }
    return 0;
  } else if (has_param) {
    return st->throw_exception(excno, stack.pop());
  } else {
    return st->throw_exception(excno);
  }
}

std::string dump_throw_any(CellSlice& cs, unsigned args) {
  bool has_param = args & 1;
  bool has_cond = args & 6;
  bool throw_cond = args & 2;
  std::ostringstream os;
  os << "THROW" << (has_param ? "ARG" : "") << "ANY";
  if (has_cond) {
    os << (throw_cond ? "IF" : "IFNOT");
  }
  return os.str();
}

int exec_try(VmState* st, int args) {
  int params = (args >> 4) & 15, retvals = args & 15;
  Stack& stack = st->get_stack();
  if (args < 0) {
    VM_LOG(st) << "execute TRY";
  } else {
    VM_LOG(st) << "execute TRYARGS " << params << "," << retvals;
  }
  stack.check_underflow(args >= 0 ? params + 2 : 2);
  auto handler_cont = stack.pop_cont();
  auto cont = stack.pop_cont();
  auto old_c2 = st->get_c2();
  auto cc = (args >= 0 ? st->extract_cc(7, params, retvals) : st->extract_cc(7));
  ControlRegs* handler_cr = force_cregs(handler_cont);
  handler_cr->define_c2(std::move(old_c2));
  handler_cr->define_c0(cc);
  st->set_c0(std::move(cc));
  st->set_c2(std::move(handler_cont));
  return st->jump(std::move(cont));
}

void register_exception_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mkfixed(0xf200 >> 6, 10, 6, instr::dump_1c_and(0x3f, "THROW "),
                                  std::bind(exec_throw_fixed, _1, _2, 63, 0)))
      .insert(OpcodeInstr::mkfixed(0xf240 >> 6, 10, 6, instr::dump_1c_and(0x3f, "THROWIF "),
                                   std::bind(exec_throw_fixed, _1, _2, 63, 3)))
      .insert(OpcodeInstr::mkfixed(0xf280 >> 6, 10, 6, instr::dump_1c_and(0x3f, "THROWIFNOT "),
                                   std::bind(exec_throw_fixed, _1, _2, 63, 2)))
      .insert(OpcodeInstr::mkfixed(0xf2c0 >> 3, 13, 11, instr::dump_1c_and(0x7ff, "THROW "),
                                   std::bind(exec_throw_fixed, _1, _2, 0x7ff, 0)))
      .insert(OpcodeInstr::mkfixed(0xf2c8 >> 3, 13, 11, instr::dump_1c_and(0x7ff, "THROWARG "),
                                   std::bind(exec_throw_arg_fixed, _1, _2, 0x7ff, 0)))
      .insert(OpcodeInstr::mkfixed(0xf2d0 >> 3, 13, 11, instr::dump_1c_and(0x7ff, "THROWIF "),
                                   std::bind(exec_throw_fixed, _1, _2, 0x7ff, 3)))
      .insert(OpcodeInstr::mkfixed(0xf2d8 >> 3, 13, 11, instr::dump_1c_and(0x7ff, "THROWARGIF "),
                                   std::bind(exec_throw_arg_fixed, _1, _2, 0x7ff, 3)))
      .insert(OpcodeInstr::mkfixed(0xf2e0 >> 3, 13, 11, instr::dump_1c_and(0x7ff, "THROWIFNOT "),
                                   std::bind(exec_throw_fixed, _1, _2, 0x7ff, 2)))
      .insert(OpcodeInstr::mkfixed(0xf2e8 >> 3, 13, 11, instr::dump_1c_and(0x7ff, "THROWARGIFNOT "),
                                   std::bind(exec_throw_arg_fixed, _1, _2, 0x7ff, 2)))
      .insert(OpcodeInstr::mkfixedrange(0xf2f0, 0xf2f6, 16, 3, dump_throw_any, exec_throw_any))
      .insert(OpcodeInstr::mksimple(0xf2ff, 16, "TRY", std::bind(exec_try, _1, -1)))
      .insert(OpcodeInstr::mkfixed(0xf3, 8, 8, instr::dump_2c("TRYARGS ", ","), exec_try));
}

int exec_set_cp_generic(VmState* st, int new_codepage) {
  st->force_cp(new_codepage);
  return 0;
}

int exec_set_cp(VmState* st, int args) {
  int cp = ((args + 0x10) & 0xff) - 0x10;
  VM_LOG(st) << "execute SETCP " << cp;
  return exec_set_cp_generic(st, cp);
}

int exec_set_cp_any(VmState* st) {
  VM_LOG(st) << "execute SETCPX";
  int cp = st->get_stack().pop_smallint_range(0x7fff, -0x8000);
  return exec_set_cp_generic(st, cp);
}

void register_codepage_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mkfixedrange(0xff00, 0xfff0, 16, 8, instr::dump_1c_and(0xff, "SETCP "), exec_set_cp))
      .insert(OpcodeInstr::mkfixedrange(0xfff1, 0x10000, 16, 8, instr::dump_1c_l_add(-256, "SETCP "), exec_set_cp))
      .insert(OpcodeInstr::mksimple(0xfff0, 16, "SETCPX", exec_set_cp_any));
}

void register_continuation_ops(OpcodeTable& cp0) {
  register_continuation_jump_ops(cp0);
  register_continuation_cond_loop_ops(cp0);
  register_continuation_change_ops(cp0);
  register_continuation_dict_jump_ops(cp0);
  register_exception_ops(cp0);
}

}  // namespace vm
