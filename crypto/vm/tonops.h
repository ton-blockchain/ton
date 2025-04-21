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
#pragma once
#include "vm/vm.h"
#include "ton/ton-types.h"
#include "mc-config.h"

namespace vm {

class OpcodeTable;

void register_ton_ops(OpcodeTable& cp0);

namespace util {

// "_q" functions throw on error if not quiet, return false if quiet (leaving cs unchanged)
bool load_var_integer_q(CellSlice& cs, td::RefInt256& res, int len_bits, bool sgnd, bool quiet);
bool load_coins_q(CellSlice& cs, td::RefInt256& res, bool quiet);
bool load_msg_addr_q(CellSlice& cs, CellSlice& res, int global_version, bool quiet);
bool parse_std_addr_q(CellSlice cs, ton::WorkchainId& res_wc, ton::StdSmcAddress& res_addr, int global_version,
                      bool quiet);

// Non-"_q" functions throw on error
td::RefInt256 load_var_integer(CellSlice& cs, int len_bits, bool sgnd);
td::RefInt256 load_coins(CellSlice& cs);
CellSlice load_msg_addr(CellSlice& cs, int global_version);
std::pair<ton::WorkchainId, ton::StdSmcAddress> parse_std_addr(CellSlice cs, int global_version);

// store_... functions throw on error if not quiet, return false if quiet (leaving cb unchanged)
bool store_var_integer(CellBuilder& cb, const td::RefInt256& x, int len_bits, bool sgnd, bool quiet = false);
bool store_coins(CellBuilder& cb, const td::RefInt256& x, bool quiet = false);

block::GasLimitsPrices get_gas_prices(const td::Ref<Tuple>& unpacked_config, bool is_masterchain);
block::MsgPrices get_msg_prices(const td::Ref<Tuple>& unpacked_config, bool is_masterchain);
td::optional<block::StoragePrices> get_storage_prices(const td::Ref<Tuple>& unpacked_config);
td::RefInt256 calculate_storage_fee(const td::optional<block::StoragePrices>& maybe_prices, bool is_masterchain,
                                    td::uint64 delta, td::uint64 bits, td::uint64 cells);

}  // namespace util

}  // namespace vm
