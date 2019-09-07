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
#include "ton/interfaces/block-handle.h"
#include "validator/interfaces/validator-manager.h"

namespace ton {

namespace validator {

namespace dummy0 {

/*
 *
 * check block proof
 * write proof
 * initialize prev, before_split, after_merge
 * initialize prev's next
 *
 */

class CheckProofLink : public td::actor::Actor {
 public:
  CheckProofLink(BlockIdExt id, td::Ref<ProofLink> proof, td::actor::ActorId<ValidatorManager> manager,
                 td::Timestamp timeout, td::Promise<BlockHandle> promise)
      : id_(id), proof_(std::move(proof)), manager_(manager), timeout_(timeout), promise_(std::move(promise)) {
  }

  void abort_query(td::Status reason);
  void finish_query();
  void alarm() override;

  void start_up() override;
  void got_block_handle(BlockHandle handle);

 private:
  BlockIdExt id_;
  td::Ref<ProofLink> proof_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<BlockHandle> promise_;

  BlockHandle handle_;
  tl_object_ptr<ton_api::test0_proofLink> unserialized_proof_;
};

class CheckProof : public td::actor::Actor {
 public:
  CheckProof(BlockIdExt id, td::Ref<Proof> proof, td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
             td::Promise<BlockHandle> promise)
      : id_(id), proof_(std::move(proof)), manager_(manager), timeout_(timeout), promise_(std::move(promise)) {
  }

  void abort_query(td::Status reason);
  void finish_query();
  void alarm() override;

  void start_up() override;
  void got_block_handle(BlockHandle handle);
  void got_masterchain_state(td::Ref<MasterchainState> state);
  void set_next();

 private:
  BlockIdExt id_;
  td::Ref<Proof> proof_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<BlockHandle> promise_;

  BlockHandle handle_;
  td::Ref<MasterchainState> state_;
  tl_object_ptr<ton_api::test0_proof> unserialized_proof_;
};

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
