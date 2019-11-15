#include "import-db-slice.hpp"
#include "validator/db/fileref.hpp"
#include "td/utils/overloaded.h"
#include "validator/fabric.h"
#include "td/actor/MultiPromise.h"
#include "common/checksum.h"
#include "td/utils/port/path.h"

namespace ton {

namespace validator {

ArchiveImporter::ArchiveImporter(std::string path, td::Ref<MasterchainState> state, BlockSeqno shard_client_seqno,
                                 td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager,
                                 td::Promise<std::vector<BlockSeqno>> promise)
    : path_(std::move(path))
    , state_(std::move(state))
    , shard_client_seqno_(shard_client_seqno)
    , opts_(std::move(opts))
    , manager_(manager)
    , promise_(std::move(promise)) {
}

void ArchiveImporter::start_up() {
  auto R = Package::open(path_, false, false);
  if (R.is_error()) {
    abort_query(R.move_as_error());
    return;
  }
  package_ = std::make_shared<Package>(R.move_as_ok());

  bool fail = false;
  package_->iterate([&](std::string filename, td::BufferSlice data, td::uint64 offset) -> bool {
    auto F = FileReference::create(filename);
    if (F.is_error()) {
      abort_query(F.move_as_error());
      fail = true;
      return false;
    }
    auto f = F.move_as_ok();

    BlockIdExt b;
    bool is_proof = false;
    bool ignore = true;

    f.ref().visit(td::overloaded(
        [&](const fileref::Proof &p) {
          b = p.block_id;
          ignore = !b.is_masterchain();
          is_proof = true;
        },
        [&](const fileref::ProofLink &p) {
          b = p.block_id;
          ignore = b.is_masterchain();
          is_proof = true;
        },
        [&](const fileref::Block &p) {
          b = p.block_id;
          ignore = false;
          is_proof = false;
        },
        [&](const auto &p) { ignore = true; }));

    if (!ignore) {
      blocks_[b][is_proof ? 0 : 1] = offset;
      if (b.is_masterchain()) {
        masterchain_blocks_[b.seqno()] = b;
      }
    }
    return true;
  });

  if (fail) {
    return;
  }

  if (masterchain_blocks_.size() == 0) {
    abort_query(td::Status::Error(ErrorCode::notready, "archive does not contain any masterchain blocks"));
    return;
  }

  auto seqno = masterchain_blocks_.begin()->first;

  check_masterchain_block(seqno);
}

void ArchiveImporter::check_masterchain_block(BlockSeqno seqno) {
  auto it = masterchain_blocks_.find(seqno);
  if (it == masterchain_blocks_.end()) {
    if (seqno == 0) {
      abort_query(td::Status::Error(ErrorCode::notready, "no new blocks"));
      return;
    }
    checked_all_masterchain_blocks(seqno - 1);
    return;
  }
  if (seqno < state_->get_block_id().seqno()) {
    if (!state_->check_old_mc_block_id(it->second)) {
      abort_query(td::Status::Error(ErrorCode::protoviolation, "bad old masterchain block id"));
      return;
    }
    check_masterchain_block(seqno + 1);
  } else if (seqno == state_->get_block_id().seqno()) {
    if (state_->get_block_id() != it->second) {
      abort_query(td::Status::Error(ErrorCode::protoviolation, "bad old masterchain block id"));
      return;
    }
    check_masterchain_block(seqno + 1);
  } else {
    if (seqno != state_->get_block_id().seqno() + 1) {
      abort_query(td::Status::Error(ErrorCode::protoviolation, "hole in masterchain seqno"));
      return;
    }
    auto it2 = blocks_.find(it->second);
    CHECK(it2 != blocks_.end());

    auto R1 = package_->read(it2->second[0]);
    if (R1.is_error()) {
      abort_query(R1.move_as_error());
      return;
    }

    auto proofR = create_proof(it->second, std::move(R1.move_as_ok().second));
    if (proofR.is_error()) {
      abort_query(proofR.move_as_error());
      return;
    }

    auto R2 = package_->read(it2->second[1]);
    if (R2.is_error()) {
      abort_query(R2.move_as_error());
      return;
    }

    if (sha256_bits256(R2.ok().second.as_slice()) != it->second.file_hash) {
      abort_query(td::Status::Error(ErrorCode::protoviolation, "bad block file hash"));
      return;
    }
    auto dataR = create_block(it->second, std::move(R2.move_as_ok().second));
    if (dataR.is_error()) {
      abort_query(dataR.move_as_error());
      return;
    }

    auto proof = proofR.move_as_ok();
    auto data = dataR.move_as_ok();

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), id = state_->get_block_id(),
                                         data](td::Result<BlockHandle> R) mutable {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ArchiveImporter::abort_query, R.move_as_error());
        return;
      }
      auto handle = R.move_as_ok();
      CHECK(!handle->merge_before());
      if (handle->one_prev(true) != id) {
        td::actor::send_closure(SelfId, &ArchiveImporter::abort_query,
                                td::Status::Error(ErrorCode::protoviolation, "prev block mismatch"));
        return;
      }
      td::actor::send_closure(SelfId, &ArchiveImporter::checked_masterchain_proof, std::move(handle), std::move(data));
    });

    run_check_proof_query(it->second, std::move(proof), manager_, td::Timestamp::in(2.0), std::move(P), state_,
                          opts_->is_hardfork(it->second));
  }
}

void ArchiveImporter::checked_masterchain_proof(BlockHandle handle, td::Ref<BlockData> data) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ArchiveImporter::applied_masterchain_block, std::move(handle));
  });
  run_apply_block_query(handle->id(), std::move(data), handle->id(), manager_, td::Timestamp::in(10.0), std::move(P));
}

void ArchiveImporter::applied_masterchain_block(BlockHandle handle) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ArchiveImporter::got_new_materchain_state,
                            td::Ref<MasterchainState>(R.move_as_ok()));
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, handle, std::move(P));
}

void ArchiveImporter::got_new_materchain_state(td::Ref<MasterchainState> state) {
  state_ = std::move(state);
  check_masterchain_block(state_->get_block_id().seqno() + 1);
}

void ArchiveImporter::checked_all_masterchain_blocks(BlockSeqno seqno) {
  max_shard_client_seqno_ = seqno;
  check_next_shard_client_seqno(shard_client_seqno_ + 1);
}

void ArchiveImporter::check_next_shard_client_seqno(BlockSeqno seqno) {
  if (seqno > max_shard_client_seqno_) {
    finish_query();
    return;
  }

  if (seqno == max_shard_client_seqno_) {
    got_masterchain_state(state_);
  } else {
    BlockIdExt b;
    bool f = state_->get_old_mc_block_id(seqno, b);
    CHECK(f);
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &ArchiveImporter::got_masterchain_state,
                              td::Ref<MasterchainState>{R.move_as_ok()});
    });
    td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db_short, b, std::move(P));
  }
}

void ArchiveImporter::got_masterchain_state(td::Ref<MasterchainState> state) {
  auto s = state->get_shards();

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), seqno = state->get_block_id().seqno()](td::Result<td::Unit> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &ArchiveImporter::abort_query, R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &ArchiveImporter::check_next_shard_client_seqno, seqno + 1);
        }
      });

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(P));

  for (auto &shard : s) {
    apply_shard_block(shard->top_block_id(), state->get_block_id(), ig.get_promise());
  }
}

void ArchiveImporter::apply_shard_block(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                        td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), masterchain_block_id, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        R.ensure();
        td::actor::send_closure(SelfId, &ArchiveImporter::apply_shard_block_cont1, R.move_as_ok(), masterchain_block_id,
                                std::move(promise));
      });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id, true, std::move(P));
}

void ArchiveImporter::apply_shard_block_cont1(BlockHandle handle, BlockIdExt masterchain_block_id,
                                              td::Promise<td::Unit> promise) {
  if (handle->is_applied()) {
    promise.set_value(td::Unit());
    return;
  }

  auto it = blocks_.find(handle->id());
  if (it == blocks_.end()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "no proof for shard block"));
    return;
  }
  TRY_RESULT_PROMISE(promise, data, package_->read(it->second[0]));
  TRY_RESULT_PROMISE(promise, proof, create_proof_link(handle->id(), std::move(data.second)));
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle, masterchain_block_id,
                                       promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ArchiveImporter::apply_shard_block_cont2, std::move(handle),
                              masterchain_block_id, std::move(promise));
    }
  });
  run_check_proof_link_query(handle->id(), std::move(proof), manager_, td::Timestamp::in(10.0), std::move(P));
}

void ArchiveImporter::apply_shard_block_cont2(BlockHandle handle, BlockIdExt masterchain_block_id,
                                              td::Promise<td::Unit> promise) {
  if (handle->is_applied()) {
    promise.set_value(td::Unit());
    return;
  }
  CHECK(handle->id().seqno() > 0);

  if (!handle->merge_before() && handle->one_prev(true).shard_full() == handle->id().shard_full()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle, masterchain_block_id,
                                         promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &ArchiveImporter::apply_shard_block_cont3, std::move(handle),
                                masterchain_block_id, std::move(promise));
      }
    });
    apply_shard_block(handle->one_prev(true), masterchain_block_id, std::move(P));
  } else {
    td::MultiPromise mp;
    auto ig = mp.init_guard();
    ig.add_promise(std::move(promise));
    check_shard_block_applied(handle->one_prev(true), ig.get_promise());
    if (handle->merge_before()) {
      check_shard_block_applied(handle->one_prev(false), ig.get_promise());
    }
  }
}

void ArchiveImporter::apply_shard_block_cont3(BlockHandle handle, BlockIdExt masterchain_block_id,
                                              td::Promise<td::Unit> promise) {
  auto it = blocks_.find(handle->id());
  CHECK(it != blocks_.end());
  TRY_RESULT_PROMISE(promise, data, package_->read(it->second[1]));
  if (sha256_bits256(data.second.as_slice()) != handle->id().file_hash) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "bad block file hash"));
    return;
  }
  TRY_RESULT_PROMISE(promise, block, create_block(handle->id(), std::move(data.second)));

  run_apply_block_query(handle->id(), std::move(block), masterchain_block_id, manager_, td::Timestamp::in(10.0),
                        std::move(promise));
}

void ArchiveImporter::check_shard_block_applied(BlockIdExt block_id, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          if (!handle->is_applied()) {
            promise.set_error(td::Status::Error(ErrorCode::notready, "not applied"));
          } else {
            promise.set_value(td::Unit());
          }
        }
      });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id, false, std::move(P));
}

void ArchiveImporter::abort_query(td::Status error) {
  if (promise_) {
    promise_.set_error(std::move(error));
    td::unlink(path_).ensure();
  }
  stop();
}
void ArchiveImporter::finish_query() {
  if (promise_) {
    promise_.set_value(std::vector<BlockSeqno>(state_->get_block_id().seqno(), max_shard_client_seqno_));
    td::unlink(path_).ensure();
  }
  stop();
}

}  // namespace validator

}  // namespace ton
