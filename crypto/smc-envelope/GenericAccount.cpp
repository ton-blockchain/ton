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
#include "block/block-auto.h"
#include "block/block-parse.h"

#include "GenericAccount.h"
namespace ton {

namespace smc {
td::Ref<vm::CellSlice> pack_grams(td::uint64 amount) {
  vm::CellBuilder cb;
  block::tlb::t_Grams.store_integer_value(cb, td::BigInt256(amount));
  return vm::load_cell_slice_ref(cb.finalize());
}

bool unpack_grams(td::Ref<vm::CellSlice> cs, td::uint64& amount) {
  td::RefInt256 got;
  if (!block::tlb::t_Grams.as_integer_to(cs, got)) {
    return false;
  }
  if (!got->unsigned_fits_bits(63)) {
    return false;
  }
  auto x = got->to_long();
  if (x < 0) {
    return false;
  }
  amount = x;
  return true;
}
}  // namespace smc

td::Ref<vm::Cell> GenericAccount::get_init_state(const td::Ref<vm::Cell>& code,
                                                 const td::Ref<vm::Cell>& data) noexcept {
  return vm::CellBuilder()
      .store_zeroes(2)
      .store_ones(2)
      .store_zeroes(1)
      .store_ref(std::move(code))
      .store_ref(std::move(data))
      .finalize();
}
block::StdAddress GenericAccount::get_address(ton::WorkchainId workchain_id,
                                              const td::Ref<vm::Cell>& init_state) noexcept {
  if (init_state.is_null()) {
    return {};
  }
  return block::StdAddress(workchain_id, init_state->get_hash().bits(), true /*bounce*/);
}

bool GenericAccount::store_int_message(vm::CellBuilder& cb, const block::StdAddress& dest_address, td::int64 gramms,
                                       td::Ref<vm::Cell> extra_currencies) {
  td::BigInt256 dest_addr;
  dest_addr.import_bits(dest_address.addr.as_bitslice());
  return cb.store_zeroes_bool(1) && cb.store_ones_bool(1) && cb.store_long_bool(dest_address.bounceable, 1) &&
         cb.store_zeroes_bool(3) && cb.store_ones_bool(1) && cb.store_zeroes_bool(2) &&
         cb.store_long_bool(dest_address.workchain, 8) && cb.store_int256_bool(dest_addr, 256) &&
         block::tlb::t_Grams.store_integer_value(cb, td::BigInt256(gramms)) && cb.store_maybe_ref(extra_currencies) &&
         cb.store_zeroes_bool(8 + 64 + 32);
}

td::Ref<vm::Cell> GenericAccount::create_ext_message(const block::StdAddress& address, td::Ref<vm::Cell> new_state,
                                                     td::Ref<vm::Cell> body) noexcept {
  if (body.is_null()) {
    return {};
  }
  block::gen::Message::Record message;
  /*info*/ {
    block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
    /* src */
    if (!tlb::csr_pack(info.src, block::gen::MsgAddressExt::Record_addr_none{})) {
      return {};
    }
    /* dest */ {
      block::gen::MsgAddressInt::Record_addr_std dest;
      vm::CellBuilder anycast;
      if (!anycast.store_zeroes_bool(1)) {
        return {};
      }
      dest.anycast = anycast.as_cellslice_ref();
      dest.workchain_id = address.workchain;
      dest.address = address.addr;

      if (!tlb::csr_pack(info.dest, dest)) {
        return {};
      }
    }
    /* import_fee */ {
      vm::CellBuilder cb;
      if (!block::tlb::t_Grams.store_integer_value(cb, td::BigInt256(0))) {
        return {};
      }
      info.import_fee = cb.as_cellslice_ref();
    }

    if (!tlb::csr_pack(message.info, info)) {
      return {};
    }
  }
  /* init */ {
    if (new_state.not_null()) {
      // Just(Left(new_state))
      vm::CellBuilder cb;
      auto init = vm::load_cell_slice(new_state);
      if (!(cb.store_ones_bool(1) && cb.store_zeroes_bool(1) && cb.append_cellslice_bool(init))) {
        return {};
      }
      message.init = cb.as_cellslice_ref();
    } else {
      vm::CellBuilder cb;
      if (!cb.store_zeroes_bool(1)) {
        return {};
      }
      message.init = cb.as_cellslice_ref();
    }
  }
  /* body */ {
    vm::CellBuilder cb;
    auto body_slice = vm::load_cell_slice_ref(body);
    if (body_slice.not_null() && cb.can_extend_by(1 + body_slice->size(), body_slice->size_refs())) {
      if (!(cb.store_zeroes_bool(1) && cb.append_cellslice_bool(body_slice))) {
        return {};
      }
    } else if (!(cb.store_ones_bool(1) && cb.store_ref_bool(body))) {
      return {};
    }
    message.body = cb.as_cellslice_ref();
  }

  td::Ref<vm::Cell> res;
  if (!tlb::type_pack_cell(res, block::gen::t_Message_Any, message) || res.is_null()) {
    return {};
  }

  return res;
}
td::Result<td::Ed25519::PublicKey> GenericAccount::get_public_key(const SmartContract& sc) {
  auto answer = sc.run_get_method("get_public_key");
  if (!answer.success) {
    return td::Status::Error("get_public_key failed");
  }
  auto do_get_public_key = [&]() -> td::Result<td::Ed25519::PublicKey> {
    auto key = answer.stack.write().pop_int_finite();
    td::SecureString bytes(32);
    if (!key->export_bytes(bytes.as_mutable_slice().ubegin(), bytes.size(), false)) {
      return td::Status::Error("get_public_key failed");
    }
    return td::Ed25519::PublicKey(std::move(bytes));
  };
  return TRY_VM(do_get_public_key());
}

td::Result<td::uint32> GenericAccount::get_seqno(const SmartContract& sc) {
  return TRY_VM([&]() -> td::Result<td::uint32> {
    auto answer = sc.run_get_method("seqno");
    if (!answer.success) {
      return td::Status::Error("seqno get method failed");
    }
    return static_cast<td::uint32>(answer.stack.write().pop_long_range(std::numeric_limits<td::uint32>::max()));
  }());
}
td::Result<td::uint32> GenericAccount::get_wallet_id(const SmartContract& sc) {
  return TRY_VM([&]() -> td::Result<td::uint32> {
    auto answer = sc.run_get_method("wallet_id");
    if (!answer.success) {
      return td::Status::Error("wallet_id get method failed");
    }
    return static_cast<td::uint32>(answer.stack.write().pop_long_range(std::numeric_limits<td::uint32>::max()));
  }());
}
}  // namespace ton
