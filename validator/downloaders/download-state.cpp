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
#include "common/checksum.h"
#include "common/delay.h"
#include "crypto/block/block-auto.h"
#include "crypto/block/block-parse.h"
#include "ton/ton-io.hpp"
#include "validator/fabric.h"
#include "vm/cells/MerkleProof.h"

#include "download-state.hpp"

namespace ton {

namespace validator {

class SplitStateDeserializer {
 public:
  td::Result<std::vector<SplitStatePart>> get_effective_shards_from_header(ShardId shard_id, RootHash root_hash,
                                                                           td::Ref<vm::Cell> wrapped_header,
                                                                           td::uint32 split_depth) {
    int shard_prefix_length = shard_pfx_len(shard_id);
    CHECK(split_depth <= 63 && shard_prefix_length < static_cast<int>(split_depth));

    try {
      TRY_RESULT(header, vm::MerkleProof::try_virtualize(wrapped_header));

      if (RootHash{header->get_hash().bits()} != root_hash) {
        return td::Status::Error("Hash mismatch in split state header");
      }

      auto shard_state_cs = vm::load_cell_slice(header);
      bool rc = block::gen::t_ShardStateUnsplit.unpack(shard_state_cs, shard_state_);
      if (!rc) {
        return td::Status::Error("Cannot deserialize ShardStateUnsplit");
      }

      vm::AugmentedDictionary accounts{
          vm::load_cell_slice_ref(shard_state_.accounts),
          256,
          block::tlb::aug_ShardAccounts,
          false,
      };

      std::vector<SplitStatePart> parts;

      // The following loop is the same as in state-serializer.cpp.
      ShardId effective_shard = shard_id ^ (1ULL << (63 - shard_prefix_length)) ^ (1ULL << (63 - split_depth));
      ShardId increment = 1ULL << (64 - split_depth);

      for (int i = 0; i < (1 << (split_depth - shard_prefix_length)); ++i, effective_shard += increment) {
        td::BitArray<64> prefix;
        prefix.store_ulong(effective_shard);
        auto account_dict_part = accounts;
        account_dict_part.cut_prefix_subdict(prefix.bits(), split_depth);

        if (!account_dict_part.is_empty()) {
          parts.push_back({effective_shard, account_dict_part.get_wrapped_dict_root()->get_hash()});
        }
      }

      // Now check that header does not contain pruned cells outside of accounts dict. For that, we
      // just replace account dict with an empty cell and see if header remains virtualized or not.
      shard_state_.accounts = vm::DataCell::create("", 0, {}, false).move_as_ok();

      vm::CellBuilder cb;
      block::gen::t_ShardStateUnsplit.pack(cb, shard_state_);
      if (cb.finalize()->is_virtualized()) {
        return td::Status::Error("State headers is pruned outside of account dict");
      }

      return parts;
    } catch (vm::VmVirtError const&) {
      return td::Status::Error("Insufficient number of cells in split state header");
    }
  }

  td::Ref<vm::Cell> merge(std::vector<td::Ref<vm::Cell>> const& parts) {
    vm::AugmentedDictionary accounts{256, block::tlb::aug_ShardAccounts};
    for (auto const& part_root : parts) {
      vm::AugmentedDictionary part{
          vm::load_cell_slice_ref(part_root),
          256,
          block::tlb::aug_ShardAccounts,
          false,
      };
      bool rc = accounts.combine_with(part);
      LOG_CHECK(rc) << "Split state parts have been validated but merging them still resulted in a conflict";
    }

    CHECK(accounts.is_valid());

    shard_state_.accounts = accounts.get_wrapped_dict_root();

    vm::CellBuilder cb;
    block::gen::t_ShardStateUnsplit.pack(cb, shard_state_);
    auto state_root = cb.finalize();
    CHECK(!state_root->is_virtualized());
    return state_root;
  }

 private:
  block::gen::ShardStateUnsplit::Record shard_state_;
};

DownloadShardState::DownloadShardState(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::uint32 split_depth,
                                       td::uint32 priority, td::actor::ActorId<ValidatorManager> manager,
                                       td::Timestamp timeout, td::Promise<td::Ref<ShardState>> promise)
    : block_id_(block_id)
    , masterchain_block_id_(masterchain_block_id)
    , split_depth_(split_depth)
    , priority_(priority)
    , manager_(manager)
    , timeout_(timeout)
    , promise_(std::move(promise)) {
  CHECK(masterchain_block_id_.is_valid() || split_depth_ == 0);

  int shard_prefix_length = shard_pfx_len(block_id_.shard_full().shard);
  if (shard_prefix_length >= static_cast<int>(split_depth_)) {
    split_depth_ = 0;
  }

  LOG(INFO) << "requested to download state of " << block_id.to_str() << " referenced by "
            << masterchain_block_id.to_str() << " with split depth " << split_depth;
}

DownloadShardState::~DownloadShardState() = default;

void DownloadShardState::start_up() {
  status_ = ProcessStatus(manager_, "process.download_state");
  alarm_timestamp() = timeout_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadShardState::got_block_handle, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id_, true, std::move(P));
}

void DownloadShardState::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);

  if (handle_->received_state()) {
    LOG(WARNING) << "shard state " << block_id_.to_str() << " already stored in db";
    td::actor::send_closure(manager_, &ValidatorManagerInterface::get_shard_state_from_db, handle_,
                            [SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
                              R.ensure();
                              td::actor::send_closure(SelfId, &DownloadShardState::written_shard_state, R.move_as_ok());
                            });
  } else {
    download_state();
  }
}

void DownloadShardState::retry() {
  deserializer_ = {};
  parts_.clear();
  download_state();
}

void DownloadShardState::download_state() {
  if (handle_->id().seqno() == 0 || handle_->inited_proof() || handle_->inited_proof_link()) {
    checked_proof_link();
    return;
  }
  status_.set_status(PSTRING() << block_id_.id.to_str() << " : downloading proof");

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), block_id = block_id_](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      LOG(DEBUG) << "Cannot get proof link from import: " << R.move_as_error();
      td::actor::send_closure(SelfId, &DownloadShardState::download_proof_link);
    } else {
      LOG(INFO) << "Got proof link for " << block_id.to_str() << " from import";
      td::actor::send_closure(SelfId, &DownloadShardState::downloaded_proof_link, R.move_as_ok());
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_proof_link_from_import, block_id_,
                          masterchain_block_id_, std::move(P));
}

void DownloadShardState::download_proof_link() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      fail_handler(SelfId, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadShardState::downloaded_proof_link, R.move_as_ok());
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::send_get_block_proof_link_request, block_id_, priority_,
                          std::move(P));
}

void DownloadShardState::downloaded_proof_link(td::BufferSlice data) {
  auto pp = create_proof_link(block_id_, std::move(data));
  if (pp.is_error()) {
    fail_handler(actor_id(this), pp.move_as_error());
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      fail_handler(SelfId, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadShardState::checked_proof_link);
    }
  });
  run_check_proof_link_query(block_id_, pp.move_as_ok(), manager_, td::Timestamp::in(60.0), std::move(P));
}

void DownloadShardState::checked_proof_link() {
  if (block_id_.seqno() == 0) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadShardState::download_zero_state);
      } else {
        td::actor::send_closure(SelfId, &DownloadShardState::downloaded_zero_state, R.move_as_ok());
      }
    });
    td::actor::send_closure(manager_, &ValidatorManager::try_get_static_file, block_id_.file_hash, std::move(P));
    status_.set_status(PSTRING() << block_id_.id.to_str() << " : downloading zero state");
  } else {
    CHECK(masterchain_block_id_.is_valid());
    CHECK(masterchain_block_id_.is_masterchain());

    if (split_depth_ == 0) {
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          fail_handler(SelfId, R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &DownloadShardState::downloaded_shard_state, R.move_as_ok());
        }
      });
      td::actor::send_closure(manager_, &ValidatorManager::send_get_persistent_state_request, block_id_,
                              masterchain_block_id_, UnsplitStateType{}, priority_, std::move(P));
      status_.set_status(PSTRING() << block_id_.id.to_str() << " : downloading state");
    } else {
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          fail_handler(SelfId, R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &DownloadShardState::downloaded_split_state_header, R.move_as_ok());
        }
      });
      td::actor::send_closure(manager_, &ValidatorManager::send_get_persistent_state_request, block_id_,
                              masterchain_block_id_, SplitPersistentStateType{}, priority_, std::move(P));
      status_.set_status(PSTRING() << block_id_.id.to_str() << " : downloading state header");
    }
  }
}

void DownloadShardState::download_zero_state() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      fail_handler(SelfId, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadShardState::downloaded_zero_state, R.move_as_ok());
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::send_get_zero_state_request, block_id_, priority_, std::move(P));
}

void DownloadShardState::downloaded_zero_state(td::BufferSlice data) {
  if (sha256_bits256(data.as_slice()) != block_id_.file_hash) {
    fail_handler(actor_id(this), td::Status::Error(ErrorCode::protoviolation, "bad zero state: file hash mismatch"));
    return;
  }

  data_ = std::move(data);
  auto S = create_shard_state(block_id_, data_.clone());
  S.ensure();
  state_ = S.move_as_ok();

  CHECK(state_->root_hash() == block_id_.root_hash);
  checked_shard_state();
}

void DownloadShardState::downloaded_shard_state(td::BufferSlice data) {
  status_.set_status(PSTRING() << block_id_.id.to_str() << " : processing downloaded state");
  auto S = create_shard_state(block_id_, data.clone());
  if (S.is_error()) {
    fail_handler(actor_id(this), S.move_as_error());
    return;
  }
  auto state = S.move_as_ok();
  if (state->root_hash() != handle_->state()) {
    fail_handler(actor_id(this),
                 td::Status::Error(ErrorCode::protoviolation, "bad persistent state: root hash mismatch"));
    return;
  }
  auto St = state->validate_deep();
  if (St.is_error()) {
    fail_handler(actor_id(this), St.move_as_error());
    return;
  }
  state_ = std::move(state);
  data_ = data.clone();
  checked_shard_state();
}

void DownloadShardState::checked_shard_state() {
  status_.set_status(PSTRING() << block_id_.id.to_str() << " : storing state file");
  LOG(WARNING) << "checked shard state " << block_id_.to_str();
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadShardState::written_shard_state_file);
  });
  if (block_id_.seqno() == 0) {
    td::actor::send_closure(manager_, &ValidatorManager::store_zero_state_file, block_id_, std::move(data_),
                            std::move(P));
  } else {
    td::actor::send_closure(manager_, &ValidatorManager::store_persistent_state_file, block_id_, masterchain_block_id_,
                            UnsplitStateType{}, std::move(data_), std::move(P));
  }
}

void DownloadShardState::downloaded_split_state_header(td::BufferSlice data) {
  LOG(INFO) << "processing state header";
  status_.set_status(PSTRING() << block_id_.id.to_str() << " : processing state header");

  deserializer_ = std::make_unique<SplitStateDeserializer>();

  auto maybe_header = vm::std_boc_deserialize(data);
  if (maybe_header.is_error()) {
    fail_handler(actor_id(this), maybe_header.move_as_error());
    return;
  }

  auto maybe_parts = deserializer_->get_effective_shards_from_header(block_id_.shard_full().shard, handle_->state(),
                                                                     maybe_header.move_as_ok(), split_depth_);
  if (maybe_parts.is_error()) {
    fail_handler(actor_id(this), maybe_parts.move_as_error());
    return;
  }

  parts_ = maybe_parts.move_as_ok();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadShardState::download_next_part_or_finish);
  });
  td::actor::send_closure(manager_, &ValidatorManager::store_persistent_state_file, block_id_, masterchain_block_id_,
                          SplitPersistentStateType{}, std::move(data), std::move(P));
}

namespace {

void retry_part_download(td::actor::ActorId<DownloadShardState> SelfId, td::Status error) {
  LOG(WARNING) << "failed to download state part : " << error;
  delay_action([=]() { td::actor::send_closure(SelfId, &DownloadShardState::download_next_part_or_finish); },
               td::Timestamp::in(1.0));
}

}  // namespace

void DownloadShardState::download_next_part_or_finish() {
  if (stored_parts_.size() == parts_.size()) {
    auto state_root = deserializer_->merge(stored_parts_);
    auto maybe_state = create_shard_state(block_id_, state_root);

    // We cannot rollback database changes here without significant elbow grease.
    maybe_state.ensure();
    state_ = maybe_state.move_as_ok();
    CHECK(state_->root_hash() == handle_->state());

    written_shard_state_file();
    return;
  }

  size_t idx = stored_parts_.size();

  LOG(INFO) << "downloading state part " << idx + 1 << " out of " << parts_.size();
  status_.set_status(PSTRING() << block_id_.id.to_str() << " : downloading state part (part " << idx + 1 << " out of "
                               << parts_.size() << ")");

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      retry_part_download(SelfId, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadShardState::downloaded_state_part, R.move_as_ok());
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::send_get_persistent_state_request, block_id_,
                          masterchain_block_id_, SplitAccountStateType{parts_[idx].effective_shard}, priority_,
                          std::move(P));
}

void DownloadShardState::downloaded_state_part(td::BufferSlice data) {
  size_t idx = stored_parts_.size();

  LOG(INFO) << "processing state part " << idx + 1 << " out of " << parts_.size();
  status_.set_status(PSTRING() << block_id_.id.to_str() << " : processing state part (part " << idx + 1 << " out of "
                               << parts_.size() << ")");

  auto maybe_part = vm::std_boc_deserialize(data);
  if (maybe_part.is_error()) {
    retry_part_download(actor_id(this), maybe_part.move_as_error());
    return;
  }

  auto root = maybe_part.move_as_ok();
  if (root->get_hash() != parts_[idx].root_hash) {
    auto error_message =
        "Hash mismatch for part " +
        persistent_state_type_to_string(block_id_.shard_full(), SplitAccountStateType{parts_[idx].effective_shard});
    retry_part_download(actor_id(this), td::Status::Error(error_message));
    return;
  }

  stored_parts_.push_back(root);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadShardState::written_state_part_file);
  });
  td::actor::send_closure(manager_, &ValidatorManager::store_persistent_state_file, block_id_, masterchain_block_id_,
                          SplitAccountStateType{parts_[idx].effective_shard}, std::move(data), std::move(P));

  LOG(INFO) << "storing state part to file " << idx + 1 << " out of " << parts_.size();
  status_.set_status(PSTRING() << block_id_.id.to_str() << " : storing state part to file (part " << idx + 1
                               << " out of " << parts_.size() << ")");
}

void DownloadShardState::written_state_part_file() {
  size_t idx = stored_parts_.size() - 1;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<vm::DataCell>> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadShardState::saved_state_part_into_celldb, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::store_block_state_part,
                          BlockId{block_id_.shard_full().workchain, parts_[idx].effective_shard, block_id_.seqno()},
                          stored_parts_.back(), std::move(P));
  LOG(INFO) << "saving to celldb state part " << idx + 1 << " out of " << parts_.size();
  status_.set_status(PSTRING() << block_id_.id.to_str() << " : saving state part to celldb (part " << idx + 1
                               << " out of " << parts_.size() << ")");
}

void DownloadShardState::saved_state_part_into_celldb(td::Ref<vm::DataCell> cell) {
  stored_parts_.back() = cell;
  download_next_part_or_finish();
}

void DownloadShardState::written_shard_state_file() {
  status_.set_status(PSTRING() << block_id_.id.to_str() << " : storing state to celldb");
  LOG(WARNING) << "written shard state file " << block_id_.to_str();
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadShardState::written_shard_state, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::set_block_state, handle_, std::move(state_), std::move(P));
}

void DownloadShardState::written_shard_state(td::Ref<ShardState> state) {
  status_.set_status(PSTRING() << block_id_.id.to_str() << " : finishing");
  state_ = std::move(state);
  handle_->set_unix_time(state_->get_unix_time());
  handle_->set_is_key_block(block_id_.is_masterchain());
  handle_->set_logical_time(state_->get_logical_time());
  handle_->set_applied();
  handle_->set_split(state_->before_split());
  if (!block_id_.is_masterchain()) {
    handle_->set_masterchain_ref_block(masterchain_block_id_.seqno());
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle = handle_](td::Result<td::Unit> R) {
    CHECK(handle->handle_moved_to_archive());
    CHECK(handle->moved_to_archive())
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadShardState::written_block_handle);
  });
  td::actor::send_closure(manager_, &ValidatorManager::archive, handle_, std::move(P));
}

void DownloadShardState::written_block_handle() {
  LOG(WARNING) << "finished downloading and storing shard state " << block_id_.to_str();
  finish_query();
}

void DownloadShardState::finish_query() {
  if (promise_) {
    promise_.set_value(std::move(state_));
  }
  stop();
}

void DownloadShardState::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void DownloadShardState::abort_query(td::Status reason) {
  if (promise_) {
    promise_.set_error(std::move(reason));
  }
  stop();
}

void DownloadShardState::fail_handler(td::actor::ActorId<DownloadShardState> SelfId, td::Status error) {
  LOG(WARNING) << "failed to download state : " << error;
  delay_action([=]() { td::actor::send_closure(SelfId, &DownloadShardState::retry); }, td::Timestamp::in(1.0));
}

}  // namespace validator

}  // namespace ton
