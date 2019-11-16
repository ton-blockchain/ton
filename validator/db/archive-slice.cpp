#include "archive-slice.hpp"

#include "td/actor/MultiPromise.h"
#include "validator/fabric.h"
#include "td/db/RocksDb.h"
#include "ton/ton-io.hpp"
#include "td/utils/port/path.h"
#include "common/delay.h"
#include "files-async.hpp"

namespace ton {

namespace validator {

void PackageWriter::append(std::string filename, td::BufferSlice data,
                           td::Promise<std::pair<td::uint64, td::uint64>> promise) {
  auto offset = package_->append(std::move(filename), std::move(data), !async_mode_);
  auto size = package_->size();

  promise.set_value(std::pair<td::uint64, td::uint64>{offset, size});
}

class PackageReader : public td::actor::Actor {
 public:
  PackageReader(std::shared_ptr<Package> package, td::uint64 offset,
                td::Promise<std::pair<std::string, td::BufferSlice>> promise)
      : package_(std::move(package)), offset_(offset), promise_(std::move(promise)) {
  }
  void start_up() {
    promise_.set_result(package_->read(offset_));
  }

 private:
  std::shared_ptr<Package> package_;
  td::uint64 offset_;
  td::Promise<std::pair<std::string, td::BufferSlice>> promise_;
};

void ArchiveSlice::add_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  if (destroyed_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "package already gc'd"));
    return;
  }
  if (handle->id().seqno() == 0) {
    update_handle(std::move(handle), std::move(promise));
    return;
  }
  CHECK(!key_blocks_only_);
  CHECK(!temp_);
  CHECK(handle->inited_unix_time());
  CHECK(handle->inited_logical_time());

  auto key = get_db_key_lt_desc(handle->id().shard_full());

  std::string value;
  auto R = kv_->get(key.as_slice(), value);
  R.ensure();
  tl_object_ptr<ton_api::db_lt_desc_value> v;
  bool add_shard = false;
  if (R.move_as_ok() == td::KeyValue::GetStatus::Ok) {
    auto F = fetch_tl_object<ton_api::db_lt_desc_value>(td::BufferSlice{value}, true);
    F.ensure();
    v = F.move_as_ok();
  } else {
    v = create_tl_object<ton_api::db_lt_desc_value>(1, 1, 0, 0, 0);
    add_shard = true;
  }
  if (handle->id().seqno() <= static_cast<td::uint32>(v->last_seqno_) ||
      handle->logical_time() <= static_cast<LogicalTime>(v->last_lt_) ||
      handle->unix_time() <= static_cast<UnixTime>(v->last_ts_)) {
    update_handle(std::move(handle), std::move(promise));
    return;
  }
  auto db_value = create_serialize_tl_object<ton_api::db_lt_el_value>(create_tl_block_id(handle->id()),
                                                                      handle->logical_time(), handle->unix_time());
  auto db_key = get_db_key_lt_el(handle->id().shard_full(), v->last_idx_++);
  auto status_key = create_serialize_tl_object<ton_api::db_lt_status_key>();
  v->last_seqno_ = handle->id().seqno();
  v->last_lt_ = handle->logical_time();
  v->last_ts_ = handle->unix_time();

  td::uint32 idx = 0;
  if (add_shard) {
    auto G = kv_->get(status_key.as_slice(), value);
    G.ensure();
    if (G.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
      idx = 0;
    } else {
      auto F = fetch_tl_object<ton_api::db_lt_status_value>(value, true);
      F.ensure();
      auto f = F.move_as_ok();
      idx = f->total_shards_;
    }
  }

  auto version = handle->version();

  begin_transaction();
  kv_->set(key, serialize_tl_object(v, true)).ensure();
  kv_->set(db_key, db_value.as_slice()).ensure();
  if (add_shard) {
    auto shard_key = create_serialize_tl_object<ton_api::db_lt_shard_key>(idx);
    auto shard_value =
        create_serialize_tl_object<ton_api::db_lt_shard_value>(handle->id().id.workchain, handle->id().id.shard);
    kv_->set(status_key.as_slice(), create_serialize_tl_object<ton_api::db_lt_status_value>(idx + 1)).ensure();
    kv_->set(shard_key.as_slice(), shard_value.as_slice()).ensure();
  }
  kv_->set(get_db_key_block_info(handle->id()), handle->serialize().as_slice()).ensure();
  commit_transaction();

  handle->flushed_upto(version);
  handle->set_handle_moved_to_archive();

  if (handle->need_flush()) {
    update_handle(std::move(handle), std::move(promise));
  } else {
    promise.set_value(td::Unit());
  }
}

void ArchiveSlice::update_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  if (destroyed_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "package already gc'd"));
    return;
  }
  if (!handle->need_flush() && (temp_ || handle->handle_moved_to_archive())) {
    promise.set_value(td::Unit());
    return;
  }
  CHECK(!key_blocks_only_);

  begin_transaction();
  do {
    auto version = handle->version();
    kv_->set(get_db_key_block_info(handle->id()), handle->serialize().as_slice()).ensure();
    handle->flushed_upto(version);
  } while (handle->need_flush());
  commit_transaction();
  if (!temp_) {
    handle->set_handle_moved_to_archive();
  }

  promise.set_value(td::Unit());
}

void ArchiveSlice::add_file(FileReference ref_id, td::BufferSlice data, td::Promise<td::Unit> promise) {
  if (destroyed_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "package already gc'd"));
    return;
  }
  std::string value;
  auto R = kv_->get(ref_id.hash().to_hex(), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::Ok) {
    promise.set_value(td::Unit());
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), ref_id, promise = std::move(promise)](
                                          td::Result<std::pair<td::uint64, td::uint64>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
      return;
    }
    auto v = R.move_as_ok();
    td::actor::send_closure(SelfId, &ArchiveSlice::add_file_cont, std::move(ref_id), v.first, v.second,
                            std::move(promise));
  });

  td::actor::send_closure(writer_, &PackageWriter::append, ref_id.filename(), std::move(data), std::move(P));
}

void ArchiveSlice::add_file_cont(FileReference ref_id, td::uint64 offset, td::uint64 size,
                                 td::Promise<td::Unit> promise) {
  if (destroyed_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "package already gc'd"));
    return;
  }
  begin_transaction();
  kv_->set("status", td::to_string(size)).ensure();
  kv_->set(ref_id.hash().to_hex(), td::to_string(offset)).ensure();
  commit_transaction();
  promise.set_value(td::Unit());
}

void ArchiveSlice::get_handle(BlockIdExt block_id, td::Promise<BlockHandle> promise) {
  if (destroyed_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "package already gc'd"));
    return;
  }
  CHECK(!key_blocks_only_);
  std::string value;
  auto R = kv_->get(get_db_key_block_info(block_id), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "handle not in archive slice"));
    return;
  }
  auto E = create_block_handle(td::BufferSlice{value});
  E.ensure();
  auto handle = E.move_as_ok();
  if (!temp_) {
    handle->set_handle_moved_to_archive();
  }
  promise.set_value(std::move(handle));
}

void ArchiveSlice::get_file(FileReference ref_id, td::Promise<td::BufferSlice> promise) {
  if (destroyed_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "package already gc'd"));
    return;
  }
  std::string value;
  auto R = kv_->get(ref_id.hash().to_hex(), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "file not in archive slice"));
    return;
  }
  auto offset = td::to_integer<td::uint64>(value);
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise)](td::Result<std::pair<std::string, td::BufferSlice>> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(std::move(R.move_as_ok().second));
        }
      });
  td::actor::create_actor<PackageReader>("reader", package_, offset, std::move(P)).release();
}

void ArchiveSlice::get_block_common(AccountIdPrefixFull account_id,
                                    std::function<td::int32(ton_api::db_lt_desc_value &)> compare_desc,
                                    std::function<td::int32(ton_api::db_lt_el_value &)> compare, bool exact,
                                    td::Promise<BlockHandle> promise) {
  if (destroyed_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "package already gc'd"));
    return;
  }
  bool f = false;
  BlockIdExt block_id;
  td::uint32 ls = 0;
  for (td::uint32 len = 0; len <= 60; len++) {
    auto s = shard_prefix(account_id, len);
    auto key = get_db_key_lt_desc(s);
    std::string value;
    auto F = kv_->get(key, value);
    F.ensure();
    if (F.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
      if (!f) {
        continue;
      } else {
        break;
      }
    }
    f = true;
    auto G = fetch_tl_object<ton_api::db_lt_desc_value>(td::BufferSlice{value}, true);
    G.ensure();
    auto g = G.move_as_ok();
    if (compare_desc(*g.get()) > 0) {
      continue;
    }
    td::uint32 l = g->first_idx_ - 1;
    BlockIdExt lseq;
    td::uint32 r = g->last_idx_;
    BlockIdExt rseq;
    while (r - l > 1) {
      auto x = (r + l) / 2;
      auto db_key = get_db_key_lt_el(s, x);
      F = kv_->get(db_key, value);
      F.ensure();
      CHECK(F.move_as_ok() == td::KeyValue::GetStatus::Ok);
      auto E = fetch_tl_object<ton_api::db_lt_el_value>(td::BufferSlice{value}, true);
      E.ensure();
      auto e = E.move_as_ok();
      int cmp_val = compare(*e.get());

      if (cmp_val < 0) {
        rseq = create_block_id(e->id_);
        r = x;
      } else if (cmp_val > 0) {
        lseq = create_block_id(e->id_);
        l = x;
      } else {
        get_handle(create_block_id(e->id_), std::move(promise));
        return;
      }
    }
    if (rseq.is_valid()) {
      if (!block_id.is_valid()) {
        block_id = rseq;
      } else if (block_id.id.seqno > rseq.id.seqno) {
        block_id = rseq;
      }
    }
    if (lseq.is_valid()) {
      if (ls < lseq.id.seqno) {
        ls = lseq.id.seqno;
      }
    }
    if (block_id.is_valid() && ls + 1 == block_id.id.seqno) {
      if (!exact) {
        get_handle(block_id, std::move(promise));
      } else {
        promise.set_error(td::Status::Error(ErrorCode::notready, "ltdb: block not found"));
      }
      return;
    }
  }
  if (!exact && block_id.is_valid()) {
    get_handle(block_id, std::move(promise));
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "ltdb: block not found"));
  }
}

void ArchiveSlice::get_block_by_lt(AccountIdPrefixFull account_id, LogicalTime lt, td::Promise<BlockHandle> promise) {
  return get_block_common(
      account_id,
      [lt](ton_api::db_lt_desc_value &w) {
        return lt > static_cast<LogicalTime>(w.last_lt_) ? 1 : lt == static_cast<LogicalTime>(w.last_lt_) ? 0 : -1;
      },
      [lt](ton_api::db_lt_el_value &w) {
        return lt > static_cast<LogicalTime>(w.lt_) ? 1 : lt == static_cast<LogicalTime>(w.lt_) ? 0 : -1;
      },
      false, std::move(promise));
}

void ArchiveSlice::get_block_by_seqno(AccountIdPrefixFull account_id, BlockSeqno seqno,
                                      td::Promise<BlockHandle> promise) {
  return get_block_common(
      account_id,
      [seqno](ton_api::db_lt_desc_value &w) {
        return seqno > static_cast<BlockSeqno>(w.last_seqno_)
                   ? 1
                   : seqno == static_cast<BlockSeqno>(w.last_seqno_) ? 0 : -1;
      },
      [seqno](ton_api::db_lt_el_value &w) {
        return seqno > static_cast<BlockSeqno>(w.id_->seqno_)
                   ? 1
                   : seqno == static_cast<BlockSeqno>(w.id_->seqno_) ? 0 : -1;
      },
      true, std::move(promise));
}

void ArchiveSlice::get_block_by_unix_time(AccountIdPrefixFull account_id, UnixTime ts,
                                          td::Promise<BlockHandle> promise) {
  return get_block_common(
      account_id,
      [ts](ton_api::db_lt_desc_value &w) {
        return ts > static_cast<UnixTime>(w.last_ts_) ? 1 : ts == static_cast<UnixTime>(w.last_ts_) ? 0 : -1;
      },
      [ts](ton_api::db_lt_el_value &w) {
        return ts > static_cast<UnixTime>(w.ts_) ? 1 : ts == static_cast<UnixTime>(w.ts_) ? 0 : -1;
      },
      false, std::move(promise));
}

td::BufferSlice ArchiveSlice::get_db_key_lt_desc(ShardIdFull shard) {
  return create_serialize_tl_object<ton_api::db_lt_desc_key>(shard.workchain, shard.shard);
}

td::BufferSlice ArchiveSlice::get_db_key_lt_el(ShardIdFull shard, td::uint32 idx) {
  return create_serialize_tl_object<ton_api::db_lt_el_key>(shard.workchain, shard.shard, idx);
}

td::BufferSlice ArchiveSlice::get_db_key_block_info(BlockIdExt block_id) {
  return create_serialize_tl_object<ton_api::db_blockdb_key_value>(create_tl_block_id(block_id));
}

void ArchiveSlice::get_slice(td::uint64 offset, td::uint32 limit, td::Promise<td::BufferSlice> promise) {
  td::actor::create_actor<db::ReadFile>("readfile", prefix_ + ".pack", offset, limit, 0, std::move(promise)).release();
}

void ArchiveSlice::start_up() {
  auto R = Package::open(prefix_ + ".pack", false, true);
  if (R.is_error()) {
    LOG(FATAL) << "failed to open/create archive '" << prefix_ << ".pack"
               << "': " << R.move_as_error();
    return;
  }
  package_ = std::make_shared<Package>(R.move_as_ok());
  kv_ = std::make_shared<td::RocksDb>(td::RocksDb::open(prefix_ + ".index").move_as_ok());

  std::string value;
  auto R2 = kv_->get("status", value);
  R2.ensure();

  if (R2.move_as_ok() == td::KeyValue::GetStatus::Ok) {
    auto len = td::to_integer<td::uint64>(value);
    package_->truncate(len);
  } else {
    package_->truncate(0);
  }

  writer_ = td::actor::create_actor<PackageWriter>("writer", package_);
}

void ArchiveSlice::begin_transaction() {
  if (!async_mode_ || !huge_transaction_started_) {
    kv_->begin_transaction().ensure();
    if (async_mode_) {
      huge_transaction_started_ = true;
    }
  }
}

void ArchiveSlice::commit_transaction() {
  if (!async_mode_ || huge_transaction_size_++ >= 100) {
    kv_->commit_transaction().ensure();
    if (async_mode_) {
      huge_transaction_size_ = 0;
      huge_transaction_started_ = false;
    }
  }
}

void ArchiveSlice::set_async_mode(bool mode, td::Promise<td::Unit> promise) {
  async_mode_ = mode;
  if (!async_mode_ && huge_transaction_started_) {
    kv_->commit_transaction().ensure();
    huge_transaction_size_ = 0;
    huge_transaction_started_ = false;
  }

  td::actor::send_closure(writer_, &PackageWriter::set_async_mode, mode, std::move(promise));
}

ArchiveSlice::ArchiveSlice(bool key_blocks_only, bool temp, std::string prefix)
    : key_blocks_only_(key_blocks_only), temp_(temp), prefix_(std::move(prefix)) {
}

namespace {

void destroy_db(std::string name, td::uint32 attempt, td::Promise<td::Unit> promise) {
  auto S = td::RocksDb::destroy(name);
  if (S.is_ok()) {
    promise.set_value(td::Unit());
    return;
  }
  if (S.is_error() && attempt > 0 && attempt % 64 == 0) {
    LOG(ERROR) << "failed to destroy index " << name << ": " << S;
  } else {
    LOG(DEBUG) << "failed to destroy index " << name << ": " << S;
  }
  delay_action(
      [name, attempt, promise = std::move(promise)]() mutable { destroy_db(name, attempt, std::move(promise)); },
      td::Timestamp::in(1.0));
}
}  // namespace

void ArchiveSlice::destroy(td::Promise<td::Unit> promise) {
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));
  destroyed_ = true;

  writer_.reset();
  package_ = nullptr;
  kv_ = nullptr;

  td::unlink(prefix_ + ".pack").ensure();

  delay_action([name = prefix_ + ".index", attempt = 0,
                promise = ig.get_promise()]() mutable { destroy_db(name, attempt, std::move(promise)); },
               td::Timestamp::in(0.0));
}

}  // namespace validator

}  // namespace ton
