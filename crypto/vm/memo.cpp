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

    Copyright 2020 Telegram Systems LLP
*/
#include "vm/memo.h"
#include "vm/excno.hpp"
#include "vm/vm.h"

namespace vm {
using td::Ref;

bool FakeVmStateLimits::register_op(int op_units) {
  bool ok = (ops_remaining -= op_units) >= 0;
  if (!ok && !quiet) {
    throw VmError{Excno::out_of_gas, "too many operations"};
  }
  return ok;
}

Ref<Cell> DummyVmState::load_library(td::ConstBitPtr hash) {
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

}  // namespace vm
