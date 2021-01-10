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

#include "smc-envelope/SmartContract.h"
#include "smc-envelope/WalletInterface.h"
#include "vm/cells.h"
#include "Ed25519.h"
#include "block/block.h"
#include "vm/cells/CellString.h"

namespace ton {

struct WalletV3Traits {
  using InitData = WalletInterface::DefaultInitData;

  static constexpr unsigned max_message_size = vm::CellString::max_bytes;
  static constexpr unsigned max_gifts_size = 4;
  static constexpr auto code_type = SmartContractCode::WalletV3;
};

class WalletV3 : public WalletBase<WalletV3, WalletV3Traits> {
 public:
  explicit WalletV3(State state) : WalletBase(std::move(state)) {
  }
  td::Result<td::Ref<vm::Cell>> make_a_gift_message(const td::Ed25519::PrivateKey& private_key, td::uint32 valid_until,
                                                    td::Span<Gift> gifts) const override;
  static td::Ref<vm::Cell> get_init_data(const InitData& init_data) noexcept;

  // can't use get methods for compatibility with old revisions
  td::Result<td::uint32> get_wallet_id() const override;
  td::Result<td::Ed25519::PublicKey> get_public_key() const override;
};
}  // namespace ton

namespace ton {

struct RestrictedWalletTraits {
  struct InitData {
    td::SecureString init_key;
    td::SecureString main_key;
    td::uint32 wallet_id{0};
  };

  static constexpr unsigned max_message_size = vm::CellString::max_bytes;
  static constexpr unsigned max_gifts_size = 4;
  static constexpr auto code_type = SmartContractCode::RestrictedWallet;
};

class RestrictedWallet : public WalletBase<RestrictedWallet, RestrictedWalletTraits> {
 public:
  struct Config {
    td::uint32 start_at{0};
    std::vector<std::pair<td::int32, td::uint64>> limits;
  };

  explicit RestrictedWallet(State state) : WalletBase(std::move(state)) {
  }

  td::Result<Config> get_config() const {
    return TRY_VM([this]() -> td::Result<Config> {
      auto cs = vm::load_cell_slice(get_state().data);
      Config config;
      td::Ref<vm::Cell> dict_root;
      auto ok = cs.advance(32 + 32 + 256) && cs.fetch_uint_to(32, config.start_at) && cs.fetch_maybe_ref(dict_root);
      vm::Dictionary dict(std::move(dict_root), 32);
      dict.check_for_each([&](auto cs, auto ptr, auto ptr_bits) {
        auto r_seconds = td::narrow_cast_safe<td::int32>(dict.key_as_integer(ptr, true)->to_long());
        if (r_seconds.is_error()) {
          ok = false;
          return ok;
        }
        td::uint64 value;
        ok &= smc::unpack_grams(cs, value);
        config.limits.emplace_back(r_seconds.ok(), value);
        return ok;
      });
      if (!ok) {
        return td::Status::Error("Can't parse config");
      }
      std::sort(config.limits.begin(), config.limits.end());
      return config;
    }());
  }

  static td::Ref<vm::Cell> get_init_data(const InitData& init_data) {
    vm::CellBuilder cb;
    cb.store_long(0, 32);
    cb.store_long(init_data.wallet_id, 32);
    CHECK(init_data.init_key.size() == 32);
    CHECK(init_data.main_key.size() == 32);
    cb.store_bytes(init_data.init_key.as_slice());
    cb.store_bytes(init_data.main_key.as_slice());
    return cb.finalize();
  }

  td::Result<td::Ref<vm::Cell>> get_init_message(const td::Ed25519::PrivateKey& init_private_key,
                                                 td::uint32 valid_until, const Config& config) const {
    vm::CellBuilder cb;
    TRY_RESULT(seqno, get_seqno());
    TRY_RESULT(wallet_id, get_wallet_id());
    LOG(ERROR) << "seqno: " << seqno << " wallet_id: " << wallet_id;
    if (seqno != 0) {
      return td::Status::Error("Wallet is already inited");
    }

    cb.store_long(wallet_id, 32);
    cb.store_long(valid_until, 32);
    cb.store_long(seqno, 32);

    cb.store_long(config.start_at, 32);
    vm::Dictionary dict(32);

    auto add = [&](td::int32 till, td::uint64 value) {
      auto key = dict.integer_key(td::make_refint(till), 32, true);
      vm::CellBuilder gcb;
      block::tlb::t_Grams.store_integer_value(gcb, td::BigInt256(value));
      dict.set_builder(key.bits(), 32, gcb);
    };
    for (auto limit : config.limits) {
      add(limit.first, limit.second);
    }
    cb.store_maybe_ref(dict.get_root_cell());

    auto message_outer = cb.finalize();
    auto signature = init_private_key.sign(message_outer->get_hash().as_slice()).move_as_ok();
    return vm::CellBuilder().store_bytes(signature).append_cellslice(vm::load_cell_slice(message_outer)).finalize();
  }

  td::Result<td::Ref<vm::Cell>> make_a_gift_message(const td::Ed25519::PrivateKey& private_key, td::uint32 valid_until,
                                                    td::Span<Gift> gifts) const override {
    CHECK(gifts.size() <= Traits::max_gifts_size);

    vm::CellBuilder cb;
    TRY_RESULT(seqno, get_seqno());
    TRY_RESULT(wallet_id, get_wallet_id());
    if (seqno == 0) {
      return td::Status::Error("Wallet is not inited yet");
    }
    cb.store_long(wallet_id, 32);
    cb.store_long(valid_until, 32);
    cb.store_long(seqno, 32);

    for (auto& gift : gifts) {
      td::int32 send_mode = 3;
      if (gift.gramms == -1) {
        send_mode += 128;
      }
      cb.store_long(send_mode, 8).store_ref(create_int_message(gift));
    }

    auto message_outer = cb.finalize();
    auto signature = private_key.sign(message_outer->get_hash().as_slice()).move_as_ok();
    return vm::CellBuilder().store_bytes(signature).append_cellslice(vm::load_cell_slice(message_outer)).finalize();
  }
};
}  // namespace ton
