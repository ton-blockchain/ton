#pragma once

#include "td/utils/Status.h"
#include "crypto/block/block.h"
#include "vm/cells.h"
#include "ton/ton-types.h"

#include <vector>

namespace ton {

namespace gettx {

/**
 * ShardBlockIterator - Extracts shard block IDs from a masterchain block
 *
 * Reads the shard_hashes dictionary from a masterchain block's state
 * and returns all referenced shard block IDs.
 */
class ShardBlockIterator {
 public:
  struct ShardBlockInfo {
    ton::WorkchainId workchain;
    ton::ShardId shard;
    ton::BlockSeqno seqno;
    ton::RootHash root_hash;
    ton::FileHash file_hash;

    ton::BlockIdExt to_block_id_ext() const {
      ton::BlockId id{workchain, shard, seqno};
      return ton::BlockIdExt{id, root_hash, file_hash};
    }
  };

  // Extract all shard block IDs from a masterchain block
  static td::Result<std::vector<ShardBlockInfo>> extract_shard_blocks(const td::BufferSlice& mc_block_data);

 private:
  // Parse the masterchain block and extract shard_hashes dictionary
  static td::Result<std::vector<ShardBlockInfo>> parse_shard_hashes_from_block(const vm::Ref<vm::Cell>& block_root);
};

}  // namespace gettx

}  // namespace ton
