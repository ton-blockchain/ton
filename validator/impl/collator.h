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
#include "td/actor/actor.h"
#include "ton/ton-types.h"
#include "validator/validator.h"
#include "block/block-db.h"
#include "vm/cells.h"

namespace ton {
using td::Ref;

extern int collator_settings;  // +1 = force want_split, +2 = force want_merge

class Collator : public td::actor::Actor {
 protected:
  Collator() = default;

 public:
  virtual ~Collator() = default;
  static td::actor::ActorOwn<Collator> create_collator(
      td::actor::ActorId<block::BlockDb> block_db,
      ShardIdFull shard /* , td::actor::ActorId<ValidatorManager> validator_manager */);
  virtual void generate_block_candidate(ShardIdFull shard, td::Promise<BlockCandidate> promise) = 0;
  virtual td::Result<bool> register_external_message_cell(Ref<vm::Cell> ext_msg) = 0;
  virtual td::Result<bool> register_external_message(td::Slice ext_msg_boc) = 0;
  virtual td::Result<bool> register_ihr_message_cell(Ref<vm::Cell> ihr_msg) = 0;
  virtual td::Result<bool> register_ihr_message(td::Slice ihr_msg_boc) = 0;
  virtual td::Result<bool> register_shard_signatures_cell(Ref<vm::Cell> shard_blk_signatures) = 0;
  virtual td::Result<bool> register_shard_signatures(td::Slice shard_blk_signatures_boc) = 0;
};

}  // namespace ton
