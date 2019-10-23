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
#include "rootdb.hpp"
#include "validator/fabric.h"
#include "archiver.hpp"

#include "td/db/RocksDb.h"
#include "ton/ton-tl.hpp"
#include "td/utils/overloaded.h"
#include "common/checksum.h"
#include "validator/stats-merger.h"
#include "td/actor/MultiPromise.h"

namespace ton {

namespace validator {

void RootDb::store_block_data(BlockHandle handle, td::Ref<BlockData> block, td::Promise<td::Unit> promise) {
  if (handle->moved_to_storage() || handle->moved_to_archive()) {
    promise.set_value(td::Unit());
    return;
  }
  auto id = block_db_.get();

  auto P = td::PromiseCreator::lambda([id, handle, promise = std::move(promise)](td::Result<FileHash> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      handle->set_received();
      td::actor::send_closure(id, &BlockDb::store_block_handle, std::move(handle), std::move(promise));
    }
  });

  td::actor::send_closure(file_db_, &FileDb::store_file, FileDb::RefId{fileref::Block{handle->id()}}, block->data(),
                          std::move(P));
}

void RootDb::get_block_data(BlockHandle handle, td::Promise<td::Ref<BlockData>> promise) {
  if (!handle->received()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in db"));
  } else {
    auto P = td::PromiseCreator::lambda(
        [id = handle->id(), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            promise.set_result(create_block(id, R.move_as_ok()));
          }
        });

    if (handle->moved_to_archive()) {
      td::actor::send_closure(new_archive_db_, &ArchiveManager::read, handle->unix_time(), handle->is_key_block(),
                              FileDb::RefId{fileref::Block{handle->id()}}, std::move(P));
    } else {
      td::actor::send_closure(handle->moved_to_storage() ? old_archive_db_.get() : file_db_.get(), &FileDb::load_file,
                              FileDb::RefId{fileref::Block{handle->id()}}, std::move(P));
    }
  }
}

void RootDb::store_block_signatures(BlockHandle handle, td::Ref<BlockSignatureSet> data,
                                    td::Promise<td::Unit> promise) {
  if (handle->moved_to_storage() || handle->moved_to_archive()) {
    promise.set_value(td::Unit());
    return;
  }
  auto id = block_db_.get();

  auto P = td::PromiseCreator::lambda([id, handle, promise = std::move(promise)](td::Result<FileHash> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      handle->set_signatures();
      td::actor::send_closure(id, &BlockDb::store_block_handle, std::move(handle), std::move(promise));
    }
  });

  td::actor::send_closure(file_db_, &FileDb::store_file, FileDb::RefId{fileref::Signatures{handle->id()}},
                          data->serialize(), std::move(P));
}

void RootDb::get_block_signatures(BlockHandle handle, td::Promise<td::Ref<BlockSignatureSet>> promise) {
  if (!handle->inited_signatures()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in db"));
  } else {
    if (handle->moved_to_storage() || handle->moved_to_archive()) {
      promise.set_error(td::Status::Error(ErrorCode::error, "signatures already gc'd"));
      return;
    }
    auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
      } else {
        promise.set_result(create_signature_set(R.move_as_ok()));
      }
    });
    td::actor::send_closure(file_db_, &FileDb::load_file, FileDb::RefId{fileref::Signatures{handle->id()}},
                            std::move(P));
  }
}

void RootDb::store_block_proof(BlockHandle handle, td::Ref<Proof> proof, td::Promise<td::Unit> promise) {
  if (handle->moved_to_storage() || handle->moved_to_archive()) {
    promise.set_value(td::Unit());
    return;
  }
  auto id = block_db_.get();

  auto P = td::PromiseCreator::lambda([id, handle, promise = std::move(promise)](td::Result<FileHash> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      handle->set_proof();
      td::actor::send_closure(id, &BlockDb::store_block_handle, std::move(handle), std::move(promise));
    }
  });

  td::actor::send_closure(file_db_, &FileDb::store_file, FileDb::RefId{fileref::Proof{handle->id()}}, proof->data(),
                          std::move(P));
}

void RootDb::get_block_proof(BlockHandle handle, td::Promise<td::Ref<Proof>> promise) {
  if (!handle->inited_proof()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in db"));
  } else {
    auto P = td::PromiseCreator::lambda(
        [id = handle->id(), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            promise.set_result(create_proof(id, R.move_as_ok()));
          }
        });
    if (handle->moved_to_archive()) {
      td::actor::send_closure(new_archive_db_, &ArchiveManager::read, handle->unix_time(), handle->is_key_block(),
                              FileDb::RefId{fileref::Proof{handle->id()}}, std::move(P));
    } else {
      td::actor::send_closure(handle->moved_to_storage() ? old_archive_db_.get() : file_db_.get(), &FileDb::load_file,
                              FileDb::RefId{fileref::Proof{handle->id()}}, std::move(P));
    }
  }
}

void RootDb::store_block_proof_link(BlockHandle handle, td::Ref<ProofLink> proof, td::Promise<td::Unit> promise) {
  if (handle->moved_to_storage() || handle->moved_to_archive()) {
    promise.set_value(td::Unit());
    return;
  }
  auto id = block_db_.get();

  auto P = td::PromiseCreator::lambda([id, handle, promise = std::move(promise)](td::Result<FileHash> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      handle->set_proof_link();
      td::actor::send_closure(id, &BlockDb::store_block_handle, std::move(handle), std::move(promise));
    }
  });

  td::actor::send_closure(file_db_, &FileDb::store_file, FileDb::RefId{fileref::ProofLink{handle->id()}}, proof->data(),
                          std::move(P));
}

void RootDb::get_block_proof_link(BlockHandle handle, td::Promise<td::Ref<ProofLink>> promise) {
  if (!handle->inited_proof_link()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in db"));
  } else {
    auto P = td::PromiseCreator::lambda(
        [id = handle->id(), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            promise.set_result(create_proof_link(id, R.move_as_ok()));
          }
        });
    if (handle->moved_to_archive()) {
      td::actor::send_closure(new_archive_db_, &ArchiveManager::read, handle->unix_time(), handle->is_key_block(),
                              FileDb::RefId{fileref::ProofLink{handle->id()}}, std::move(P));
    } else {
      td::actor::send_closure(handle->moved_to_storage() ? old_archive_db_.get() : file_db_.get(), &FileDb::load_file,
                              FileDb::RefId{fileref::ProofLink{handle->id()}}, std::move(P));
    }
  }
}

void RootDb::store_block_candidate(BlockCandidate candidate, td::Promise<td::Unit> promise) {
  auto obj = create_serialize_tl_object<ton_api::db_candidate>(
      PublicKey{pubkeys::Ed25519{candidate.pubkey.as_bits256()}}.tl(), create_tl_block_id(candidate.id),
      std::move(candidate.data), std::move(candidate.collated_data));

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<FileHash> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_value(td::Unit());
    }
  });
  td::actor::send_closure(file_db_, &FileDb::store_file,
                          FileDb::RefId{fileref::Candidate{PublicKey{pubkeys::Ed25519{candidate.pubkey.as_bits256()}},
                                                           candidate.id, candidate.collated_file_hash}},
                          std::move(obj), std::move(P));
}

void RootDb::get_block_candidate(PublicKey source, BlockIdExt id, FileHash collated_data_file_hash,
                                 td::Promise<BlockCandidate> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto f = fetch_tl_object<ton_api::db_candidate>(R.move_as_ok(), true);
      f.ensure();
      auto val = f.move_as_ok();
      auto hash = sha256_bits256(val->collated_data_);

      auto key = ton::PublicKey{val->source_};
      auto e_key = Ed25519_PublicKey{key.ed25519_value().raw()};
      promise.set_value(BlockCandidate{e_key, create_block_id(val->id_), hash, std::move(val->data_),
                                       std::move(val->collated_data_)});
    }
  });
  td::actor::send_closure(file_db_, &FileDb::load_file,
                          FileDb::RefId{fileref::Candidate{source, id, collated_data_file_hash}}, std::move(P));
}

void RootDb::store_block_state(BlockHandle handle, td::Ref<ShardState> state,
                               td::Promise<td::Ref<ShardState>> promise) {
  if (handle->moved_to_storage() || handle->moved_to_archive()) {
    promise.set_value(std::move(state));
    return;
  }
  if (!handle->inited_state_boc()) {
    auto P = td::PromiseCreator::lambda([b = block_db_.get(), root_hash = state->root_hash(), handle,
                                         promise = std::move(promise)](td::Result<td::Ref<vm::DataCell>> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
      } else {
        handle->set_state_root_hash(root_hash);
        handle->set_state_boc();

        auto S = create_shard_state(handle->id(), R.move_as_ok());
        S.ensure();

        auto P = td::PromiseCreator::lambda(
            [promise = std::move(promise), state = S.move_as_ok()](td::Result<td::Unit> R) mutable {
              R.ensure();
              promise.set_value(std::move(state));
            });

        td::actor::send_closure(b, &BlockDb::store_block_handle, std::move(handle), std::move(P));
      }
    });
    td::actor::send_closure(cell_db_, &CellDb::store_cell, handle->id(), state->root_cell(), std::move(P));
  } else {
    get_block_state(handle, std::move(promise));
  }
}

void RootDb::get_block_state(BlockHandle handle, td::Promise<td::Ref<ShardState>> promise) {
  if (handle->inited_state_boc()) {
    if (handle->deleted_state_boc()) {
      promise.set_error(td::Status::Error(ErrorCode::error, "state already gc'd"));
      return;
    }
    auto P =
        td::PromiseCreator::lambda([handle, promise = std::move(promise)](td::Result<td::Ref<vm::DataCell>> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            auto S = create_shard_state(handle->id(), R.move_as_ok());
            S.ensure();
            promise.set_value(S.move_as_ok());
          }
        });
    td::actor::send_closure(cell_db_, &CellDb::load_cell, handle->state(), std::move(P));
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "state not in db"));
  }
}

void RootDb::store_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::BufferSlice state,
                                         td::Promise<td::Unit> promise) {
  auto id = block_db_.get();

  auto P = td::PromiseCreator::lambda([id, promise = std::move(promise)](td::Result<FileHash> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_value(td::Unit());
    }
  });

  td::actor::send_closure(old_archive_db_, &FileDb::store_file,
                          FileDb::RefId{fileref::PersistentState{block_id, masterchain_block_id}}, std::move(state),
                          std::move(P));
}

void RootDb::get_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                       td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(old_archive_db_, &FileDb::load_file,
                          FileDb::RefId{fileref::PersistentState{block_id, masterchain_block_id}}, std::move(promise));
}

void RootDb::get_persistent_state_file_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::int64 offset,
                                             td::int64 max_size, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(old_archive_db_, &FileDb::load_file_slice,
                          FileDb::RefId{fileref::PersistentState{block_id, masterchain_block_id}}, offset, max_size,
                          std::move(promise));
}

void RootDb::check_persistent_state_file_exists(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                td::Promise<bool> promise) {
  td::actor::send_closure(old_archive_db_, &FileDb::check_file,
                          FileDb::RefId{fileref::PersistentState{block_id, masterchain_block_id}}, std::move(promise));
}

void RootDb::store_zero_state_file(BlockIdExt block_id, td::BufferSlice state, td::Promise<td::Unit> promise) {
  auto id = block_db_.get();

  auto P = td::PromiseCreator::lambda([id, promise = std::move(promise)](td::Result<FileHash> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_value(td::Unit());
    }
  });

  td::actor::send_closure(old_archive_db_, &FileDb::store_file, FileDb::RefId{fileref::ZeroState{block_id}},
                          std::move(state), std::move(P));
}

void RootDb::get_zero_state_file(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(old_archive_db_, &FileDb::load_file, FileDb::RefId{fileref::ZeroState{block_id}},
                          std::move(promise));
}

void RootDb::check_zero_state_file_exists(BlockIdExt block_id, td::Promise<bool> promise) {
  td::actor::send_closure(old_archive_db_, &FileDb::check_file, FileDb::RefId{fileref::ZeroState{block_id}},
                          std::move(promise));
}

void RootDb::store_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  if (handle->moved_to_archive()) {
    td::actor::send_closure(new_archive_db_, &ArchiveManager::write_handle, std::move(handle), std::move(promise));
  } else {
    td::actor::send_closure(block_db_, &BlockDb::store_block_handle, std::move(handle), std::move(promise));
  }
}

void RootDb::get_block_handle(BlockIdExt id, td::Promise<BlockHandle> promise) {
  auto P = td::PromiseCreator::lambda(
      [db = block_db_.get(), id, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          td::actor::send_closure(db, &BlockDb::get_block_handle, id, std::move(promise));
        } else {
          promise.set_value(R.move_as_ok());
        }
      });
  td::actor::send_closure(new_archive_db_, &ArchiveManager::read_handle, id, std::move(P));
}

void RootDb::try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(static_files_db_, &StaticFilesDb::load_file, file_hash, std::move(promise));
}

void RootDb::apply_block(BlockHandle handle, td::Promise<td::Unit> promise) {
  if (handle->id().id.seqno == 0) {
    promise.set_value(td::Unit());
  } else {
    td::actor::send_closure(lt_db_, &LtDb::add_new_block, handle->id(), handle->logical_time(), handle->unix_time(),
                            std::move(promise));
  }
}

void RootDb::get_block_by_lt(AccountIdPrefixFull account, LogicalTime lt, td::Promise<BlockIdExt> promise) {
  td::actor::send_closure(lt_db_, &LtDb::get_block_by_lt, account, lt, std::move(promise));
}

void RootDb::get_block_by_unix_time(AccountIdPrefixFull account, UnixTime ts, td::Promise<BlockIdExt> promise) {
  td::actor::send_closure(lt_db_, &LtDb::get_block_by_unix_time, account, ts, std::move(promise));
}

void RootDb::get_block_by_seqno(AccountIdPrefixFull account, BlockSeqno seqno, td::Promise<BlockIdExt> promise) {
  td::actor::send_closure(lt_db_, &LtDb::get_block_by_seqno, account, seqno, std::move(promise));
}

void RootDb::update_init_masterchain_block(BlockIdExt block, td::Promise<td::Unit> promise) {
  td::actor::send_closure(state_db_, &StateDb::update_init_masterchain_block, block, std::move(promise));
}

void RootDb::get_init_masterchain_block(td::Promise<BlockIdExt> promise) {
  td::actor::send_closure(state_db_, &StateDb::get_init_masterchain_block, std::move(promise));
}

void RootDb::update_gc_masterchain_block(BlockIdExt block, td::Promise<td::Unit> promise) {
  td::actor::send_closure(state_db_, &StateDb::update_gc_masterchain_block, block, std::move(promise));
}

void RootDb::get_gc_masterchain_block(td::Promise<BlockIdExt> promise) {
  td::actor::send_closure(state_db_, &StateDb::get_gc_masterchain_block, std::move(promise));
}

void RootDb::update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) {
  td::actor::send_closure(state_db_, &StateDb::update_shard_client_state, masterchain_block_id, std::move(promise));
}

void RootDb::get_shard_client_state(td::Promise<BlockIdExt> promise) {
  td::actor::send_closure(state_db_, &StateDb::get_shard_client_state, std::move(promise));
}

void RootDb::update_destroyed_validator_sessions(std::vector<ValidatorSessionId> sessions,
                                                 td::Promise<td::Unit> promise) {
  td::actor::send_closure(state_db_, &StateDb::update_destroyed_validator_sessions, std::move(sessions),
                          std::move(promise));
}

void RootDb::get_destroyed_validator_sessions(td::Promise<std::vector<ValidatorSessionId>> promise) {
  td::actor::send_closure(state_db_, &StateDb::get_destroyed_validator_sessions, std::move(promise));
}

void RootDb::update_async_serializer_state(AsyncSerializerState state, td::Promise<td::Unit> promise) {
  td::actor::send_closure(state_db_, &StateDb::update_async_serializer_state, std::move(state), std::move(promise));
}

void RootDb::get_async_serializer_state(td::Promise<AsyncSerializerState> promise) {
  td::actor::send_closure(state_db_, &StateDb::get_async_serializer_state, std::move(promise));
}

void RootDb::update_hardforks(std::vector<BlockIdExt> blocks, td::Promise<td::Unit> promise) {
  td::actor::send_closure(state_db_, &StateDb::update_hardforks, std::move(blocks), std::move(promise));
}

void RootDb::get_hardforks(td::Promise<std::vector<BlockIdExt>> promise) {
  td::actor::send_closure(state_db_, &StateDb::get_hardforks, std::move(promise));
}

void RootDb::start_up() {
  cell_db_ = td::actor::create_actor<CellDb>("celldb", actor_id(this), root_path_ + "/celldb/");
  block_db_ = td::actor::create_actor<BlockDb>("blockdb", actor_id(this), root_path_ + "/blockdb/");
  file_db_ = td::actor::create_actor<FileDb>("filedb", actor_id(this), root_path_ + "/files/", depth_, false);
  old_archive_db_ =
      td::actor::create_actor<FileDb>("filedbarchive", actor_id(this), root_path_ + "/archive/", depth_, true);
  lt_db_ = td::actor::create_actor<LtDb>("ltdb", actor_id(this), root_path_ + "/ltdb/");
  state_db_ = td::actor::create_actor<StateDb>("statedb", actor_id(this), root_path_ + "/state/");
  static_files_db_ = td::actor::create_actor<StaticFilesDb>("staticfilesdb", actor_id(this), root_path_ + "/static/");
  new_archive_db_ = td::actor::create_actor<ArchiveManager>("archivemanager", root_path_ + "/archive/");
}

void RootDb::archive(BlockIdExt block_id, td::Promise<td::Unit> promise) {
  td::actor::create_actor<BlockArchiver>("archiveblock", block_id, actor_id(this), file_db_.get(),
                                         old_archive_db_.get(), new_archive_db_.get(), std::move(promise))
      .release();
}

void RootDb::allow_state_gc(BlockIdExt block_id, td::Promise<bool> promise) {
  td::actor::send_closure(validator_manager_, &ValidatorManager::allow_block_state_gc, block_id, std::move(promise));
}

void RootDb::allow_block_gc(BlockIdExt block_id, td::Promise<bool> promise) {
  td::actor::send_closure(validator_manager_, &ValidatorManager::allow_block_info_gc, block_id, std::move(promise));
}

void RootDb::allow_gc(FileDb::RefId ref_id, bool is_archive, td::Promise<bool> promise) {
  ref_id.visit(
      td::overloaded([&](const fileref::Empty &key) { UNREACHABLE(); },
                     [&](const fileref::Block &key) {
                       td::actor::send_closure(validator_manager_, &ValidatorManager::allow_block_data_gc, key.block_id,
                                               is_archive, std::move(promise));
                     },
                     [&](const fileref::ZeroState &key) {
                       td::actor::send_closure(validator_manager_, &ValidatorManager::allow_zero_state_file_gc,
                                               key.block_id, std::move(promise));
                     },
                     [&](const fileref::PersistentState &key) {
                       CHECK(is_archive);
                       td::actor::send_closure(validator_manager_, &ValidatorManager::allow_persistent_state_file_gc,
                                               key.block_id, key.masterchain_block_id, std::move(promise));
                     },
                     [&](const fileref::Proof &key) {
                       td::actor::send_closure(validator_manager_, &ValidatorManager::allow_block_proof_gc,
                                               key.block_id, is_archive, std::move(promise));
                     },
                     [&](const fileref::ProofLink &key) {
                       td::actor::send_closure(validator_manager_, &ValidatorManager::allow_block_proof_link_gc,
                                               key.block_id, is_archive, std::move(promise));
                     },
                     [&](const fileref::Signatures &key) {
                       CHECK(!is_archive);
                       td::actor::send_closure(validator_manager_, &ValidatorManager::allow_block_signatures_gc,
                                               key.block_id, std::move(promise));
                     },
                     [&](const fileref::Candidate &key) {
                       CHECK(!is_archive);
                       td::actor::send_closure(validator_manager_, &ValidatorManager::allow_block_candidate_gc,
                                               key.block_id, std::move(promise));
                     },
                     [&](const fileref::BlockInfo &key) { UNREACHABLE(); }));
}

void RootDb::prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) {
  auto merger = StatsMerger::create(std::move(promise));

  td::actor::send_closure(file_db_, &FileDb::prepare_stats, merger.make_promise("filedb."));
  td::actor::send_closure(old_archive_db_, &FileDb::prepare_stats, merger.make_promise("archivedb."));
}

void RootDb::truncate(td::Ref<MasterchainState> state, td::Promise<td::Unit> promise) {
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  td::actor::send_closure(lt_db_, &LtDb::truncate, state, ig.get_promise());
  td::actor::send_closure(block_db_, &BlockDb::truncate, state, ig.get_promise());
}

}  // namespace validator

}  // namespace ton
