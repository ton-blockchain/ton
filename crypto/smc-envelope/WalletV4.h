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

#include "smc-envelope/SmartContract.h"
#include "smc-envelope/WalletInterface.h"
#include "vm/cells.h"
#include "Ed25519.h"
#include "block/block.h"
#include "vm/cells/CellString.h"

namespace ton {

struct WalletV4Traits {
  using InitData = WalletInterface::DefaultInitData;

  static constexpr unsigned max_message_size = vm::CellString::max_bytes;
  static constexpr unsigned max_gifts_size = 4;
  static constexpr auto code_type = SmartContractCode::WalletV4;
};

class WalletV4 : public WalletBase<WalletV4, WalletV4Traits> {
 public:
  explicit WalletV4(State state) : WalletBase(std::move(state)) {
  }
  td::Result<td::Ref<vm::Cell>> make_a_gift_message(const td::Ed25519::PrivateKey& private_key, td::uint32 valid_until,
                                                    td::Span<Gift> gifts) const override;
  static td::Ref<vm::Cell> get_init_data(const InitData& init_data) noexcept;

  td::Result<td::uint32> get_wallet_id() const override;
};
}  // namespace ton