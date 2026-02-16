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
#include <algorithm>
#include <functional>
#include "vm/debugops.h"

#include "boc.h"
#include "td/utils/base64.h"
#include "vm/log.h"
#include "vm/opctable.h"
#include "vm/stack.hpp"
#include "vm/excno.hpp"
#include "vm/vm.h"

namespace vm {

bool vm_debug_enabled = false;

void set_debug_enabled(bool enable_debug) {
  vm_debug_enabled = enable_debug;
}

int exec_dummy_debug(VmState* st, int args) {
  VM_LOG(st) << "execute DEBUG " << (args & 0xff);
  return 0;
}

std::string dump_extcall(CellSlice& cs, unsigned _, int pfx_bits) {
  unsigned bits = pfx_bits + 32;
  if (!cs.have(bits)) {
    return "";
  }
  cs.advance(pfx_bits);
  const auto arg = cs.fetch_ulong(32);
  std::ostringstream os;
  os << "EXTCALL " << arg;
  return os.str();
}

int compute_len_extcall(const CellSlice& cs, unsigned _, int pfx_bits) {
  unsigned bits = pfx_bits + 32;
  return cs.have(bits) ? bits : 0;
}

int exec_extcall(VmState* st, CellSlice& cs, unsigned _, [[maybe_unused]] int pfx_bits) {
  if (!cs.have(pfx_bits + 32)) {
    throw VmError{Excno::inv_opcode, "not enough data bits for a EXTCALL instruction"};
  }

  cs.advance(pfx_bits);
  const auto arg = cs.fetch_ulong(32);
  VM_LOG(st) << "execute EXTCALL " << arg;

  if (!st->ext_methods.contains(arg)) {
    VM_LOG(st) << "skip unknown external method with id: " << arg;
  } else {
    const auto& method = st->ext_methods.at(arg);
    Stack& current_stack = st->get_stack();
    const int original_depth = current_stack.depth();
    const int requested_items = method.stack_items_count == 255
                                    ? original_depth
                                    : std::min<int>(method.stack_items_count, original_depth);

    Stack callback_stack;
    for (int i = requested_items - 1; i >= 0; --i) {
      callback_stack.push(current_stack[i]);
    }

    // Serialize selected stack segment to base64 string
    auto builder = CellBuilder{};
    callback_stack.serialize(builder);
    const auto buf = std_boc_serialize(builder.as_cellslice().get_base_cell()).move_as_ok();
    const auto stack = td::base64_encode(buf);

    const char* exec_res = nullptr;

    try {
      exec_res = method.callback(method.ctx, stack.c_str());
    } catch (const std::exception& e) {
      VM_LOG(st) << "cannot execute method with id " << arg << ": " << e.what();
      throw VmError{Excno::fatal};
    }

    // Deserialize result stack from base64
    const td::Slice res_slice(exec_res);
    const auto decoded_res = td::base64_decode(res_slice).move_as_ok();
    const auto cell_res = std_boc_deserialize(decoded_res).move_as_ok();

    // The result stack is always inherited from the initial one,
    // so we can simply set it to replace the previous one
    auto new_stack = Stack{};
    CellSlice cs{NoVm{}, cell_res};
    new_stack.deserialize(cs);

    if (requested_items == original_depth) {
      current_stack.set_contents(std::move(new_stack));
    } else {
      Stack merged_stack;
      for (int i = original_depth - 1; i >= requested_items; --i) {
        merged_stack.push(current_stack[i]);
      }
      for (int i = new_stack.depth() - 1; i >= 0; --i) {
        merged_stack.push(new_stack[i]);
      }
      current_stack.set_contents(std::move(merged_stack));
    }
  }

  return 0;
}

// similar to PUSHSLICE instruction in cellops.cpp
int exec_dummy_debug_str(VmState* st, CellSlice& cs, unsigned args, int pfx_bits) {
  int data_bits = ((args & 15) + 1) * 8;
  if (!cs.have(pfx_bits + data_bits)) {
    throw VmError{Excno::inv_opcode, "not enough data bits for a DEBUGSTR instruction"};
  }
  cs.advance(pfx_bits);
  auto slice = cs.fetch_subslice(data_bits);
  VM_LOG(st) << "execute DEBUGSTR " << slice->as_bitslice().to_hex();
  return 0;
}

std::string dump_dummy_debug_str(CellSlice& cs, unsigned args, int pfx_bits) {
  int data_bits = ((args & 15) + 1) * 8;
  if (!cs.have(pfx_bits + data_bits)) {
    return "";
  }
  cs.advance(pfx_bits);
  auto slice = cs.fetch_subslice(data_bits);
  slice.unique_write().remove_trailing();
  std::ostringstream os;
  os << "DEBUGSTR ";
  slice->dump_hex(os, 1, false);
  return os.str();
}

int compute_len_debug_str(const CellSlice& cs, unsigned args, int pfx_bits) {
  unsigned bits = pfx_bits + ((args & 15) + 1) * 8;
  return cs.have(bits) ? bits : 0;
}

int exec_dump_stack(VmState* st) {
  VM_LOG(st) << "execute DUMPSTK";
  if (!vm_debug_enabled) {
    return 0;
  }
  Stack& stack = st->get_stack();
  int d = stack.depth();

  std::ostringstream os;
  os << "#DEBUG#: stack(" << d << " values) : ";
  if (d > 255) {
    os << "... ";
    d = 255;
  }
  for (int i = d; i > 0; i--) {
    stack[i - 1].print_list(os);
    os << ' ';
  }

  VM_LOG(st) << os.str();

  return 0;
}

int exec_dump_value(VmState* st, unsigned arg) {
  arg &= 15;
  VM_LOG(st) << "execute DUMP s" << arg;
  if (!vm_debug_enabled) {
    return 0;
  }
  Stack& stack = st->get_stack();
  if ((int)arg < stack.depth()) {
    std::ostringstream os;
    os << "#DEBUG#: s" << arg << " = ";
    stack[arg].print_list(os);

    VM_LOG(st) << os.str();
  } else {
    VM_LOG(st) << "#DEBUG#: s" << arg << " is absent";
  }
  return 0;
}

int exec_dump_string(VmState* st) {
  VM_LOG(st) << "execute STRDUMP";
  if (!vm_debug_enabled) {
    return 0;
  }

  Stack& stack = st->get_stack();

  if (stack.depth() > 0) {
    auto cs = stack[0].as_slice();

    if (cs.not_null()) {  // wanted t_slice
      auto size = cs->size();

      if (size % 8 == 0) {
        auto cnt = size / 8;

        unsigned char tmp[128];
        cs.write().fetch_bytes(tmp, cnt);
        std::string s{tmp, tmp + cnt};

        VM_LOG(st) << "#DEBUG#: " << s;
      } else {
        VM_LOG(st) << "#DEBUG#: slice contains not valid bits count";
      }

    } else {
      VM_LOG(st) << "#DEBUG#: is not a slice";
    }
  } else {
    VM_LOG(st) << "#DEBUG#: s0 is absent";
  }

  return 0;
}

void register_debug_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mkext(0xFC00, 16, 0, dump_extcall, exec_extcall, compute_len_extcall));
  if (!vm_debug_enabled) {
    cp0.insert(OpcodeInstr::mkfixedrange(0xfe00, 0xfef0, 16, 8, instr::dump_1c_and(0xff, "DEBUG "), exec_dummy_debug))
        .insert(OpcodeInstr::mkext(0xfef, 12, 4, dump_dummy_debug_str, exec_dummy_debug_str, compute_len_debug_str));
  } else {
    // NB: all non-redefined opcodes in fe00..feff should be redirected to dummy debug definitions
    cp0.insert(OpcodeInstr::mksimple(0xfe00, 16, "DUMPSTK", exec_dump_stack))
        .insert(OpcodeInstr::mkfixedrange(0xfe01, 0xfe14, 16, 8, instr::dump_1c_and(0xff, "DEBUG "), exec_dummy_debug))
        .insert(OpcodeInstr::mksimple(0xfe14, 16,"STRDUMP", exec_dump_string))
        .insert(OpcodeInstr::mkfixedrange(0xfe15, 0xfe20, 16, 8, instr::dump_1c_and(0xff, "DEBUG "), exec_dummy_debug))
        .insert(OpcodeInstr::mkfixed(0xfe2, 12, 4, instr::dump_1sr("DUMP"), exec_dump_value))
        .insert(OpcodeInstr::mkfixedrange(0xfe30, 0xfef0, 16, 8, instr::dump_1c_and(0xff, "DEBUG "), exec_dummy_debug))
        .insert(OpcodeInstr::mkext(0xfef, 12, 4, dump_dummy_debug_str, exec_dummy_debug_str, compute_len_debug_str));
  }
}

}  // namespace vm
