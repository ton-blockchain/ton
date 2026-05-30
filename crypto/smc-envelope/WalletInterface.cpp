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
#include "WalletInterface.h"

#include "td/utils/logging.h"

namespace ton {
td::Result<td::uint64> WalletInterface::get_balance(td::uint64 account_balance, td::uint32 now) const {
  return TRY_VM([&]() -> td::Result<td::uint64> {
    Answer answer = this->run_get_method(Args().set_method_id("balance").set_balance(account_balance).set_now(now));
    if (!answer.success) {
      return td::Status::Error("balance get method failed");
    }
    return static_cast<td::uint64>(answer.stack.write().pop_long());
  }());
}

td::Result<td::Ed25519::PublicKey> WalletInterface::get_public_key() const {
  return GenericAccount::get_public_key(*this);
};

td::Result<td::uint32> WalletInterface::get_seqno() const {
  return GenericAccount::get_seqno(*this);
}

td::Result<td::uint32> WalletInterface::get_wallet_id() const {
  return GenericAccount::get_wallet_id(*this);
}

td::Result<td::Ref<vm::Cell>> WalletInterface::get_init_message(const td::Ed25519::PrivateKey &private_key,
                                                                td::uint32 valid_until) const {
  return make_a_gift_message(private_key, valid_until, {});
}

td::Result<td::Ref<vm::Cell>> WalletInterface::try_create_int_message(const Gift &gift) {
  vm::CellBuilder cbi;
  if (!GenericAccount::store_int_message(cbi, gift.destination, gift.gramms < 0 ? 0 : gift.gramms,
                                         gift.extra_currencies)) {
    return td::Status::Error("Failed to store internal message header");
  }
  if (gift.init_state.not_null()) {
    if (!(cbi.store_ones_bool(2) && cbi.store_ref_bool(gift.init_state))) {
      return td::Status::Error("Failed to store internal message init state");
    }
  } else if (!cbi.store_zeroes_bool(1)) {
    return td::Status::Error("Failed to store empty internal message init state");
  }
  TRY_STATUS(store_gift_message(cbi, gift));
  td::Ref<vm::Cell> res;
  if (!cbi.finalize_to(res)) {
    return td::Status::Error("Failed to finalize internal message");
  }
  return res;
}

td::Ref<vm::Cell> WalletInterface::create_int_message(const Gift &gift) {
  auto r_message = try_create_int_message(gift);
  if (r_message.is_error()) {
    LOG(ERROR) << "Failed to create internal message: " << r_message.error();
    return {};
  }
  return r_message.move_as_ok();
}

td::Status WalletInterface::store_gift_message(vm::CellBuilder &cb, const Gift &gift) {
  if (gift.body.not_null()) {
    auto body = vm::load_cell_slice(gift.body);
    if (cb.can_extend_by(1 + body.size(), body.size_refs())) {
      if (cb.store_zeroes_bool(1) && cb.append_cellslice_bool(body)) {
        return td::Status::OK();
      }
      return td::Status::Error("Failed to store inline message body");
    }
    if (cb.store_ones_bool(1) && cb.store_ref_bool(gift.body)) {
      return td::Status::OK();
    }
    return td::Status::Error("Failed to store referenced message body");
  }

  if (!cb.store_zeroes_bool(1)) {
    return td::Status::Error("Failed to store empty message body marker");
  }
  if (!cb.store_long_bool(gift.is_encrypted ? EncryptedCommentOp : 0, 32)) {
    return td::Status::Error("Failed to store message body type");
  }
  TRY_STATUS(vm::CellString::store(cb, gift.message, 35 * 8));
  return td::Status::OK();
}
}  // namespace ton
