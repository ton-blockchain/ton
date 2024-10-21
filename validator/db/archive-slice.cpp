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
#include "td/utils/port/path.h"
#include "common/delay.h"
#include "files-async.hpp"
#include "db-utils.h"

namespace ton {

namespace validator {

class PackageStatistics {
  public:
  void record_open(uint64_t count = 1) {
    open_count.fetch_add(count, std::memory_order_relaxed);
  }

  void record_close(uint64_t count = 1) {
    close_count.fetch_add(count, std::memory_order_relaxed);
  }

  void record_read(double time, uint64_t bytes) {
    read_bytes.fetch_add(bytes, std::memory_order_relaxed);
    std::lock_guard guard(read_mutex);
    read_time.insert(time);
  }

  void record_write(double time, uint64_t bytes) {
    write_bytes.fetch_add(bytes, std::memory_order_relaxed);
    std::lock_guard guard(write_mutex);
    write_time.insert(time);
  }

  std::string to_string_and_reset() {
    std::stringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(6);

    ss << "ton.pack.open COUNT : " << open_count.exchange(0, std::memory_order_relaxed) << "\n";
    ss << "ton.pack.close COUNT : " << close_count.exchange(0, std::memory_order_relaxed) << "\n";

    ss << "ton.pack.read.bytes COUNT : " << read_bytes.exchange(0, std::memory_order_relaxed) << "\n";
    ss << "ton.pack.write.bytes COUNT : " << write_bytes.exchange(0, std::memory_order_relaxed) << "\n";

    PercentileStats temp_read_time;
    {
      std::lock_guard guard(read_mutex);
      temp_read_time = std::move(read_time);
      read_time.clear();
    }
    ss << "ton.pack.read.micros " << temp_read_time.to_string() << "\n";

    PercentileStats temp_write_time;
    {
      std::lock_guard guard(write_mutex);
      temp_write_time = std::move(write_time);
      write_time.clear();
    }
    ss << "ton.pack.write.micros " << temp_write_time.to_string() << "\n";

    return ss.str();
  }

  private:
  std::atomic_uint64_t open_count{0};
  std::atomic_uint64_t close_count{0};
  PercentileStats read_time;
  std::atomic_uint64_t read_bytes{0};
  PercentileStats write_time;
  std::atomic_uint64_t write_bytes{0};

  mutable std::mutex read_mutex;
  mutable std::mutex write_mutex;
};

void DbStatistics::init() {
  rocksdb_statistics = td::RocksDb::create_statistics();
  pack_statistics = std::make_shared<PackageStatistics>();
}

std::string DbStatistics::to_string_and_reset() {
  std::stringstream ss;
  ss << td::RocksDb::statistics_to_string(rocksdb_statistics) << pack_statistics->to_string_and_reset();
  td::RocksDb::reset_statistics(rocksdb_statistics);
  return ss.str();
}

void PackageWriter::append(std::string filename, td::BufferSlice data,
                           td::Promise<std::pair<td::uint64, td::uint64>> promise) {
  td::uint64 offset, size;
  auto data_size = data.size();
  td::Timestamp start, end;
  {
    auto p = package_.lock();
    if (!p) {
      promise.set_error(td::Status::Error("Package is closed"));
      return;
    }
    start = td::Timestamp::now();
    offset = p->append(std::move(filename), std::move(data), !async_mode_);
    end = td::Timestamp::now();
    size = p->size();
  }
  if (statistics_) {
    statistics_->record_write((end.at() - start.at()) * 1e6, data_size);
  }
  promise.set_value(std::pair<td::uint64, td::uint64>{offset, size});
}

class PackageReader : public td::actor::Actor {
 public:
  PackageReader(std::shared_ptr<Package> package, td::uint64 offset,
                td::Promise<std::pair<std::string, td::BufferSlice>> promise, std::shared_ptr<PackageStatistics> statistics)
      : package_(std::move(package)), offset_(offset), promise_(std::move(promise)), statistics_(std::move(statistics)) {
  }
  void start_up() override {
    auto start = td::Timestamp::now();
    auto result = package_->read(offset_);
    if (statistics_ && result.is_ok()) {
      statistics_->record_read((td::Timestamp::now().at() - start.at()) * 1e6, result.ok_ref().second.size());
    }
    package_ = {};
    promise_.set_result(std::move(result));
    stop();
  }

 private:
  std::shared_ptr<Package> package_;
  td::uint64 offset_;
  td::Promise<std::pair<std::string, td::BufferSlice>> promise_;
  std::shared_ptr<PackageStatistics> statistics_;
};

static std::string get_package_file_name(PackageId p_id, ShardIdFull shard_prefix) {
  td::StringBuilder sb;
  sb << p_id.name();
  if (!shard_prefix.is_masterchain()) {
    sb << ".";
    sb << shard_prefix.workchain << ":" << shard_to_str(shard_prefix.shard);
  }
  sb << ".pack";
  return sb.as_cslice().str();
}

static std::string package_info_to_str(BlockSeqno seqno, ShardIdFull shard_prefix) {
  return PSTRING() << seqno << "." << shard_prefix.workchain << ":" << shard_to_str(shard_prefix.shard);
}

void ArchiveSlice::add_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  if (destroyed_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "package already gc'd"));
    return;
  }
  if (handle->id().seqno() == 0) {
    update_handle(std::move(handle), std::move(promise));
    return;
  }
  before_query();
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
  before_query();
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
  before_query();
  TRY_RESULT_PROMISE(
      promise, p,
      choose_package(
          handle ? handle->id().is_masterchain() ? handle->id().seqno() : handle->masterchain_ref_block() : 0,
          handle ? handle->id().shard_full() : ShardIdFull{masterchainId}, true));
  std::string value;
  auto R = kv_->get(ref_id.hash().to_hex(), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::Ok) {
    promise.set_value(td::Unit());
    return;
  }
  promise = begin_async_query(std::move(promise));
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
  before_query();
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
  before_query();
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
  before_query();
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
          handle ? handle->id().is_masterchain() ? handle->id().seqno() : handle->masterchain_ref_block() : 0,
          handle ? handle->id().shard_full() : ShardIdFull{masterchainId}, false));
  promise = begin_async_query(std::move(promise));
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise)](td::Result<std::pair<std::string, td::BufferSlice>> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(std::move(R.move_as_ok().second));
        }
      });
  td::actor::create_actor<PackageReader>("reader", p->package, offset, std::move(P), statistics_.pack_statistics).release();
}

void ArchiveSlice::get_block_common(AccountIdPrefixFull account_id,
                                    std::function<td::int32(ton_api::db_lt_desc_value &)> compare_desc,
                                    std::function<td::int32(ton_api::db_lt_el_value &)> compare, bool exact,
                                    td::Promise<ConstBlockHandle> promise) {
  if (destroyed_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "package already gc'd"));
    return;
  }
  before_query();
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
    if (compare_desc(*g) > 0) {
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
      int cmp_val = compare(*e);

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
      if (!block_id.is_valid() || block_id.id.seqno > rseq.id.seqno) {
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
  before_query();
  auto value = static_cast<td::uint32>(archive_id >> 32);
  PackageInfo *p;
  if (shard_split_depth_ == 0) {
    TRY_RESULT_PROMISE_ASSIGN(promise, p, choose_package(value, ShardIdFull{masterchainId}, false));
  } else {
    if (value >= packages_.size()) {
      promise.set_error(td::Status::Error(ErrorCode::notready, "no such package"));
      return;
    }
    p = &packages_[value];
  }
  promise = begin_async_query(std::move(promise));
  td::actor::create_actor<db::ReadFile>("readfile", p->path, offset, limit, 0, std::move(promise)).release();
}

void ArchiveSlice::get_archive_id(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix,
                                  td::Promise<td::uint64> promise) {
  before_query();
  if (!sliced_mode_) {
    promise.set_result(archive_id_);
  } else {
    TRY_RESULT_PROMISE(promise, p, choose_package(masterchain_seqno, shard_prefix, false));
    if (shard_split_depth_ == 0) {
      promise.set_result(p->seqno * (1ull << 32) + archive_id_);
    } else {
      promise.set_result(p->idx * (1ull << 32) + archive_id_);
    }
  }
}

void ArchiveSlice::before_query() {
  if (status_ == st_closed) {
    LOG(DEBUG) << "Opening archive slice " << db_path_;
    td::RocksDbOptions db_options;
    db_options.statistics = statistics_.rocksdb_statistics;
    kv_ = std::make_unique<td::RocksDb>(td::RocksDb::open(db_path_, std::move(db_options)).move_as_ok());
    std::string value;
    auto R2 = kv_->get("status", value);
    R2.ensure();
    sliced_mode_ = false;
    slice_size_ = 100;

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
        R2 = kv_->get("shard_split_depth", value);
        R2.ensure();
        if (R2.move_as_ok() == td::KeyValue::GetStatus::Ok) {
          shard_split_depth_ = td::to_integer<td::uint32>(value);
          CHECK(shard_split_depth_ <= 60);
        } else {
          shard_split_depth_ = 0;
        }
        for (td::uint32 i = 0; i < tot; i++) {
          R2 = kv_->get(PSTRING() << "status." << i, value);
          R2.ensure();
          CHECK(R2.move_as_ok() == td::KeyValue::GetStatus::Ok);
          auto len = td::to_integer<td::uint64>(value);
          R2 = kv_->get(PSTRING() << "version." << i, value);
          R2.ensure();
          td::uint32 ver = 0;
          if (R2.move_as_ok() == td::KeyValue::GetStatus::Ok) {
            ver = td::to_integer<td::uint32>(value);
          }
          td::uint32 seqno;
          ShardIdFull shard_prefix;
          if (shard_split_depth_ == 0) {
            seqno = archive_id_ + slice_size_ * i;
            shard_prefix = ShardIdFull{masterchainId};
          } else {
            R2 = kv_->get(PSTRING() << "info." << i, value);
            R2.ensure();
            CHECK(R2.move_as_ok() == td::KeyValue::GetStatus::Ok);
            unsigned long long shard;
            CHECK(sscanf(value.c_str(), "%u.%d:%016llx", &seqno, &shard_prefix.workchain, &shard) == 3);
            shard_prefix.shard = shard;
          }
          add_package(seqno, shard_prefix, len, ver);
        }
      } else {
        auto len = td::to_integer<td::uint64>(value);
        add_package(archive_id_, ShardIdFull{masterchainId}, len, 0);
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
        if (shard_split_depth_ > 0) {
          kv_->set("info.0", package_info_to_str(archive_id_, ShardIdFull{masterchainId})).ensure();
          kv_->set("shard_split_depth", td::to_string(shard_split_depth_)).ensure();
        }
        kv_->commit_transaction().ensure();
        add_package(archive_id_, ShardIdFull{masterchainId}, 0, default_package_version());
      } else {
        kv_->begin_transaction().ensure();
        kv_->set("status", "0").ensure();
        kv_->commit_transaction().ensure();
        add_package(archive_id_, ShardIdFull{masterchainId}, 0, 0);
      }
    }
  }
  status_ = st_open;
  if (!archive_lru_.empty()) {
    td::actor::send_closure(archive_lru_, &ArchiveLru::on_query, actor_id(this), p_id_,
                            packages_.size() + ESTIMATED_DB_OPEN_FILES);
  }
}

void ArchiveSlice::open_files() {
  before_query();
}

void ArchiveSlice::close_files() {
  if (status_ == st_open) {
    if (active_queries_ == 0) {
      do_close();
    } else {
      status_ = st_want_close;
    }
  }
}

void ArchiveSlice::do_close() {
  if (destroyed_) {
    return;
  }
  CHECK(status_ != st_closed && active_queries_ == 0);
  LOG(DEBUG) << "Closing archive slice " << db_path_;
  status_ = st_closed;
  kv_ = {};
  if (statistics_.pack_statistics) {
    statistics_.pack_statistics->record_close(packages_.size());
  }
  packages_.clear();
  id_to_package_.clear();
}

template<typename T>
td::Promise<T> ArchiveSlice::begin_async_query(td::Promise<T> promise) {
  ++active_queries_;
  return [SelfId = actor_id(this), promise = std::move(promise)](td::Result<T> R) mutable {
    td::actor::send_closure(SelfId, &ArchiveSlice::end_async_query);
    promise.set_result(std::move(R));
  };
}

void ArchiveSlice::end_async_query() {
  CHECK(active_queries_ > 0);
  --active_queries_;
  if (active_queries_ == 0 && status_ == st_want_close) {
    do_close();
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
  if (!async_mode_ && huge_transaction_started_ && kv_) {
    kv_->commit_transaction().ensure();
    huge_transaction_size_ = 0;
    huge_transaction_started_ = false;
  }

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  for (auto &p : packages_) {
    td::actor::send_closure(p.writer, &PackageWriter::set_async_mode, mode, ig.get_promise());
  }
}

ArchiveSlice::ArchiveSlice(td::uint32 archive_id, bool key_blocks_only, bool temp, bool finalized,
                           td::uint32 shard_split_depth, std::string db_root,
                           td::actor::ActorId<ArchiveLru> archive_lru, DbStatistics statistics)
    : archive_id_(archive_id)
    , key_blocks_only_(key_blocks_only)
    , temp_(temp)
    , finalized_(finalized)
    , p_id_(archive_id_, key_blocks_only_, temp_)
    , shard_split_depth_(temp || key_blocks_only ? 0 : shard_split_depth)
    , db_root_(std::move(db_root))
    , archive_lru_(std::move(archive_lru))
    , statistics_(statistics) {
  db_path_ = PSTRING() << db_root_ << p_id_.path() << p_id_.name() << ".index";
}

td::Result<ArchiveSlice::PackageInfo *> ArchiveSlice::choose_package(BlockSeqno masterchain_seqno,
                                                                     ShardIdFull shard_prefix, bool force) {
  if (temp_ || key_blocks_only_ || !sliced_mode_) {
    return &packages_[0];
  }
  if (masterchain_seqno < archive_id_) {
    return td::Status::Error(ErrorCode::notready, "too small masterchain seqno");
  }
  masterchain_seqno -= (masterchain_seqno - archive_id_) % slice_size_;
  CHECK((masterchain_seqno - archive_id_) % slice_size_ == 0);
  if (shard_split_depth_ == 0) {
    shard_prefix = ShardIdFull{masterchainId};
  } else if (!shard_prefix.is_masterchain()) {
    shard_prefix.shard |= 1;  // In case length is < split depth
    shard_prefix = ton::shard_prefix(shard_prefix, shard_split_depth_);
  }
  auto it = id_to_package_.find({masterchain_seqno, shard_prefix});
  if (it == id_to_package_.end()) {
    if (!force) {
      return td::Status::Error(ErrorCode::notready, "no such package");
    }
    begin_transaction();
    size_t v = packages_.size();
    kv_->set("slices", td::to_string(v + 1)).ensure();
    kv_->set(PSTRING() << "status." << v, "0").ensure();
    kv_->set(PSTRING() << "version." << v, td::to_string(default_package_version())).ensure();
    if (shard_split_depth_ > 0) {
      kv_->set(PSTRING() << "info." << v, package_info_to_str(masterchain_seqno, shard_prefix)).ensure();
    }
    commit_transaction();
    add_package(masterchain_seqno, shard_prefix, 0, default_package_version());
    return &packages_[v];
  } else {
    return &packages_[it->second];
  }
}

void ArchiveSlice::add_package(td::uint32 seqno, ShardIdFull shard_prefix, td::uint64 size, td::uint32 version) {
  PackageId p_id{seqno, key_blocks_only_, temp_};
  std::string path = PSTRING() << db_root_ << p_id.path() << get_package_file_name(p_id, shard_prefix);
  auto R = Package::open(path, false, true);
  if (R.is_error()) {
    LOG(FATAL) << "failed to open/create archive '" << path << "': " << R.move_as_error();
    return;
  }
  if (statistics_.pack_statistics) {
    statistics_.pack_statistics->record_open();
  }
  auto idx = td::narrow_cast<td::uint32>(packages_.size());
  id_to_package_[{seqno, shard_prefix}] = idx;
  if (finalized_) {
    packages_.emplace_back(nullptr, td::actor::ActorOwn<PackageWriter>(), seqno, shard_prefix, path, idx, version);
    return;
  }
  auto pack = std::make_shared<Package>(R.move_as_ok());
  if (version >= 1) {
    pack->truncate(size).ensure();
  }
  auto writer = td::actor::create_actor<PackageWriter>("writer", pack, async_mode_, statistics_.pack_statistics);
  packages_.emplace_back(std::move(pack), std::move(writer), seqno, shard_prefix, path, idx, version);
}

namespace {

void destroy_db(std::string name, td::uint32 attempt, td::Promise<td::Unit> promise) {
  auto S = td::RocksDb::destroy(name);
  if (S.is_ok()) {
    promise.set_value(td::Unit());
    return;
  }
  if (attempt > 0 && attempt % 64 == 0) {
    LOG(ERROR) << "failed to destroy index " << name << ": " << S;
  } else {
    LOG(DEBUG) << "failed to destroy index " << name << ": " << S;
  }
  delay_action(
      [name, attempt, promise = std::move(promise)]() mutable { destroy_db(name, attempt + 1, std::move(promise)); },
      td::Timestamp::in(1.0));
}
}  // namespace

void ArchiveSlice::destroy(td::Promise<td::Unit> promise) {
  before_query();
  destroyed_ = true;

  for (auto &p : packages_) {
    td::unlink(p.path).ensure();
  }
  if (statistics_.pack_statistics) {
    statistics_.pack_statistics->record_close(packages_.size());
  }
  packages_.clear();
  id_to_package_.clear();
  kv_ = nullptr;

  delay_action([name = db_path_, attempt = 0,
                promise = std::move(promise)]() mutable { destroy_db(name, attempt, std::move(promise)); },
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

bool ArchiveSlice::truncate_block(BlockSeqno masterchain_seqno, BlockIdExt block_id, td::uint32 cutoff_seqno,
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

  auto S = choose_package(seqno, block_id.shard_full(), false);
  S.ensure();
  auto p = S.move_as_ok();
  CHECK(p->seqno <= cutoff_seqno);
  if (p->seqno == cutoff_seqno) {
    move_handle(std::move(handle), p->package.get(), pack);
  }

  return true;
}

void ArchiveSlice::truncate_shard(BlockSeqno masterchain_seqno, ShardIdFull shard, td::uint32 cutoff_seqno,
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

    if (truncate_block(masterchain_seqno, create_block_id(e->id_), cutoff_seqno, pack)) {
      CHECK(new_last_idx == i);
      new_last_idx = i + 1;
    }
  }

  if (g->last_idx_ != new_last_idx) {
    g->last_idx_ = new_last_idx;
    kv_->set(key, serialize_tl_object(g, true)).ensure();
  }
}

void ArchiveSlice::truncate(BlockSeqno masterchain_seqno, ConstBlockHandle, td::Promise<td::Unit> promise) {
  if (temp_ || archive_id_ > masterchain_seqno) {
    destroy(std::move(promise));
    return;
  }
  before_query();
  LOG(INFO) << "TRUNCATE: slice " << archive_id_ << " maxseqno= " << max_masterchain_seqno()
            << " truncate_upto=" << masterchain_seqno;
  if (max_masterchain_seqno() <= masterchain_seqno) {
    promise.set_value(td::Unit());
    return;
  }

  std::map<ShardIdFull, PackageInfo*> old_packages;
  std::map<ShardIdFull, std::shared_ptr<Package>> new_packages;

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
    ShardIdFull shard{g->workchain_, static_cast<td::uint64>(g->shard_)};

    auto package_r = choose_package(masterchain_seqno, shard, false);
    if (package_r.is_error()) {
      continue;
    }
    auto package = package_r.move_as_ok();
    CHECK(package);
    if (!old_packages.count(package->shard_prefix)) {
      old_packages[package->shard_prefix] = package;
      auto new_package_r = Package::open(package->path + ".new", false, true);
      new_package_r.ensure();
      auto new_package = std::make_shared<Package>(new_package_r.move_as_ok());
      new_package->truncate(0).ensure();
      new_packages[package->shard_prefix] = std::move(new_package);
    }
    truncate_shard(masterchain_seqno, shard, package->seqno, new_packages[package->shard_prefix].get());
  }

  for (auto& [shard_prefix, package] : old_packages) {
    auto new_package = new_packages[shard_prefix];
    CHECK(new_package);
    package->package = new_package;
    package->writer.reset();
    td::unlink(package->path).ensure();
    td::rename(package->path + ".new", package->path).ensure();
    package->writer = td::actor::create_actor<PackageWriter>("writer", new_package, async_mode_);
  }

  std::vector<PackageInfo> new_packages_info;

  if (!sliced_mode_) {
    kv_->set("status", td::to_string(packages_.at(0).package->size())).ensure();
  } else {
    for (PackageInfo &package : packages_) {
      if (package.seqno <= masterchain_seqno) {
        new_packages_info.push_back(std::move(package));
      } else {
        td::unlink(package.path).ensure();
      }
    }
    id_to_package_.clear();
    for (td::uint32 i = 0; i < new_packages_info.size(); ++i) {
      PackageInfo &package = new_packages_info[i];
      package.idx = i;
      kv_->set(PSTRING() << "status." << i, td::to_string(package.package->size())).ensure();
      kv_->set(PSTRING() << "version." << i, td::to_string(package.version)).ensure();
      if (shard_split_depth_ > 0) {
        kv_->set(PSTRING() << "info." << i, package_info_to_str(package.seqno, package.shard_prefix)).ensure();
      }
      id_to_package_[{package.seqno, package.shard_prefix}] = i;
    }
    for (size_t i = new_packages_info.size(); i < packages_.size(); i++) {
      kv_->erase(PSTRING() << "status." << i);
      kv_->erase(PSTRING() << "version." << i);
      kv_->erase(PSTRING() << "info." << i);
    }
    kv_->set("slices", td::to_string(new_packages_info.size()));
    if (statistics_.pack_statistics) {
      statistics_.pack_statistics->record_close(packages_.size() - new_packages_info.size());
    }
    packages_ = std::move(new_packages_info);
  }

  kv_->commit_transaction().ensure();
  promise.set_value(td::Unit());
}

static std::tuple<td::uint32, bool, bool> to_tuple(const PackageId &id) {
  return {id.id, id.temp, id.key};
}

void ArchiveLru::on_query(td::actor::ActorId<ArchiveSlice> slice, PackageId id, size_t files_count) {
  SliceInfo &info = slices_[to_tuple(id)];
  if (info.opened_idx != 0) {
    total_files_ -= info.files_count;
    lru_.erase(info.opened_idx);
  }
  info.actor = std::move(slice);
  total_files_ += (info.files_count = files_count);
  info.opened_idx = current_idx_++;
  if (!info.is_permanent) {
    lru_.emplace(info.opened_idx, id);
  }
  enforce_limit();
}

void ArchiveLru::set_permanent_slices(std::vector<PackageId> ids) {
  for (auto id : permanent_slices_) {
    SliceInfo &info = slices_[to_tuple(id)];
    if (!info.is_permanent) {
      continue;
    }
    info.is_permanent = false;
    if (info.opened_idx) {
      lru_.emplace(info.opened_idx, id);
    }
  }
  permanent_slices_ = std::move(ids);
  for (auto id : permanent_slices_) {
    SliceInfo &info = slices_[to_tuple(id)];
    if (info.is_permanent) {
      continue;
    }
    info.is_permanent = true;
    if (info.opened_idx) {
      lru_.erase(info.opened_idx);
    }
  }
  enforce_limit();
}

void ArchiveLru::enforce_limit() {
  while (total_files_ > max_total_files_ && lru_.size() > 1) {
    auto it = lru_.begin();
    auto it2 = slices_.find(to_tuple(it->second));
    lru_.erase(it);
    total_files_ -= it2->second.files_count;
    td::actor::send_closure(it2->second.actor, &ArchiveSlice::close_files);
    it2->second.opened_idx = 0;
  }
}

}  // namespace validator

}  // namespace ton
