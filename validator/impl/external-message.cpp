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

#include "external-message.hpp"
#include "collator-impl.h"
#include "vm/boc.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "block/block-db.h"
#include "fabric.h"
#include "td/actor/actor.h"
#include "td/utils/Random.h"
#include "crypto/openssl/rand.hpp"

namespace ton {

namespace validator {
using td::Ref;

ExtMessageQ::ExtMessageQ(td::BufferSlice data, td::Ref<vm::Cell> root, AccountIdPrefixFull addr_prefix, ton::WorkchainId wc, ton::StdSmcAddress addr)
    : root_(std::move(root)), addr_prefix_(addr_prefix), data_(std::move(data)), wc_(wc), addr_(addr) {
  hash_ = block::compute_file_hash(data_);
}

td::Result<Ref<ExtMessageQ>> ExtMessageQ::create_ext_message(td::BufferSlice data,
                                                             block::SizeLimitsConfig::ExtMsgLimits limits) {
  if (data.size() > limits.max_size) {
    return td::Status::Error("external message too large, rejecting");
  }
  vm::BagOfCells boc;
  auto res = boc.deserialize(data.as_slice());
  if (res.is_error()) {
    return res.move_as_error();
  }
  if (boc.get_root_count() != 1) {
    return td::Status::Error("external message is not a valid bag of cells");  // not a valid bag-of-Cells
  }
  auto ext_msg = boc.get_root_cell();
  if (ext_msg->get_level() != 0) {
    return td::Status::Error("external message must have zero level");
  }
  if (ext_msg->get_depth() >= limits.max_depth) {
    return td::Status::Error("external message is too deep");
  }
  vm::CellSlice cs{vm::NoVmOrd{}, ext_msg};
  if (cs.prefetch_ulong(2) != 2) {  // ext_in_msg_info$10
    return td::Status::Error("external message must begin with ext_in_msg_info$10");
  }
  ton::Bits256 hash{ext_msg->get_hash().bits()};
  if (!block::gen::t_Message_Any.validate_ref(128, ext_msg)) {
    return td::Status::Error("external message is not a (Message Any) according to automated checks");
  }
  if (!block::tlb::t_Message.validate_ref(128, ext_msg)) {
    return td::Status::Error("external message is not a (Message Any) according to hand-written checks");
  }
  block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
  if (!tlb::unpack_cell_inexact(ext_msg, info)) {
    return td::Status::Error("cannot unpack external message header");
  }
  auto dest_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.dest);
  if (!dest_prefix.is_valid()) {
    return td::Status::Error("destination of an inbound external message is an invalid blockchain address");
  }
  ton::StdSmcAddress addr;
  ton::WorkchainId wc;
  if(!block::tlb::t_MsgAddressInt.extract_std_address(info.dest, wc, addr)) {
    return td::Status::Error(PSLICE() << "Can't parse destination address");
  }

  return Ref<ExtMessageQ>{true, std::move(data), std::move(ext_msg), dest_prefix, wc, addr};
}

void ExtMessageQ::run_message(td::BufferSlice data, block::SizeLimitsConfig::ExtMsgLimits limits,
                              td::actor::ActorId<ton::validator::ValidatorManager> manager,
                              td::Promise<td::Ref<ExtMessage>> promise) {
  auto R = create_ext_message(std::move(data), limits);
  if (R.is_error()) {
    return promise.set_error(R.move_as_error_prefix("failed to parse external message "));
  }
  auto M = R.move_as_ok();
  auto root = M->root_cell();
  block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
  tlb::unpack_cell_inexact(root, info);  // checked in create message
  ton::StdSmcAddress addr = M->addr();
  ton::WorkchainId wc = M->wc();

  run_fetch_account_state(
      wc, addr, manager,
      [promise = std::move(promise), msg_root = root, wc,
       M](td::Result<std::tuple<td::Ref<vm::CellSlice>, UnixTime, LogicalTime, std::unique_ptr<block::ConfigInfo>>>
              res) mutable {
        if (res.is_error()) {
          promise.set_error(td::Status::Error(PSLICE() << "Failed to get account state"));
        } else {
          auto tuple = res.move_as_ok();
          block::Account acc;
          auto shard_acc = std::move(std::get<0>(tuple));
          auto utime = std::get<1>(tuple);
          auto lt = std::get<2>(tuple);
          auto config = std::move(std::get<3>(tuple));
          if (!acc.unpack(shard_acc, {}, utime, false)) {
            promise.set_error(td::Status::Error(PSLICE() << "Failed to unpack account state"));
          } else {
            auto status = run_message_on_account(wc, &acc, utime, lt + 1, msg_root, std::move(config));
            if (status.is_ok()) {
              promise.set_value(std::move(M));
            } else {
              promise.set_error(td::Status::Error(PSLICE() << "External message was not accepted\n"
                                                           << status.message()));
            }
          }
        }
      });
}

td::Status ExtMessageQ::run_message_on_account(ton::WorkchainId wc,
                                               block::Account* acc,
                                               UnixTime utime, LogicalTime lt,
                                               td::Ref<vm::Cell> msg_root,
                                               std::unique_ptr<block::ConfigInfo> config) {

   Ref<vm::Cell> old_mparams;
   std::vector<block::StoragePrices> storage_prices_;
   block::StoragePhaseConfig storage_phase_cfg_{&storage_prices_};
   td::BitArray<256> rand_seed_;
   block::ComputePhaseConfig compute_phase_cfg_;
   block::ActionPhaseConfig action_phase_cfg_;
   td::RefInt256 masterchain_create_fee, basechain_create_fee;

   auto fetch_res = Collator::impl_fetch_config_params(std::move(config), &old_mparams,
                                                 &storage_prices_, &storage_phase_cfg_,
                                                 &rand_seed_, &compute_phase_cfg_,
                                                 &action_phase_cfg_, &masterchain_create_fee,
                                                 &basechain_create_fee, wc);
   if(fetch_res.is_error()) {
     auto error = fetch_res.move_as_error();
     LOG(DEBUG) << "Cannot fetch config params: " << error.message();
     return error.move_as_error_prefix("Cannot fetch config params: ");
   }
   compute_phase_cfg_.with_vm_log = true;

   auto res = Collator::impl_create_ordinary_transaction(msg_root, acc, utime, lt,
                                                    &storage_phase_cfg_, &compute_phase_cfg_,
                                                    &action_phase_cfg_,
                                                    true, lt);
   if(res.is_error()) {
     auto error = res.move_as_error();
     LOG(DEBUG) << "Cannot run message on account: " << error.message();
     return error.move_as_error_prefix("Cannot run message on account: ");
   }
   std::unique_ptr<block::Transaction> trans = res.move_as_ok();

   auto trans_root = trans->commit(*acc);
   if (trans_root.is_null()) {
     LOG(DEBUG) << "Cannot commit new transaction for smart contract";
     return td::Status::Error("Cannot commit new transaction for smart contract");
   }
   return td::Status::OK();
}

}  // namespace validator
}  // namespace ton
