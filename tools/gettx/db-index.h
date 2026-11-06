#pragma once

#include "td/actor/actor.h"
#include "td/utils/Status.h"
#include "validator/db/fileref.hpp"
#include "validator/db/archive-slice.hpp"
#include "crypto/block/block.h"

#include <map>
#include <string>
#include <vector>
#include <memory>

namespace ton {

namespace gettx {

struct FirstBlockInfo {
  ton::BlockSeqno seqno;
  ton::UnixTime ts;
  ton::LogicalTime lt;
  FirstBlockInfo() = default;
  FirstBlockInfo(ton::BlockSeqno s, ton::UnixTime t, ton::LogicalTime l) : seqno(s), ts(t), lt(l) {}
};

/**
 * DbIndexReader - Reads the Global Archive Index to discover packages
 *
 * The Global Archive Index is a RocksDB database at /files/globalindex
 * that contains metadata about all archive packages.
 */
class DbIndexReader {
 public:
  struct PackageInfo {
    validator::PackageId id;
    bool deleted;
    std::map<ton::ShardIdFull, FirstBlockInfo> first_blocks;

    PackageInfo() : id(0, false, false), deleted(false) {}
  };

  explicit DbIndexReader(std::string db_root, bool include_deleted = false);
  ~DbIndexReader();

  // Open the Global Archive Index
  td::Status open();

  // Get all packages from the index
  td::Result<std::vector<PackageInfo>> load_packages() const;

  // Find package containing given logical time for a shard
  td::Result<const PackageInfo*> find_package_by_lt(ton::ShardIdFull shard, ton::LogicalTime lt) const;

  // Find package containing given seqno for a shard
  td::Result<const PackageInfo*> find_package_by_seqno(ton::ShardIdFull shard, ton::BlockSeqno seqno) const;

 private:
  std::string db_root_;
  bool include_deleted_;
  std::shared_ptr<td::KeyValue> index_;
  mutable std::vector<PackageInfo> packages_cache_;  // Cache to keep pointers valid
  mutable bool cache_loaded_ = false;

  td::Status load_index();
};

}  // namespace gettx

}  // namespace ton
