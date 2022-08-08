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
  if (handle->received()) {
    promise.set_value(td::Unit());
    return;
  }

  auto P = td::PromiseCreator::lambda(
      [id = archive_db_.get(), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          handle->set_received();
          td::actor::send_closure(id, &ArchiveManager::update_handle, std::move(handle), std::move(promise));
        }
      });

  td::actor::send_closure(archive_db_, &ArchiveManager::add_file, handle, fileref::Block{handle->id()}, block->data(),
                          std::move(P));
}

void RootDb::get_block_data(ConstBlockHandle handle, td::Promise<td::Ref<BlockData>> promise) {
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

    td::actor::send_closure(archive_db_, &ArchiveManager::get_file, handle, fileref::Block{handle->id()}, std::move(P));
  }
}

void RootDb::store_block_signatures(BlockHandle handle, td::Ref<BlockSignatureSet> data,
                                    td::Promise<td::Unit> promise) {
  if (handle->inited_signatures() || handle->moved_to_archive()) {
    promise.set_value(td::Unit());
    return;
  }

  auto P = td::PromiseCreator::lambda(
      [id = archive_db_.get(), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          handle->set_signatures();
          td::actor::send_closure(id, &ArchiveManager::update_handle, std::move(handle), std::move(promise));
        }
      });
  td::actor::send_closure(archive_db_, &ArchiveManager::add_temp_file_short, fileref::Signatures{handle->id()},
                          data->serialize(), std::move(P));
}

void RootDb::get_block_signatures(ConstBlockHandle handle, td::Promise<td::Ref<BlockSignatureSet>> promise) {
  if (!handle->inited_signatures() || handle->moved_to_archive()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in db"));
  } else {
    auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
      } else {
        promise.set_result(create_signature_set(R.move_as_ok()));
      }
    });
    td::actor::send_closure(archive_db_, &ArchiveManager::get_temp_file_short, fileref::Signatures{handle->id()},
                            std::move(P));
  }
}

void RootDb::store_block_proof(BlockHandle handle, td::Ref<Proof> proof, td::Promise<td::Unit> promise) {
  if (handle->inited_proof()) {
    promise.set_value(td::Unit());
    return;
  }

  auto P = td::PromiseCreator::lambda(
      [id = archive_db_.get(), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          handle->set_proof();
          td::actor::send_closure(id, &ArchiveManager::update_handle, std::move(handle), std::move(promise));
        }
      });

  td::actor::send_closure(archive_db_, &ArchiveManager::add_file, handle, fileref::Proof{handle->id()}, proof->data(),
                          std::move(P));
}

void RootDb::get_block_proof(ConstBlockHandle handle, td::Promise<td::Ref<Proof>> promise) {
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
    td::actor::send_closure(archive_db_, &ArchiveManager::get_file, handle, fileref::Proof{handle->id()}, std::move(P));
  }
}

void RootDb::store_block_proof_link(BlockHandle handle, td::Ref<ProofLink> proof, td::Promise<td::Unit> promise) {
  if (handle->inited_proof_link()) {
    promise.set_value(td::Unit());
    return;
  }

  auto P = td::PromiseCreator::lambda(
      [id = archive_db_.get(), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          handle->set_proof_link();
          td::actor::send_closure(id, &ArchiveManager::update_handle, std::move(handle), std::move(promise));
        }
      });
  td::actor::send_closure(archive_db_, &ArchiveManager::add_file, handle, fileref::ProofLink{handle->id()},
                          proof->data(), std::move(P));
}

void RootDb::get_block_proof_link(ConstBlockHandle handle, td::Promise<td::Ref<ProofLink>> promise) {
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
    td::actor::send_closure(archive_db_, &ArchiveManager::get_file, handle, fileref::ProofLink{handle->id()},
                            std::move(P));
  }
}

void RootDb::store_block_candidate(BlockCandidate candidate, td::Promise<td::Unit> promise) {
  auto obj = create_serialize_tl_object<ton_api::db_candidate>(
      PublicKey{pubkeys::Ed25519{candidate.pubkey.as_bits256()}}.tl(), create_tl_block_id(candidate.id),
      std::move(candidate.data), std::move(candidate.collated_data));

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_value(td::Unit());
    }
  });
  td::actor::send_closure(archive_db_, &ArchiveManager::add_temp_file_short,
                          fileref::Candidate{PublicKey{pubkeys::Ed25519{candidate.pubkey.as_bits256()}}, candidate.id,
                                             candidate.collated_file_hash},
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
  td::actor::send_closure(archive_db_, &ArchiveManager::get_temp_file_short,
                          fileref::Candidate{source, id, collated_data_file_hash}, std::move(P));
}

void RootDb::store_block_state(BlockHandle handle, td::Ref<ShardState> state,
                               td::Promise<td::Ref<ShardState>> promise) {
  if (handle->moved_to_archive()) {
    promise.set_value(std::move(state));
    return;
  }
  if (!handle->inited_state_boc()) {
    auto P = td::PromiseCreator::lambda([b = archive_db_.get(), root_hash = state->root_hash(), handle,
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

        td::actor::send_closure(b, &ArchiveManager::update_handle, std::move(handle), std::move(P));
      }
    });
    td::actor::send_closure(cell_db_, &CellDb::store_cell, handle->id(), state->root_cell(), std::move(P));
  } else {
    get_block_state(handle, std::move(promise));
  }
}

void RootDb::get_block_state(ConstBlockHandle handle, td::Promise<td::Ref<ShardState>> promise) {
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

void RootDb::get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) {
  td::actor::send_closure(cell_db_, &CellDb::get_cell_db_reader, std::move(promise));
}

void RootDb::store_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::BufferSlice state,
                                         td::Promise<td::Unit> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::add_persistent_state, block_id, masterchain_block_id,
                          std::move(state), std::move(promise));
}

void RootDb::store_persistent_state_file_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                             std::function<td::Status(td::FileFd&)> write_data,
                                             td::Promise<td::Unit> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::add_persistent_state_gen, block_id, masterchain_block_id,
                          std::move(write_data), std::move(promise));
}

void RootDb::get_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                       td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_persistent_state, block_id, masterchain_block_id,
                          std::move(promise));
}

void RootDb::get_persistent_state_file_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::int64 offset,
                                             td::int64 max_size, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_persistent_state_slice, block_id, masterchain_block_id,
                          offset, max_size, std::move(promise));
}

void RootDb::check_persistent_state_file_exists(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                td::Promise<bool> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::check_persistent_state, block_id, masterchain_block_id,
                          std::move(promise));
}

void RootDb::store_zero_state_file(BlockIdExt block_id, td::BufferSlice state, td::Promise<td::Unit> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::add_zero_state, block_id, std::move(state), std::move(promise));
}

void RootDb::get_zero_state_file(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_zero_state, block_id, std::move(promise));
}

void RootDb::check_zero_state_file_exists(BlockIdExt block_id, td::Promise<bool> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::check_zero_state, block_id, std::move(promise));
}

void RootDb::store_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::update_handle, std::move(handle), std::move(promise));
}

void RootDb::get_block_handle(BlockIdExt id, td::Promise<BlockHandle> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_handle, id, std::move(promise));
}

void RootDb::try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(static_files_db_, &StaticFilesDb::load_file, file_hash, std::move(promise));
}

void RootDb::apply_block(BlockHandle handle, td::Promise<td::Unit> promise) {
  td::actor::create_actor<BlockArchiver>("archiver", std::move(handle), archive_db_.get(), std::move(promise))
      .release();
}

void RootDb::get_block_by_lt(AccountIdPrefixFull account, LogicalTime lt, td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_block_by_lt, account, lt, std::move(promise));
}

void RootDb::get_block_by_unix_time(AccountIdPrefixFull account, UnixTime ts, td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_block_by_unix_time, account, ts, std::move(promise));
}

void RootDb::get_block_by_seqno(AccountIdPrefixFull account, BlockSeqno seqno, td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_block_by_seqno, account, seqno, std::move(promise));
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
  state_db_ = td::actor::create_actor<StateDb>("statedb", actor_id(this), root_path_ + "/state/");
  static_files_db_ = td::actor::create_actor<StaticFilesDb>("staticfilesdb", actor_id(this), root_path_ + "/static/");
  archive_db_ = td::actor::create_actor<ArchiveManager>("archive", actor_id(this), root_path_);
}

void RootDb::archive(BlockHandle handle, td::Promise<td::Unit> promise) {
  td::actor::create_actor<BlockArchiver>("archiveblock", std::move(handle), archive_db_.get(), std::move(promise))
      .release();
}

void RootDb::allow_state_gc(BlockIdExt block_id, td::Promise<bool> promise) {
  td::actor::send_closure(validator_manager_, &ValidatorManager::allow_block_state_gc, block_id, std::move(promise));
}

void RootDb::allow_block_gc(BlockIdExt block_id, td::Promise<bool> promise) {
  td::actor::send_closure(validator_manager_, &ValidatorManager::allow_block_info_gc, block_id, std::move(promise));
}

void RootDb::prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) {
  auto merger = StatsMerger::create(std::move(promise));
}

void RootDb::truncate(BlockSeqno seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) {
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  td::actor::send_closure(archive_db_, &ArchiveManager::truncate, seqno, handle, ig.get_promise());
  td::actor::send_closure(state_db_, &StateDb::truncate, seqno, handle, ig.get_promise());
}

void RootDb::add_key_block_proof(td::Ref<Proof> proof, td::Promise<td::Unit> promise) {
  auto i = proof->get_basic_header_info().move_as_ok();
  td::actor::send_closure(archive_db_, &ArchiveManager::add_key_block_proof, i.utime, proof->block_id().seqno(),
                          i.end_lt, fileref::Proof{proof->block_id()}, proof->data(), std::move(promise));
}

void RootDb::add_key_block_proof_link(td::Ref<ProofLink> proof, td::Promise<td::Unit> promise) {
  auto i = proof->get_basic_header_info().move_as_ok();
  td::actor::send_closure(archive_db_, &ArchiveManager::add_key_block_proof, i.utime, proof->block_id().seqno(),
                          i.end_lt, fileref::ProofLink{proof->block_id()}, proof->data(), std::move(promise));
}
void RootDb::get_key_block_proof(BlockIdExt block_id, td::Promise<td::Ref<Proof>> promise) {
  auto P = td::PromiseCreator::lambda([block_id, promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_result(create_proof(block_id, R.move_as_ok()));
    }
  });
  td::actor::send_closure(archive_db_, &ArchiveManager::get_key_block_proof, fileref::Proof{block_id}, std::move(P));
}
void RootDb::get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::Ref<ProofLink>> promise) {
  auto P = td::PromiseCreator::lambda([block_id, promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_result(create_proof_link(block_id, R.move_as_ok()));
    }
  });
  td::actor::send_closure(archive_db_, &ArchiveManager::get_key_block_proof, fileref::Proof{block_id}, std::move(P));
}

void RootDb::check_key_block_proof_exists(BlockIdExt block_id, td::Promise<bool> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_result(false);
    } else {
      promise.set_result(true);
    }
  });
  td::actor::send_closure(archive_db_, &ArchiveManager::get_key_block_proof, fileref::Proof{block_id}, std::move(P));
}
void RootDb::check_key_block_proof_link_exists(BlockIdExt block_id, td::Promise<bool> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_result(false);
    } else {
      promise.set_result(true);
    }
  });
  td::actor::send_closure(archive_db_, &ArchiveManager::get_key_block_proof, fileref::ProofLink{block_id},
                          std::move(P));
}

void RootDb::get_archive_id(BlockSeqno masterchain_seqno, td::Promise<td::uint64> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_archive_id, masterchain_seqno, std::move(promise));
}

void RootDb::get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                               td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_archive_slice, archive_id, offset, limit,
                          std::move(promise));
}

void RootDb::set_async_mode(bool mode, td::Promise<td::Unit> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::set_async_mode, mode, std::move(promise));
}

void RootDb::run_gc(UnixTime ts, UnixTime archive_ttl) {
  td::actor::send_closure(archive_db_, &ArchiveManager::run_gc, ts, archive_ttl);
}

}  // namespace validator

}  // namespace ton
