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
#pragma once
#include "common/refcnt.hpp"
#include "common/refint.h"
#include "vm/cells.h"
#include "vm/cellslice.h"
#include "vm/dict.h"
#include "vm/boc.h"
#include <ostream>
#include "tl/tlblib.hpp"
#include "td/utils/bits.h"
#include "ton/ton-types.h"
#include "block/block.h"
#include "block/mc-config.h"

namespace block::precompiled {

struct Result {
  int exit_code = 0;
  td::optional<long long> exit_arg;
  bool accepted = true;
  bool committed = false;

  static Result error(int code, long long arg = 0) {
    Result res;
    res.exit_code = code;
    res.exit_arg = arg;
    return res;
  }

  static Result error(vm::Excno code, long long arg = 0) {
    Result res;
    res.exit_code = (int)code;
    res.exit_arg = arg;
    return res;
  }

  static Result not_accepted(int code = 0) {
    Result res;
    res.exit_code = code;
    res.accepted = false;
    return res;
  }

  static Result success() {
    Result res;
    res.committed = true;
    return res;
  }
};

class PrecompiledSmartContract {
 public:
  virtual ~PrecompiledSmartContract() = default;

  virtual std::string get_name() const = 0;

  virtual int required_version() const {
    return 6;
  }

  Result run(td::Ref<vm::CellSlice> my_address, ton::UnixTime now, ton::LogicalTime cur_lt, CurrencyCollection balance,
             td::Ref<vm::Cell> c4, vm::CellSlice msg_body, td::Ref<vm::Cell> msg, CurrencyCollection msg_balance,
             bool is_external, std::vector<td::Ref<vm::Cell>> libraries, int global_version, td::uint16 max_data_depth,
             td::Ref<vm::Cell> my_code, td::Ref<vm::Tuple> unpacked_config, td::RefInt256 due_payment, td::uint64 precompiled_gas_usage);

  td::Ref<vm::Cell> get_c4() const {
    return c4_;
  }
  td::Ref<vm::Cell> get_c5() const {
    return c5_;
  }

 protected:
  td::Ref<vm::CellSlice> my_address_;
  ton::UnixTime now_;
  ton::LogicalTime cur_lt_;
  CurrencyCollection balance_;
  vm::CellSlice in_msg_body_;
  td::Ref<vm::Cell> in_msg_;
  CurrencyCollection in_msg_balance_;
  bool is_external_;
  td::Ref<vm::Cell> my_code_;
  td::Ref<vm::Tuple> unpacked_config_;
  td::RefInt256 due_payment_;
  td::uint64 precompiled_gas_usage_;

  td::Ref<vm::Cell> c4_;
  td::Ref<vm::Cell> c5_ = vm::CellBuilder().finalize_novm();

  void send_raw_message(const td::Ref<vm::Cell>& msg, int mode);
  void raw_reserve(const td::RefInt256& amount, int mode);

  td::RefInt256 get_compute_fee(ton::WorkchainId wc, td::uint64 gas_used);
  td::RefInt256 get_forward_fee(ton::WorkchainId wc, td::uint64 bits, td::uint64 cells);
  td::RefInt256 get_storage_fee(ton::WorkchainId wc, td::uint64 duration, td::uint64 bits, td::uint64 cells);
  td::RefInt256 get_simple_compute_fee(ton::WorkchainId wc, td::uint64 gas_used);
  td::RefInt256 get_simple_forward_fee(ton::WorkchainId wc, td::uint64 bits, td::uint64 cells);
  td::RefInt256 get_original_fwd_fee(ton::WorkchainId wc, const td::RefInt256& x);

  virtual Result do_run() = 0;
};

std::unique_ptr<PrecompiledSmartContract> get_implementation(td::Bits256 code_hash);
void set_precompiled_execution_enabled(bool value);  // disabled by default

}  // namespace block::precompiled