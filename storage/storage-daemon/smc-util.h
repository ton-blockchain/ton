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
#include "ton/ton-types.h"
#include "crypto/vm/cellslice.h"
#include "block/block-parse.h"

using namespace ton;

struct ContractAddress {
  WorkchainId wc = workchainIdNotYet;
  td::Bits256 addr = td::Bits256::zero();

  ContractAddress() = default;
  ContractAddress(WorkchainId wc, td::Bits256 addr) : wc(wc), addr(addr) {
  }

  std::string to_string() const {
    return PSTRING() << wc << ":" << addr.to_hex();
  }
  td::Ref<vm::CellSlice> to_cellslice() const {
    return block::tlb::t_MsgAddressInt.pack_std_address(wc, addr);
  }

  static td::Result<ContractAddress> parse(td::Slice s) {
    TRY_RESULT(x, block::StdAddress::parse(s));
    return ContractAddress(x.workchain, x.addr);
  }

  bool operator==(const ContractAddress& other) const {
    return wc == other.wc && addr == other.addr;
  }
  bool operator!=(const ContractAddress& other) const {
    return !(*this == other);
  }
  bool operator<(const ContractAddress& other) const {
    return wc == other.wc ? addr < other.addr : wc < other.wc;
  }
};