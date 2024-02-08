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
#include "common.h"
#include <memory>
#include "vm/memo.h"

namespace block::precompiled {

using namespace vm;

Result PrecompiledSmartContract::run(td::Ref<vm::CellSlice> my_address, ton::UnixTime now, ton::LogicalTime cur_lt,
                                     CurrencyCollection balance, td::Ref<vm::Cell> c4, vm::CellSlice msg_body,
                                     td::Ref<vm::Cell> msg, CurrencyCollection msg_balance, bool is_external,
                                     std::vector<td::Ref<vm::Cell>> libraries, int global_version,
                                     td::uint16 max_data_depth, td::Ref<vm::Cell> my_code,
                                     td::Ref<vm::Tuple> unpacked_config, td::RefInt256 due_payment,
                                     td::uint64 precompiled_gas_usage) {
  my_address_ = std::move(my_address);
  now_ = now;
  cur_lt_ = cur_lt;
  balance_ = std::move(balance);
  c4_ = (c4.not_null() ? std::move(c4) : CellBuilder().finalize());
  in_msg_body_ = std::move(msg_body);
  in_msg_ = std::move(msg);
  in_msg_balance_ = std::move(msg_balance);
  is_external_ = is_external;
  my_code_ = std::move(my_code);
  unpacked_config_ = std::move(unpacked_config);
  due_payment_ = std::move(due_payment);
  precompiled_gas_usage_ = precompiled_gas_usage;

  vm::DummyVmState vm_state{std::move(libraries), global_version};
  vm::VmStateInterface::Guard guard{&vm_state};

  Result result;
  try {
    result = do_run();
  } catch (vm::VmError &e) {
    result = Result::error(e.get_errno(), e.get_arg());
  } catch (Result &r) {
    result = std::move(r);
  }

  if (result.exit_code != 0 && result.exit_code != 1) {
    // see VmState::try_commit()
    if (c4_.is_null() || c4_->get_depth() > max_data_depth || c4_->get_level() != 0 || c5_.is_null() ||
        c5_->get_depth() > max_data_depth || c5_->get_level() != 0) {
      result = Result::error(Excno::cell_ov, 0);
    }
  }
  return result;
}

void PrecompiledSmartContract::send_raw_message(const td::Ref<Cell> &msg, int mode) {
  CellBuilder cb;
  if (!(cb.store_ref_bool(c5_)                 // out_list$_ {n:#} prev:^(OutList n)
        && cb.store_long_bool(0x0ec3c86d, 32)  // action_send_msg#0ec3c86d
        && cb.store_long_bool(mode, 8)         // mode:(## 8)
        && cb.store_ref_bool(msg))) {
    throw VmError{Excno::cell_ov, "cannot serialize raw output message into an output action cell"};
  }
  c5_ = cb.finalize_novm();
}

void PrecompiledSmartContract::raw_reserve(const td::RefInt256 &amount, int mode) {
  if (amount->sgn() < 0) {
    throw VmError{Excno::range_chk, "amount of nanograms must be non-negative"};
  }
  CellBuilder cb;
  if (!(cb.store_ref_bool(c5_)                             // out_list$_ {n:#} prev:^(OutList n)
        && cb.store_long_bool(0x36e6b809, 32)              // action_reserve_currency#36e6b809
        && cb.store_long_bool(mode, 8)                     // mode:(## 8)
        && util::store_coins(cb, std::move(amount), true)  //
        && cb.store_maybe_ref({}))) {
    throw VmError{Excno::cell_ov, "cannot serialize raw reserved currency amount into an output action cell"};
  }
  c5_ = cb.finalize_novm();
}

td::RefInt256 PrecompiledSmartContract::get_compute_fee(ton::WorkchainId wc, td::uint64 gas_used) {
  if (gas_used >= (1ULL << 63)) {
    throw VmError{Excno::range_chk};
  }
  block::GasLimitsPrices prices = util::get_gas_prices(unpacked_config_, wc);
  return util::check_finite(prices.compute_gas_price(gas_used));
}

td::RefInt256 PrecompiledSmartContract::get_forward_fee(ton::WorkchainId wc, td::uint64 bits, td::uint64 cells) {
  if (bits >= (1ULL << 63) || cells >= (1ULL << 63)) {
    throw VmError{Excno::range_chk};
  }
  block::MsgPrices prices = util::get_msg_prices(unpacked_config_, wc);
  return util::check_finite(prices.compute_fwd_fees256(cells, bits));
}

td::RefInt256 PrecompiledSmartContract::get_storage_fee(ton::WorkchainId wc, td::uint64 duration, td::uint64 bits,
                                                        td::uint64 cells) {
  if (bits >= (1ULL << 63) || cells >= (1ULL << 63) || duration >= (1ULL << 63)) {
    throw VmError{Excno::range_chk};
  }
  td::optional<block::StoragePrices> maybe_prices = util::get_storage_prices(unpacked_config_);
  return util::check_finite(util::calculate_storage_fee(maybe_prices, wc, duration, bits, cells));
}

td::RefInt256 PrecompiledSmartContract::get_simple_compute_fee(ton::WorkchainId wc, td::uint64 gas_used) {
  if (gas_used >= (1ULL << 63)) {
    throw VmError{Excno::range_chk};
  }
  block::GasLimitsPrices prices = util::get_gas_prices(unpacked_config_, wc);
  return util::check_finite(td::rshift(td::make_refint(prices.gas_price) * gas_used, 16, 1));
}

td::RefInt256 PrecompiledSmartContract::get_simple_forward_fee(ton::WorkchainId wc, td::uint64 bits, td::uint64 cells) {
  if (bits >= (1ULL << 63) || cells >= (1ULL << 63)) {
    throw VmError{Excno::range_chk};
  }
  block::MsgPrices prices = util::get_msg_prices(unpacked_config_, wc);
  return util::check_finite(
      td::rshift(td::make_refint(prices.bit_price) * bits + td::make_refint(prices.cell_price) * cells, 16, 1));
}

td::RefInt256 PrecompiledSmartContract::get_original_fwd_fee(ton::WorkchainId wc, const td::RefInt256 &x) {
  if (x->sgn() < 0) {
    throw VmError{Excno::range_chk, "fwd_fee is negative"};
  }
  block::MsgPrices prices = util::get_msg_prices(unpacked_config_, wc);
  return util::check_finite(td::muldiv(x, td::make_refint(1 << 16), td::make_refint((1 << 16) - prices.first_frac)));
}

static std::atomic_bool precompiled_execution_enabled{false};

std::unique_ptr<PrecompiledSmartContract> get_implementation(td::Bits256 code_hash) {
  if (!precompiled_execution_enabled) {
    return nullptr;
  }
  static std::map<td::Bits256, std::unique_ptr<PrecompiledSmartContract> (*)()> map = []() {
    auto from_hex = [](td::Slice s) -> td::Bits256 {
      td::Bits256 x;
      CHECK(x.from_hex(s) == 256);
      return x;
    };
    std::map<td::Bits256, std::unique_ptr<PrecompiledSmartContract> (*)()> map;
#define CONTRACT(hash, cls) \
  map[from_hex(hash)] = []() -> std::unique_ptr<PrecompiledSmartContract> { return std::make_unique<cls>(); };
    // CONTRACT("CODE_HASH_HEX", ClassName);
    return map;
  }();
  auto it = map.find(code_hash);
  return it == map.end() ? nullptr : it->second();
}

void set_precompiled_execution_enabled(bool value) {
  precompiled_execution_enabled = value;
}

}  // namespace block::precompiled
