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
#include "vm/debugops.h"
#include "vm/log.h"
#include "vm/opctable.h"
#include "vm/stack.hpp"
#include "vm/excno.hpp"
#include "vm/vm.h"
#include "crypto/block/block-auto.h"

namespace vm {

bool vm_debug_enabled = false;

void set_debug_enabled(bool enable_debug) {
  vm_debug_enabled = enable_debug;
}

int exec_dummy_debug(VmState* st, int args) {
  VM_LOG(st) << "execute DEBUG " << (args & 0xff);
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

int dump_tlb_s0(Stack& stack, const std::string& tlb_type) {
  if (stack.depth() > 0) {
    vm::CellSlice to_dump;
    auto element = stack[0].as_cell();

    if (element.not_null()) {
      tlb::TypenameLookup tlb_dict0;
      tlb_dict0.register_types(block::gen::register_simple_types);

      auto _template = tlb_dict0.lookup(tlb_type);
      if (!_template) {
        std::cerr << "#DEBUG#: TL-B type not supported" << std::endl;
        return 0;
      }

      std::stringstream ss;
      _template->print_ref(9 << 20, ss, element);

      std::cerr << "#DEBUG#: " << ss.str() << std::endl;
    } else {
      // TODO: add slice support (?)
      std::cerr << "#DEBUG#: s0 is not a cell" << std::endl;
    }
  } else {
    std::cerr << "#DEBUG#: s0 is absent" << std::endl;
  }

  return 0;
}

int exec_debug_dumptosfmt(VmState* st, CellSlice& cs, unsigned args, int pfx_bits) {
  if (!vm_debug_enabled) {
    VM_LOG(st) << "execute DUMPTOSFMT";
    return 0;
  }

  int data_bits = ((args & 15) + 1) * 8;
  if (!cs.have(pfx_bits + data_bits)) {
    throw VmError{Excno::inv_opcode, "not enough data bits for a DUMPTOSFMT instruction"};
  }
  cs.advance(pfx_bits);
  auto slice = cs.fetch_subslice(data_bits);

  unsigned char tmp[128];
  slice.write().fetch_bytes(tmp, data_bits / 8);
  std::string s{tmp, tmp + (data_bits / 8)};

  VM_LOG(st) << "execute DUMPTOSFMT " << s;

  Stack& stack = st->get_stack();
  return dump_tlb_s0(stack, s);
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
  std::cerr << "#DEBUG#: stack(" << d << " values) : ";
  if (d > 255) {
    std::cerr << "... ";
    d = 255;
  }
  for (int i = d; i > 0; i--) {
    stack[i - 1].print_list(std::cerr);
    std::cerr << ' ';
  }
  std::cerr << std::endl;
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
    std::cerr << "#DEBUG#: s" << arg << " = ";
    stack[arg].print_list(std::cerr);
    std::cerr << std::endl;
  } else {
    std::cerr << "#DEBUG#: s" << arg << " is absent" << std::endl;
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

        std::cerr << "#DEBUG#: " << s << std::endl;
      } else {
        std::cerr << "#DEBUG#: slice contains not valid bits count" << std::endl;
      }

    } else {
      std::cerr << "#DEBUG#: is not a slice" << std::endl;
    }
  } else {
    std::cerr << "#DEBUG#: s0 is absent" << std::endl;
  }

  return 0;
}

void register_debug_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  if (!vm_debug_enabled) {
    cp0.insert(OpcodeInstr::mkfixedrange(0xfe00, 0xfef0, 16, 8, instr::dump_1c_and(0xff, "DEBUG "), exec_dummy_debug))
        .insert(OpcodeInstr::mkext(0xfef, 12, 4, dump_dummy_debug_str, exec_dummy_debug_str, compute_len_debug_str));
  } else {
    // NB: all non-redefined opcodes in fe00..feff should be redirected to dummy debug definitions
    cp0.insert(OpcodeInstr::mksimple(0xfe00, 16, "DUMPSTK", exec_dump_stack))
        .insert(OpcodeInstr::mkfixedrange(0xfe01, 0xfe14, 16, 8, instr::dump_1c_and(0xff, "DEBUG "), exec_dummy_debug))
        .insert(OpcodeInstr::mksimple(0xfe14, 16, "STRDUMP", exec_dump_string))
        .insert(OpcodeInstr::mkfixedrange(0xfe15, 0xfe20, 16, 8, instr::dump_1c_and(0xff, "DEBUG "), exec_dummy_debug))
        .insert(OpcodeInstr::mkfixed(0xfe2, 12, 4, instr::dump_1sr("DUMP"), exec_dump_value))
        .insert(OpcodeInstr::mkfixedrange(0xfe30, 0xfef0, 16, 8, instr::dump_1c_and(0xff, "DEBUG "), exec_dummy_debug))
        .insert(OpcodeInstr::mkext(0xfef, 12, 4, dump_dummy_debug_str, exec_debug_dumptosfmt, compute_len_debug_str));
  }
}

}  // namespace vm
