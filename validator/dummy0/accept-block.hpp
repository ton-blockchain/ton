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

    Copyright 2017-2019 Telegram Systems LLP
*/
#pragma once

#include "td/actor/actor.h"
#include "ton/ton-types.h"
#include "ton/ton-shard.h"
#include "interfaces/validator-manager.h"

namespace ton {

namespace validator {

namespace dummy0 {

/*
 *
 * block data (if not given) can be obtained from:
 *   db as part of collated block
 *   db as block
 *   net
 * must write block data, block signatures and block state
 * initialize prev, before_split, after_merge
 * for masterchain write block proof and set next for prev block
 * for masterchain run new_block callback
 *
 */

class AcceptBlockQuery : public td::actor::Actor {
 public:
  AcceptBlockQuery(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev, CatchainSeqno catchain_seqno,
                   td::uint32 validator_set_hash, td::Ref<BlockSignatureSet> signatures, bool send_broadcast,
                   td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise);

  void abort_query(td::Status reason);
  void finish_query();
  void alarm() override;

  void start_up() override;
  void written_block_data();
  void written_block_signatures();
  void got_block_handle(BlockHandle handle);
  void written_block_info();
  void failed_to_get_block_candidate();
  void got_block_data(td::Ref<BlockData> data);
  void got_prev_state(td::Ref<ShardState> state);
  void written_state();
  void written_block_proof();
  void written_block_next();
  void written_block_info_2();
  void applied();
  void send_block_description();

 private:
  BlockIdExt id_;
  td::Ref<BlockData> data_;
  std::vector<BlockIdExt> prev_;
  CatchainSeqno catchain_seqno_;
  td::uint32 validator_set_hash_;
  td::Ref<BlockSignatureSet> signatures_;
  bool send_broadcast_;
  td::Timestamp timeout_ = td::Timestamp::in(600);
  td::actor::ActorId<ValidatorManager> manager_;
  td::Promise<td::Unit> promise_;

  FileHash signatures_hash_;
  BlockHandle handle_;
  td::Ref<Proof> proof_;
  td::Ref<ProofLink> proof_link_;

  td::Ref<ShardState> state_;
};

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
