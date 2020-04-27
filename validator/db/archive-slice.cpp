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

    Copyright 2019-2020 Telegram Systems LLP
*/
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
    stop();
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

void ArchiveSlice::add_file(BlockHandle handle, FileReference ref_id, td::BufferSlice data,
                            td::Promise<td::Unit> promise) {
  if (destroyed_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "package already gc'd"));
    return;
  }
  TRY_RESULT_PROMISE(
      promise, p,
      choose_package(
          handle ? handle->id().is_masterchain() ? handle->id().seqno() : handle->masterchain_ref_block() : 0, true));
  std::string value;
  auto R = kv_->get(ref_id.hash().to_hex(), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::Ok) {
    promise.set_value(td::Unit());
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), idx = p->idx, ref_id, promise = std::move(promise)](
                                          td::Result<std::pair<td::uint64, td::uint64>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
      return;
    }
    auto v = R.move_as_ok();
    td::actor::send_closure(SelfId, &ArchiveSlice::add_file_cont, idx, std::move(ref_id), v.first, v.second,
                            std::move(promise));
  });

  td::actor::send_closure(p->writer, &PackageWriter::append, ref_id.filename(), std::move(data), std::move(P));
}

void ArchiveSlice::add_file_cont(size_t idx, FileReference ref_id, td::uint64 offset, td::uint64 size,
                                 td::Promise<td::Unit> promise) {
  if (destroyed_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "package already gc'd"));
    return;
  }
  begin_transaction();
  if (sliced_mode_) {
    kv_->set(PSTRING() << "status." << idx, td::to_string(size)).ensure();
    kv_->set(ref_id.hash().to_hex(), td::to_string(offset)).ensure();
  } else {
    CHECK(!idx);
    kv_->set("status", td::to_string(size)).ensure();
    kv_->set(ref_id.hash().to_hex(), td::to_string(offset)).ensure();
  }
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

void ArchiveSlice::get_temp_handle(BlockIdExt block_id, td::Promise<ConstBlockHandle> promise) {
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

void ArchiveSlice::get_file(ConstBlockHandle handle, FileReference ref_id, td::Promise<td::BufferSlice> promise) {
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
  TRY_RESULT_PROMISE(
      promise, p,
      choose_package(
          handle ? handle->id().is_masterchain() ? handle->id().seqno() : handle->masterchain_ref_block() : 0, false));
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise)](td::Result<std::pair<std::string, td::BufferSlice>> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(std::move(R.move_as_ok().second));
        }
      });
  td::actor::create_actor<PackageReader>("reader", p->package, offset, std::move(P)).release();
}

void ArchiveSlice::get_block_common(AccountIdPrefixFull account_id,
                                    std::function<td::int32(ton_api::db_lt_desc_value &)> compare_desc,
                                    std::function<td::int32(ton_api::db_lt_el_value &)> compare, bool exact,
                                    td::Promise<ConstBlockHandle> promise) {
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
    auto G = fetch_tl_object<ton_api::db_lt_desc_value>(value, true);
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
        get_temp_handle(create_block_id(e->id_), std::move(promise));
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
        get_temp_handle(block_id, std::move(promise));
      } else {
        promise.set_error(td::Status::Error(ErrorCode::notready, "ltdb: block not found"));
      }
      return;
    }
  }
  if (!exact && block_id.is_valid()) {
    get_temp_handle(block_id, std::move(promise));
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "ltdb: block not found"));
  }
}

void ArchiveSlice::get_block_by_lt(AccountIdPrefixFull account_id, LogicalTime lt,
                                   td::Promise<ConstBlockHandle> promise) {
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
                                      td::Promise<ConstBlockHandle> promise) {
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
                                          td::Promise<ConstBlockHandle> promise) {
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

void ArchiveSlice::get_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                             td::Promise<td::BufferSlice> promise) {
  if (static_cast<td::uint32>(archive_id) != archive_id_) {
    promise.set_error(td::Status::Error(ErrorCode::error, "bad archive id"));
    return;
  }
  auto value = static_cast<td::uint32>(archive_id >> 32);
  TRY_RESULT_PROMISE(promise, p, choose_package(value, false));
  td::actor::create_actor<db::ReadFile>("readfile", p->path, offset, limit, 0, std::move(promise)).release();
}

void ArchiveSlice::get_archive_id(BlockSeqno masterchain_seqno, td::Promise<td::uint64> promise) {
  if (!sliced_mode_) {
    promise.set_result(archive_id_);
  } else {
    TRY_RESULT_PROMISE(promise, p, choose_package(masterchain_seqno, false));
    promise.set_result(p->id * (1ull << 32) + archive_id_);
  }
}

void ArchiveSlice::start_up() {
  PackageId p_id{archive_id_, key_blocks_only_, temp_};
  std::string db_path = PSTRING() << db_root_ << p_id.path() << p_id.name() << ".index";
  kv_ = std::make_shared<td::RocksDb>(td::RocksDb::open(db_path).move_as_ok());

  std::string value;
  auto R2 = kv_->get("status", value);
  R2.ensure();

  if (R2.move_as_ok() == td::KeyValue::GetStatus::Ok) {
    if (value == "sliced") {
      sliced_mode_ = true;
      R2 = kv_->get("slices", value);
      R2.ensure();
      auto tot = td::to_integer<td::uint32>(value);
      R2 = kv_->get("slice_size", value);
      R2.ensure();
      slice_size_ = td::to_integer<td::uint32>(value);
      CHECK(slice_size_ > 0);
      for (td::uint32 i = 0; i < tot; i++) {
        R2 = kv_->get(PSTRING() << "status." << i, value);
        R2.ensure();
        auto len = td::to_integer<td::uint64>(value);
        R2 = kv_->get(PSTRING() << "version." << i, value);
        R2.ensure();
        td::uint32 ver = 0;
        if (R2.move_as_ok() == td::KeyValue::GetStatus::Ok) {
          ver = td::to_integer<td::uint32>(value);
        }
        auto v = archive_id_ + slice_size_ * i;
        add_package(v, len, ver);
      }
    } else {
      auto len = td::to_integer<td::uint64>(value);
      add_package(archive_id_, len, 0);
    }
  } else {
    if (!temp_ && !key_blocks_only_) {
      sliced_mode_ = true;
      kv_->begin_transaction().ensure();
      kv_->set("status", "sliced").ensure();
      kv_->set("slices", "1").ensure();
      kv_->set("slice_size", td::to_string(slice_size_)).ensure();
      kv_->set("status.0", "0").ensure();
      kv_->set("version.0", td::to_string(default_package_version())).ensure();
      kv_->commit_transaction().ensure();
      add_package(archive_id_, 0, default_package_version());
    } else {
      kv_->begin_transaction().ensure();
      kv_->set("status", "0").ensure();
      kv_->commit_transaction().ensure();
      add_package(archive_id_, 0, 0);
    }
  }
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

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  for (auto &p : packages_) {
    td::actor::send_closure(p.writer, &PackageWriter::set_async_mode, mode, std::move(promise));
  }
}

ArchiveSlice::ArchiveSlice(td::uint32 archive_id, bool key_blocks_only, bool temp, bool finalized, std::string db_root)
    : archive_id_(archive_id)
    , key_blocks_only_(key_blocks_only)
    , temp_(temp)
    , finalized_(finalized)
    , db_root_(std::move(db_root)) {
}

td::Result<ArchiveSlice::PackageInfo *> ArchiveSlice::choose_package(BlockSeqno masterchain_seqno, bool force) {
  if (temp_ || key_blocks_only_ || !sliced_mode_) {
    return &packages_[0];
  }
  if (masterchain_seqno < archive_id_) {
    return td::Status::Error(ErrorCode::notready, "too small masterchain seqno");
  }
  auto v = (masterchain_seqno - archive_id_) / slice_size_;
  if (v >= packages_.size()) {
    if (!force) {
      return td::Status::Error(ErrorCode::notready, "too big masterchain seqno");
    }
    CHECK(v == packages_.size());
    begin_transaction();
    kv_->set("slices", td::to_string(v + 1)).ensure();
    kv_->set(PSTRING() << "status." << v, "0").ensure();
    kv_->set(PSTRING() << "version." << v, td::to_string(default_package_version())).ensure();
    commit_transaction();
    CHECK((masterchain_seqno - archive_id_) % slice_size_ == 0);
    add_package(masterchain_seqno, 0, default_package_version());
    return &packages_[v];
  } else {
    return &packages_[v];
  }
}

void ArchiveSlice::add_package(td::uint32 seqno, td::uint64 size, td::uint32 version) {
  PackageId p_id{seqno, key_blocks_only_, temp_};
  std::string path = PSTRING() << db_root_ << p_id.path() << p_id.name() << ".pack";
  auto R = Package::open(path, false, true);
  if (R.is_error()) {
    LOG(FATAL) << "failed to open/create archive '" << path << "': " << R.move_as_error();
    return;
  }
  auto idx = td::narrow_cast<td::uint32>(packages_.size());
  if (finalized_) {
    packages_.emplace_back(nullptr, td::actor::ActorOwn<PackageWriter>(), seqno, path, idx, version);
    return;
  }
  auto pack = std::make_shared<Package>(R.move_as_ok());
  if (version >= 1) {
    pack->truncate(size).ensure();
  }
  auto writer = td::actor::create_actor<PackageWriter>("writer", pack);
  packages_.emplace_back(std::move(pack), std::move(writer), seqno, path, idx, version);
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

  for (auto &p : packages_) {
    td::unlink(p.path).ensure();
  }

  packages_.clear();
  kv_ = nullptr;

  PackageId p_id{archive_id_, key_blocks_only_, temp_};
  std::string db_path = PSTRING() << db_root_ << p_id.path() << p_id.name() << ".index";
  delay_action([name = db_path, attempt = 0,
                promise = ig.get_promise()]() mutable { destroy_db(name, attempt, std::move(promise)); },
               td::Timestamp::in(0.0));
}

BlockSeqno ArchiveSlice::max_masterchain_seqno() {
  auto key = get_db_key_lt_desc(ShardIdFull{masterchainId});
  std::string value;
  auto F = kv_->get(key, value);
  F.ensure();
  if (F.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    return 0;
  }
  auto G = fetch_tl_object<ton_api::db_lt_desc_value>(value, true);
  G.ensure();
  auto g = G.move_as_ok();
  if (g->first_idx_ == g->last_idx_) {
    return 0;
  }
  auto last_idx = g->last_idx_ - 1;
  auto db_key = get_db_key_lt_el(ShardIdFull{masterchainId}, last_idx);
  F = kv_->get(db_key, value);
  F.ensure();
  CHECK(F.move_as_ok() == td::KeyValue::GetStatus::Ok);
  auto E = fetch_tl_object<ton_api::db_lt_el_value>(td::BufferSlice{value}, true);
  E.ensure();
  auto e = E.move_as_ok();
  return e->id_->seqno_;
}

void ArchiveSlice::delete_file(FileReference ref_id) {
  std::string value;
  auto R = kv_->get(ref_id.hash().to_hex(), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    return;
  }
  kv_->erase(ref_id.hash().to_hex());
}

void ArchiveSlice::delete_handle(ConstBlockHandle handle) {
  delete_file(fileref::Proof{handle->id()});
  delete_file(fileref::ProofLink{handle->id()});
  delete_file(fileref::Block{handle->id()});
  kv_->erase(get_db_key_block_info(handle->id()));
}

void ArchiveSlice::move_file(FileReference ref_id, Package *old_pack, Package *pack) {
  LOG(DEBUG) << "moving " << ref_id.filename_short();
  std::string value;
  auto R = kv_->get(ref_id.hash().to_hex(), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    return;
  }
  auto offset = td::to_integer<td::uint64>(value);
  auto V = old_pack->read(offset);
  V.ensure();
  auto data = std::move(V.move_as_ok().second);
  auto r = pack->append(ref_id.filename(), std::move(data), false);
  kv_->set(ref_id.hash().to_hex(), td::to_string(r));
}

void ArchiveSlice::move_handle(ConstBlockHandle handle, Package *old_pack, Package *pack) {
  move_file(fileref::Proof{handle->id()}, old_pack, pack);
  move_file(fileref::ProofLink{handle->id()}, old_pack, pack);
  move_file(fileref::Block{handle->id()}, old_pack, pack);
}

bool ArchiveSlice::truncate_block(BlockSeqno masterchain_seqno, BlockIdExt block_id, td::uint32 cutoff_idx,
                                  Package *pack) {
  std::string value;
  auto R = kv_->get(get_db_key_block_info(block_id), value);
  R.ensure();
  CHECK(R.move_as_ok() == td::KeyValue::GetStatus::Ok);
  auto E = create_block_handle(value);
  E.ensure();
  auto handle = E.move_as_ok();
  auto seqno = handle->id().is_masterchain() ? handle->id().seqno() : handle->masterchain_ref_block();
  if (seqno > masterchain_seqno) {
    delete_handle(std::move(handle));
    return false;
  }

  auto S = choose_package(seqno, false);
  S.ensure();
  auto p = S.move_as_ok();
  CHECK(p->idx <= cutoff_idx);
  if (p->idx == cutoff_idx) {
    move_handle(std::move(handle), p->package.get(), pack);
  }

  return true;
}

void ArchiveSlice::truncate_shard(BlockSeqno masterchain_seqno, ShardIdFull shard, td::uint32 cutoff_idx,
                                  Package *pack) {
  auto key = get_db_key_lt_desc(shard);
  std::string value;
  auto F = kv_->get(key, value);
  F.ensure();
  if (F.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    return;
  }
  auto G = fetch_tl_object<ton_api::db_lt_desc_value>(value, true);
  G.ensure();
  auto g = G.move_as_ok();
  if (g->first_idx_ == g->last_idx_) {
    return;
  }

  int new_last_idx = g->first_idx_;
  for (int i = g->first_idx_; i < g->last_idx_; i++) {
    auto db_key = get_db_key_lt_el(shard, i);
    F = kv_->get(db_key, value);
    F.ensure();
    CHECK(F.move_as_ok() == td::KeyValue::GetStatus::Ok);
    auto E = fetch_tl_object<ton_api::db_lt_el_value>(value, true);
    E.ensure();
    auto e = E.move_as_ok();

    if (truncate_block(masterchain_seqno, create_block_id(e->id_), cutoff_idx, pack)) {
      CHECK(new_last_idx == i);
      new_last_idx = i + 1;
    }
  }

  if (g->last_idx_ != new_last_idx) {
    g->last_idx_ = new_last_idx;
    kv_->set(key, serialize_tl_object(g, true)).ensure();
  }
}

void ArchiveSlice::truncate(BlockSeqno masterchain_seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) {
  if (temp_ || archive_id_ > masterchain_seqno) {
    destroy(std::move(promise));
    return;
  }
  LOG(INFO) << "TRUNCATE: slice " << archive_id_ << " maxseqno= " << max_masterchain_seqno()
            << " truncate_upto=" << masterchain_seqno;
  if (max_masterchain_seqno() <= masterchain_seqno) {
    promise.set_value(td::Unit());
    return;
  }

  auto cutoff = choose_package(masterchain_seqno, false);
  cutoff.ensure();
  auto pack = cutoff.move_as_ok();
  CHECK(pack);

  auto pack_r = Package::open(pack->path + ".new", false, true);
  pack_r.ensure();
  auto new_package = std::make_shared<Package>(pack_r.move_as_ok());
  new_package->truncate(0).ensure();

  std::string value;
  auto status_key = create_serialize_tl_object<ton_api::db_lt_status_key>();
  auto R = kv_->get(status_key, value);
  R.ensure();

  auto F = fetch_tl_object<ton_api::db_lt_status_value>(value, true);
  F.ensure();
  auto f = F.move_as_ok();

  kv_->begin_transaction().ensure();
  for (int i = 0; i < f->total_shards_; i++) {
    auto shard_key = create_serialize_tl_object<ton_api::db_lt_shard_key>(i);
    R = kv_->get(shard_key, value);
    R.ensure();
    CHECK(R.move_as_ok() == td::KeyValue::GetStatus::Ok);

    auto G = fetch_tl_object<ton_api::db_lt_shard_value>(value, true);
    G.ensure();
    auto g = G.move_as_ok();

    truncate_shard(masterchain_seqno, ShardIdFull{g->workchain_, static_cast<td::uint64>(g->shard_)}, pack->idx,
                   new_package.get());
  }

  if (!sliced_mode_) {
    kv_->set("status", td::to_string(new_package->size())).ensure();
  } else {
    kv_->set(PSTRING() << "status." << pack->idx, td::to_string(new_package->size())).ensure();
    for (size_t i = pack->idx + 1; i < packages_.size(); i++) {
      kv_->erase(PSTRING() << "status." << i);
      kv_->erase(PSTRING() << "version." << i);
    }
    kv_->set("slices", td::to_string(pack->idx + 1));
  }

  pack->package = new_package;
  pack->writer.reset();
  td::unlink(pack->path).ensure();
  td::rename(pack->path + ".new", pack->path).ensure();
  pack->writer = td::actor::create_actor<PackageWriter>("writer", new_package);

  for (auto idx = pack->idx + 1; idx < packages_.size(); idx++) {
    td::unlink(packages_[idx].path).ensure();
  }
  packages_.erase(packages_.begin() + pack->idx + 1);

  kv_->commit_transaction().ensure();

  promise.set_value(td::Unit());
}

}  // namespace validator

}  // namespace ton
