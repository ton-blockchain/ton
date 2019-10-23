#include "archive-db.hpp"
#include "common/errorcode.h"

#include "common/int-to-string.hpp"
#include "files-async.hpp"

#include "td/db/RocksDb.h"
#include "validator/fabric.h"

namespace ton {

namespace validator {

void PackageWriter::append(std::string filename, td::BufferSlice data,
                           td::Promise<std::pair<td::uint64, td::uint64>> promise) {
  auto offset = package_->append(std::move(filename), std::move(data));
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

void ArchiveFile::start_up() {
  auto R = Package::open(path_, false, true);
  if (R.is_error()) {
    LOG(FATAL) << "failed to open/create archive '" << path_ << "': " << R.move_as_error();
    return;
  }
  package_ = std::make_shared<Package>(R.move_as_ok());
  index_ = std::make_shared<td::RocksDb>(td::RocksDb::open(path_ + ".index").move_as_ok());

  std::string value;
  auto R2 = index_->get("status", value);
  R2.ensure();

  if (R2.move_as_ok() == td::KeyValue::GetStatus::Ok) {
    auto len = td::to_integer<td::uint64>(value);
    package_->truncate(len);
  } else {
    package_->truncate(0);
  }

  writer_ = td::actor::create_actor<PackageWriter>("writer", package_);
}

void ArchiveFile::write(FileDb::RefId ref_id, td::BufferSlice data, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), ref_id, promise = std::move(promise)](
                                          td::Result<std::pair<td::uint64, td::uint64>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
      return;
    }
    auto v = R.move_as_ok();
    td::actor::send_closure(SelfId, &ArchiveFile::completed_write, std::move(ref_id), v.first, v.second,
                            std::move(promise));
  });

  FileHash hash;
  ref_id.visit([&](const auto &obj) { hash = obj.hash(); });

  td::actor::send_closure(writer_, &PackageWriter::append, hash.to_hex(), std::move(data), std::move(P));
}

void ArchiveFile::write_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  FileHash hash = fileref::BlockInfo{handle->id()}.hash();
  index_->begin_transaction().ensure();
  do {
    auto version = handle->version();
    index_->set(hash.to_hex(), handle->serialize().as_slice().str()).ensure();
    handle->flushed_upto(version);
  } while (handle->need_flush());
  index_->commit_transaction().ensure();
  promise.set_value(td::Unit());
}

void ArchiveFile::completed_write(FileDb::RefId ref_id, td::uint64 offset, td::uint64 new_size,
                                  td::Promise<td::Unit> promise) {
  FileHash hash;
  ref_id.visit([&](const auto &obj) { hash = obj.hash(); });
  index_->begin_transaction().ensure();
  index_->set("status", td::to_string(new_size)).ensure();
  index_->set(hash.to_hex(), td::to_string(offset)).ensure();
  index_->commit_transaction().ensure();
  promise.set_value(td::Unit());
}

void ArchiveFile::read(FileDb::RefId ref_id, td::Promise<td::BufferSlice> promise) {
  FileHash hash;
  ref_id.visit([&](const auto &obj) { hash = obj.hash(); });
  std::string value;
  auto R = index_->get(hash.to_hex(), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in db (archive)"));
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

void ArchiveFile::read_handle(BlockIdExt block_id, td::Promise<BlockHandle> promise) {
  FileHash hash = fileref::BlockInfo{block_id}.hash();

  std::string value;
  auto R = index_->get(hash.to_hex(), value);
  R.ensure();

  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in archive db"));
    return;
  }
  promise.set_result(create_block_handle(td::BufferSlice{value}));
}

ArchiveManager::ArchiveManager(std::string db_root) : db_root_(db_root) {
}

void ArchiveManager::write(UnixTime ts, bool key_block, FileDb::RefId ref_id, td::BufferSlice data,
                           td::Promise<td::Unit> promise) {
  auto f = get_file(ts, key_block);
  td::actor::send_closure(f->file_actor_id(), &ArchiveFile::write, std::move(ref_id), std::move(data),
                          std::move(promise));
}

void ArchiveManager::write_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  auto f = get_file(handle->unix_time(), handle->is_key_block());
  update_desc(*f, handle->id().shard_full(), handle->id().seqno(), handle->logical_time());
  td::actor::send_closure(f->file_actor_id(), &ArchiveFile::write_handle, std::move(handle), std::move(promise));
}

void ArchiveManager::read(UnixTime ts, bool key_block, FileDb::RefId ref_id, td::Promise<td::BufferSlice> promise) {
  auto f = get_file(ts, key_block);
  td::actor::send_closure(f->file_actor_id(), &ArchiveFile::read, std::move(ref_id), std::move(promise));
}

void ArchiveManager::read_handle(BlockIdExt block_id, td::Promise<BlockHandle> promise) {
  if (block_id.is_masterchain()) {
    auto f = get_file_by_seqno(block_id.shard_full(), block_id.seqno(), true);
    if (!f) {
      read_handle_cont(block_id, std::move(promise));
      return;
    }
    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), block_id, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &ArchiveManager::read_handle_cont, block_id, std::move(promise));
          } else {
            promise.set_value(R.move_as_ok());
          }
        });
    td::actor::send_closure(f->file_actor_id(), &ArchiveFile::read_handle, block_id, std::move(P));
  } else {
    read_handle_cont(block_id, std::move(promise));
  }
}

void ArchiveManager::read_handle_cont(BlockIdExt block_id, td::Promise<BlockHandle> promise) {
  auto f = get_file_by_seqno(block_id.shard_full(), block_id.seqno(), false);
  if (!f) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in archive db"));
    return;
  }
  td::actor::send_closure(f->file_actor_id(), &ArchiveFile::read_handle, block_id, std::move(promise));
}

UnixTime ArchiveManager::convert_ts(UnixTime ts, bool key_block) {
  if (!key_block) {
    return ts - (ts % (1 << 17));
  } else {
    return ts - (ts % (1 << 22));
  }
}

ArchiveManager::FileDescription *ArchiveManager::get_file(UnixTime ts, bool key_block, bool force) {
  ts = convert_ts(ts, key_block);
  auto &f = key_block ? key_files_ : files_;
  auto it = f.find(ts);
  if (it != f.end()) {
    return &it->second;
  }
  if (!force) {
    return nullptr;
  }

  return add_file(ts, key_block);
}

ArchiveManager::FileDescription *ArchiveManager::add_file(UnixTime ts, bool key_block) {
  CHECK((key_block ? key_files_ : files_).count(ts) == 0);
  index_->begin_transaction().ensure();
  {
    std::vector<td::int32> t;
    std::vector<td::int32> tk;
    for (auto &e : files_) {
      t.push_back(e.first);
    }
    for (auto &e : key_files_) {
      tk.push_back(e.first);
    }
    (key_block ? tk : t).push_back(ts);
    index_
        ->set(create_serialize_tl_object<ton_api::db_archive_index_key>().as_slice(),
              create_serialize_tl_object<ton_api::db_archive_index_value>(std::move(t), std::move(tk)).as_slice())
        .ensure();
  }
  {
    index_
        ->set(create_serialize_tl_object<ton_api::db_archive_package_key>(ts, key_block).as_slice(),
              create_serialize_tl_object<ton_api::db_archive_package_value>(
                  ts, key_block, std::vector<tl_object_ptr<ton_api::db_archive_package_firstBlock>>(), false)
                  .as_slice())
        .ensure();
  }
  index_->commit_transaction().ensure();
  FileDescription desc{ts, key_block};
  auto w = td::actor::create_actor<ArchiveFile>(
      PSTRING() << "archivefile" << ts,
      PSTRING() << db_root_ << "/packed/" << (key_block ? "key" : "") << ts << ".pack", ts);
  desc.file = std::move(w);

  return &(key_block ? key_files_ : files_).emplace(ts, std::move(desc)).first->second;
}

void ArchiveManager::load_package(UnixTime ts, bool key_block) {
  auto key = create_serialize_tl_object<ton_api::db_archive_package_key>(ts, key_block);

  std::string value;
  auto v = index_->get(key.as_slice(), value);
  v.ensure();
  CHECK(v.move_as_ok() == td::KeyValue::GetStatus::Ok);

  auto R = fetch_tl_object<ton_api::db_archive_package_value>(value, true);
  R.ensure();
  auto x = R.move_as_ok();

  if (x->deleted_) {
    return;
  }

  FileDescription desc{ts, key_block};
  for (auto &e : x->firstblocks_) {
    desc.first_blocks[ShardIdFull{e->workchain_, static_cast<ShardId>(e->shard_)}] =
        FileDescription::Desc{static_cast<td::uint32>(e->seqno_), static_cast<LogicalTime>(e->lt_)};
  }
  desc.file = td::actor::create_actor<ArchiveFile>(
      PSTRING() << "archivefile" << ts,
      PSTRING() << db_root_ << "/packed/" << (key_block ? "key" : "") << ts << ".pack", ts);

  (key_block ? key_files_ : files_).emplace(ts, std::move(desc));
}

void ArchiveManager::update_desc(FileDescription &desc, ShardIdFull shard, BlockSeqno seqno, LogicalTime lt) {
  auto it = desc.first_blocks.find(shard);
  if (it != desc.first_blocks.end() && it->second.seqno <= seqno) {
    return;
  }
  desc.first_blocks[shard] = FileDescription::Desc{seqno, lt};
  std::vector<tl_object_ptr<ton_api::db_archive_package_firstBlock>> vec;
  for (auto &e : desc.first_blocks) {
    vec.push_back(create_tl_object<ton_api::db_archive_package_firstBlock>(e.first.workchain, e.first.shard,
                                                                           e.second.seqno, e.second.lt));
  }
  index_->begin_transaction().ensure();
  index_
      ->set(create_serialize_tl_object<ton_api::db_archive_package_key>(desc.unix_time, desc.key_block).as_slice(),
            create_serialize_tl_object<ton_api::db_archive_package_value>(desc.unix_time, desc.key_block,
                                                                          std::move(vec), false)
                .as_slice())
      .ensure();
  index_->commit_transaction().ensure();
}

ArchiveManager::FileDescription *ArchiveManager::get_file_by_seqno(ShardIdFull shard, BlockSeqno seqno,
                                                                   bool key_block) {
  auto &f = (key_block ? key_files_ : files_);

  for (auto it = f.rbegin(); it != f.rend(); it++) {
    auto i = it->second.first_blocks.find(shard);
    if (i != it->second.first_blocks.end() && i->second.seqno <= seqno) {
      return &it->second;
    }
  }
  return nullptr;
}

void ArchiveManager::start_up() {
  td::mkdir(db_root_).ensure();
  td::mkdir(db_root_ + "/packed").ensure();
  index_ = std::make_shared<td::RocksDb>(td::RocksDb::open(db_root_ + "/packed/globalindex").move_as_ok());
  std::string value;
  auto v = index_->get(create_serialize_tl_object<ton_api::db_archive_index_key>().as_slice(), value);
  v.ensure();
  if (v.move_as_ok() == td::KeyValue::GetStatus::Ok) {
    auto R = fetch_tl_object<ton_api::db_archive_index_value>(value, true);
    R.ensure();
    auto x = R.move_as_ok();

    for (auto &d : x->packages_) {
      load_package(d, false);
    }
    for (auto &d : x->key_packages_) {
      load_package(d, true);
    }
  }
}

}  // namespace validator

}  // namespace ton
