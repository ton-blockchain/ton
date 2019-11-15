#pragma once

#include "archive-slice.hpp"

namespace ton {

namespace validator {

struct PackageId {
  td::uint32 id;
  bool key;
  bool temp;

  explicit PackageId(td::uint32 id, bool key, bool temp) : id(id), key(key), temp(temp) {
  }

  bool operator<(const PackageId &with) const {
    return id < with.id;
  }
  bool operator==(const PackageId &with) const {
    return id == with.id;
  }

  std::string path() const;
  std::string name() const;

  bool is_empty() {
    return id == std::numeric_limits<td::uint32>::max();
  }
  static PackageId empty(bool key, bool temp) {
    return PackageId(std::numeric_limits<td::uint32>::max(), key, temp);
  }
};

class RootDb;

class ArchiveManager : public td::actor::Actor {
 public:
  ArchiveManager(td::actor::ActorId<RootDb> root, std::string db_root);

  void add_handle(BlockHandle handle, td::Promise<td::Unit> promise);
  void update_handle(BlockHandle handle, td::Promise<td::Unit> promise);
  void add_file(BlockHandle handle, FileReference ref_id, td::BufferSlice data, td::Promise<td::Unit> promise);
  void add_key_block_proof(BlockSeqno seqno, UnixTime ts, LogicalTime lt, FileReference ref_id, td::BufferSlice data,
                           td::Promise<td::Unit> promise);
  void add_temp_file_short(FileReference ref_id, td::BufferSlice data, td::Promise<td::Unit> promise);
  void get_handle(BlockIdExt block_id, td::Promise<BlockHandle> promise);
  void get_key_block_proof(FileReference ref_id, td::Promise<td::BufferSlice> promise);
  void get_temp_file_short(FileReference ref_id, td::Promise<td::BufferSlice> promise);
  void get_file_short(FileReference ref_id, td::Promise<td::BufferSlice> promise);
  void get_file(BlockHandle handle, FileReference ref_id, td::Promise<td::BufferSlice> promise);

  void add_zero_state(BlockIdExt block_id, td::BufferSlice data, td::Promise<td::Unit> promise);
  void add_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::BufferSlice data,
                            td::Promise<td::Unit> promise);
  void get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise);
  void get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::Promise<td::BufferSlice> promise);
  void get_persistent_state_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::int64 offset,
                                  td::int64 max_size, td::Promise<td::BufferSlice> promise);
  void check_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::Promise<bool> promise);
  void check_zero_state(BlockIdExt block_id, td::Promise<bool> promise);

  void run_gc(UnixTime ts);

  /* from LTDB */
  void get_block_by_unix_time(AccountIdPrefixFull account_id, UnixTime ts, td::Promise<BlockHandle> promise);
  void get_block_by_lt(AccountIdPrefixFull account_id, LogicalTime lt, td::Promise<BlockHandle> promise);
  void get_block_by_seqno(AccountIdPrefixFull account_id, BlockSeqno seqno, td::Promise<BlockHandle> promise);

  void get_archive_id(BlockSeqno masterchain_seqno, td::Promise<td::uint64> promise);
  void get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                         td::Promise<td::BufferSlice> promise);

  void start_up() override;

  void begin_transaction();
  void commit_transaction();
  void set_async_mode(bool mode, td::Promise<td::Unit> promise);

 private:
  struct FileDescription {
    struct Desc {
      BlockSeqno seqno;
      UnixTime ts;
      LogicalTime lt;
    };
    FileDescription(PackageId id, bool deleted) : id(id), deleted(deleted) {
    }
    auto file_actor_id() const {
      return file.get();
    }
    void clear_actor_id() {
      file.reset();
    }
    PackageId id;
    bool deleted;

    std::map<ShardIdFull, Desc> first_blocks;
    td::actor::ActorOwn<ArchiveSlice> file;
  };

  std::map<PackageId, FileDescription> files_;
  std::map<PackageId, FileDescription> key_files_;
  std::map<PackageId, FileDescription> temp_files_;
  bool async_mode_ = false;
  bool huge_transaction_started_ = false;
  td::uint32 huge_transaction_size_ = 0;

  auto &get_file_map(const PackageId &p) {
    return p.key ? key_files_ : p.temp ? temp_files_ : files_;
  }

  std::map<FileHash, FileReferenceShort> perm_states_;

  void load_package(PackageId seqno);
  void delete_package(PackageId seqno);
  void deleted_package(PackageId seqno);
  void get_handle_cont(BlockIdExt block_id, PackageId id, td::Promise<BlockHandle> promise);
  void get_handle_finish(BlockHandle handle, td::Promise<BlockHandle> promise);
  void get_file_short_cont(FileReference ref_id, PackageId idx, td::Promise<td::BufferSlice> promise);

  FileDescription *get_file_desc(ShardIdFull shard, PackageId id, BlockSeqno seqno, UnixTime ts, LogicalTime lt,
                                 bool force);
  FileDescription *add_file_desc(ShardIdFull shard, PackageId id, BlockSeqno seqno, UnixTime ts, LogicalTime lt);
  void update_desc(FileDescription &desc, ShardIdFull shard, BlockSeqno seqno, UnixTime ts, LogicalTime lt);
  FileDescription *get_file_desc_by_seqno(ShardIdFull shard, BlockSeqno seqno, bool key_block);
  FileDescription *get_file_desc_by_lt(ShardIdFull shard, LogicalTime lt, bool key_block);
  FileDescription *get_file_desc_by_unix_time(ShardIdFull shard, UnixTime ts, bool key_block);
  FileDescription *get_file_desc_by_seqno(AccountIdPrefixFull shard, BlockSeqno seqno, bool key_block);
  FileDescription *get_file_desc_by_lt(AccountIdPrefixFull shard, LogicalTime lt, bool key_block);
  FileDescription *get_file_desc_by_unix_time(AccountIdPrefixFull shard, UnixTime ts, bool key_block);
  FileDescription *get_temp_file_desc_by_idx(PackageId idx);
  PackageId get_max_temp_file_desc_idx();
  PackageId get_prev_temp_file_desc_idx(PackageId id);

  void written_perm_state(FileReferenceShort id);

  void persistent_state_gc(FileHash last);
  void got_gc_masterchain_handle(BlockHandle handle, FileHash hash);

  std::string db_root_;

  std::shared_ptr<td::KeyValue> index_;

  PackageId get_package_id(BlockSeqno seqno) const;
  PackageId get_package_id_force(BlockSeqno masterchain_seqno, ShardIdFull shard, BlockSeqno seqno, UnixTime ts,
                                 LogicalTime lt, bool is_key);
  PackageId get_temp_package_id() const;
  PackageId get_key_package_id(BlockSeqno seqno) const;
  PackageId get_temp_package_id_by_unixtime(UnixTime ts) const;
};

}  // namespace validator

}  // namespace ton
