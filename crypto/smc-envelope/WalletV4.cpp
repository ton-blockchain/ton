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
#include "WalletV4.h"
#include "GenericAccount.h"
#include "SmartContractCode.h"

#include "vm/boc.h"
#include "vm/cells/CellString.h"
#include "td/utils/base64.h"

#include <limits>

namespace ton {
td::Result<td::Ref<vm::Cell>> WalletV4::make_a_gift_message(const td::Ed25519::PrivateKey& private_key,
                                                            td::uint32 valid_until, td::Span<Gift> gifts) const {
  CHECK(gifts.size() <= get_max_gifts_size());
  TRY_RESULT(seqno, get_seqno());
  TRY_RESULT(wallet_id, get_wallet_id());
  vm::CellBuilder cb;
  cb.store_long(wallet_id, 32).store_long(valid_until, 32).store_long(seqno, 32);
  cb.store_long(0, 8);  // The only difference with wallet-v3

  for (auto& gift : gifts) {
    td::int32 send_mode = 3;
    if (gift.gramms == -1) {
      send_mode += 128;
    }
    if (gift.send_mode > -1) {
      send_mode = gift.send_mode;
    }
    cb.store_long(send_mode, 8).store_ref(create_int_message(gift));
  }

  auto message_outer = cb.finalize();
  auto signature = private_key.sign(message_outer->get_hash().as_slice()).move_as_ok();
  return vm::CellBuilder().store_bytes(signature).append_cellslice(vm::load_cell_slice(message_outer)).finalize();
}

td::Ref<vm::Cell> WalletV4::get_init_data(const InitData& init_data) noexcept {
  return vm::CellBuilder()
      .store_long(init_data.seqno, 32)
      .store_long(init_data.wallet_id, 32)
      .store_bytes(init_data.public_key)
      .store_zeroes(1)  // plugins dict
      .finalize();
}

td::Result<td::uint32> WalletV4::get_wallet_id() const {
  return TRY_VM([&]() -> td::Result<td::uint32> {
    auto answer = run_get_method("get_subwallet_id");
    if (!answer.success) {
      return td::Status::Error("get_subwallet_id get method failed");
    }
    return static_cast<td::uint32>(answer.stack.write().pop_long_range(std::numeric_limits<td::uint32>::max()));
  }());
}
}  // namespace ton
