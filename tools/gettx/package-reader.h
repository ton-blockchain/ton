#pragma once

#include "db-index.h"
#include "validator/db/package.hpp"
#include "td/utils/Status.h"
#include "td/utils/buffer.h"

#include <map>
#include <string>
#include <memory>

namespace ton {

namespace gettx {

/**
 * PackageReader - Reads .pack archive files and their .index/ RocksDB databases
 *
 * This class handles:
 * 1. Opening ArchiveSlice .index/ directories (RocksDB)
 * 2. Looking up file_hash → {offset, size} in the index
 * 3. Reading data from .pack files using the Package class
 */
class PackageReader {
 public:
  explicit PackageReader(std::string db_root, const DbIndexReader::PackageInfo* pkg_info);
  ~PackageReader();

  // Open the package file and its index
  td::Status open();

  // Get block data by logical time (returns BlockIdExt and raw block data)
  td::Result<std::pair<ton::BlockIdExt, td::BufferSlice>> get_block_data_by_lt(ton::AccountIdPrefixFull account_id, ton::LogicalTime lt) const;

  // Get block data by seqno (returns BlockIdExt and raw block data)
  td::Result<std::pair<ton::BlockIdExt, td::BufferSlice>> get_block_data_by_seqno(ton::ShardIdFull shard, ton::BlockSeqno seqno) const;

 private:
  std::string db_root_;
  const DbIndexReader::PackageInfo* pkg_info_;
  std::shared_ptr<td::KeyValue> kv_;
  std::unique_ptr<Package> package_;

  td::Status open_index();
  td::Status open_package();

  // Helper methods for block lookup
  td::Result<ton::BlockIdExt> get_block_by_lt(ton::AccountIdPrefixFull account_id, ton::LogicalTime lt) const;
  td::Result<ton::BlockIdExt> get_block_by_seqno(ton::ShardIdFull shard, ton::BlockSeqno seqno) const;

  // Helper to create DB keys for LT lookup
  static td::BufferSlice get_db_key_lt_desc(ton::ShardIdFull shard);
  static td::BufferSlice get_db_key_lt_el(ton::ShardIdFull shard, td::uint32 idx);

  // Read block file directly from package using BlockIdExt
  td::Result<td::BufferSlice> read_block_file(const ton::BlockIdExt& block_id) const;
};

}  // namespace gettx

}  // namespace ton
