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
#include <limits>

#include "td/utils/base64.h"
#include "vm/boc.h"
#include "vm/cells/CellString.h"

#include "GenericAccount.h"
#include "HighloadWalletV2.h"
#include "SmartContractCode.h"

namespace ton {

td::Result<td::Ref<vm::Cell>> HighloadWalletV2::get_init_message(const td::Ed25519::PrivateKey& private_key,
                                                                 td::uint32 valid_until) const noexcept {
  TRY_RESULT(wallet_id, get_wallet_id());
  td::uint32 id = -1;
  auto make_message = [&](td::Slice signature) -> td::Result<td::Ref<vm::Cell>> {
    vm::CellBuilder cb;
    if (!signature.empty()) {
      cb.store_bytes(signature);
    }
    if (!(cb.store_long_bool(wallet_id, 32) && cb.store_long_bool(valid_until, 32) && cb.store_long_bool(id, 32) &&
          cb.store_maybe_ref({}))) {
      return td::Status::Error("Failed to store highload wallet init message");
    }
    return cb.finalize();
  };
  TRY_RESULT(unsigned_message, make_message({}));
  TRY_RESULT(signature, private_key.sign(unsigned_message->get_hash().as_slice()));

  return make_message(signature.as_slice());
}

td::Result<td::Ref<vm::Cell>> HighloadWalletV2::make_a_gift_message(const td::Ed25519::PrivateKey& private_key,
                                                                    td::uint32 valid_until,
                                                                    td::Span<Gift> gifts) const {
  TRY_RESULT(wallet_id, get_wallet_id());
  if (gifts.size() > get_max_gifts_size()) {
    return td::Status::Error("Too many messages");
  }
  vm::Dictionary messages(16);
  for (size_t i = 0; i < gifts.size(); i++) {
    auto& gift = gifts[i];
    td::int32 send_mode = 3;
    if (gift.gramms == -1) {
      send_mode += 128;
    }
    TRY_RESULT(message, try_create_int_message(gift));
    vm::CellBuilder cb;
    cb.store_long(send_mode, 8).store_ref(std::move(message));
    auto key = messages.integer_key(td::make_refint(i), 16, false);
    messages.set_builder(key.bits(), 16, cb);
  }
  std::string hash;
  {
    vm::CellBuilder cb;
    if (!cb.store_maybe_ref(messages.get_root_cell())) {
      return td::Status::Error("Failed to store highload wallet messages hash");
    }
    hash = cb.finalize()->get_hash().as_slice().substr(28, 4).str();
  }

  vm::CellBuilder cb;
  cb.store_long(wallet_id, 32).store_long(valid_until, 32).store_bytes(hash);
  if (!cb.store_maybe_ref(messages.get_root_cell())) {
    return td::Status::Error("Failed to store highload wallet messages");
  }
  auto message_outer = cb.finalize();
  TRY_RESULT(signature, private_key.sign(message_outer->get_hash().as_slice()));
  return vm::CellBuilder().store_bytes(signature).append_cellslice(vm::load_cell_slice(message_outer)).finalize();
}

td::Ref<vm::Cell> HighloadWalletV2::get_init_data(const InitData& init_data) noexcept {
  vm::CellBuilder cb;
  cb.store_long(init_data.wallet_id, 32).store_long(init_data.seqno, 64).store_bytes(init_data.public_key);
  if (!cb.store_maybe_ref({})) {
    return {};
  }
  return cb.finalize();
}

td::Result<td::uint32> HighloadWalletV2::get_wallet_id() const {
  return TRY_VM([&]() -> td::Result<td::uint32> {
    if (state_.data.is_null()) {
      return 0;
    }
    auto cs = vm::load_cell_slice(state_.data);
    return static_cast<td::uint32>(cs.fetch_ulong(32));
  }());
}

td::Result<td::Ed25519::PublicKey> HighloadWalletV2::get_public_key() const {
  return TRY_VM([&]() -> td::Result<td::Ed25519::PublicKey> {
    if (state_.data.is_null()) {
      return td::Status::Error("data is null");
    }
    auto cs = vm::load_cell_slice(state_.data);
    cs.skip_first(96);
    td::SecureString res(td::Ed25519::PublicKey::LENGTH);
    cs.fetch_bytes(res.as_mutable_slice().ubegin(), td::narrow_cast<td::int32>(res.size()));
    return td::Ed25519::PublicKey(std::move(res));
  }());
}

}  // namespace ton
