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
#pragma once
#include "common/refcnt.hpp"
#include "vm/cells.h"
#include "vm/vmstate.h"
#include "td/utils/optional.h"

namespace vm {
using td::Ref;

class FakeVmStateLimits : public VmStateInterface {
  long long ops_remaining;
  bool quiet;

 public:
  FakeVmStateLimits(long long max_ops = 1LL << 62, bool _quiet = true) : ops_remaining(max_ops), quiet(_quiet) {
  }
  bool register_op(int op_units = 1) override;
};

class DummyVmState : public VmStateInterface {
 public:
  explicit DummyVmState(std::vector<Ref<Cell>> libraries, int global_version = ton::SUPPORTED_VERSION)
      : libraries(std::move(libraries)), global_version(global_version) {
  }
  Ref<Cell> load_library(td::ConstBitPtr hash) override;
  int get_global_version() const override {
    return global_version;
  }
  td::optional<td::Bits256> get_missing_library() const {
    return missing_library;
  }

 private:
  std::vector<Ref<Cell>> libraries;
  int global_version;
  td::optional<td::Bits256> missing_library;
};

}  // namespace vm
