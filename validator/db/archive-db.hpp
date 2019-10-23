#pragma once

#include "td/actor/actor.h"
#include "td/utils/buffer.h"
#include "ton/ton-types.h"
#include "td/utils/port/FileFd.h"
#include "package.hpp"
#include "filedb.hpp"
#include "validator/interfaces/block-handle.h"

#include <map>
#include <list>

namespace ton {

namespace validator {

class PackageWriter : public td::actor::Actor {
 public:
  PackageWriter(std::shared_ptr<Package> package) : package_(std::move(package)) {
  }

  void append(std::string filename, td::BufferSlice data, td::Promise<std::pair<td::uint64, td::uint64>> promise);

 private:
  std::shared_ptr<Package> package_;
};

class ArchiveFile : public td::actor::Actor {
 public:
  ArchiveFile(std::string path, UnixTime ts) : path_(std::move(path)), ts_(ts) {
  }
  void start_up() override;
  void write(FileDb::RefId ref_id, td::BufferSlice data, td::Promise<td::Unit> promise);
  void write_handle(BlockHandle handle, td::Promise<td::Unit> promise);
  void read(FileDb::RefId ref_id, td::Promise<td::BufferSlice> promise);
  void read_handle(BlockIdExt block_id, td::Promise<BlockHandle> promise);

 private:
  void completed_write(FileDb::RefId ref_id, td::uint64 offset, td::uint64 new_size, td::Promise<td::Unit> promise);

  std::shared_ptr<Package> package_;
  std::shared_ptr<td::KeyValue> index_;

  td::actor::ActorOwn<PackageWriter> writer_;

  std::string path_;
  UnixTime ts_;
};

class ArchiveManager : public td::actor::Actor {
 public:
  ArchiveManager(std::string db_root);
  void write(UnixTime ts, bool key_block, FileDb::RefId ref_id, td::BufferSlice data, td::Promise<td::Unit> promise);
  void write_handle(BlockHandle handle, td::Promise<td::Unit> promise);
  void read(UnixTime ts, bool key_block, FileDb::RefId ref_id, td::Promise<td::BufferSlice> promise);
  void read_handle(BlockIdExt block_id, td::Promise<BlockHandle> promise);

  void start_up() override;

 private:
  void read_handle_cont(BlockIdExt block_id, td::Promise<BlockHandle> promise);
  struct FileDescription {
    struct Desc {
      BlockSeqno seqno;
      LogicalTime lt;
    };
    FileDescription(UnixTime unix_time, bool key_block) : unix_time(unix_time), key_block(key_block) {
    }
    auto file_actor_id() const {
      return file.get();
    }
    UnixTime unix_time;
    bool key_block;

    std::map<ShardIdFull, Desc> first_blocks;
    td::actor::ActorOwn<ArchiveFile> file;
  };

  void load_package(UnixTime ts, bool key_block);

  UnixTime convert_ts(UnixTime ts, bool key_block);
  FileDescription *get_file(UnixTime ts, bool key_block, bool force = true);
  FileDescription *add_file(UnixTime ts, bool key_block);
  void update_desc(FileDescription &desc, ShardIdFull shard, BlockSeqno seqno, LogicalTime lt);
  FileDescription *get_file_by_seqno(ShardIdFull shard, BlockSeqno seqno, bool key_block);

  std::string db_root_;
  std::map<UnixTime, FileDescription> files_;
  std::map<UnixTime, FileDescription> key_files_;

  std::shared_ptr<td::KeyValue> index_;
};

}  // namespace validator

}  // namespace ton
