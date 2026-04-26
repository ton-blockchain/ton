#include "shard-iterator.h"
#include "crypto/block/block-auto.h"
#include "crypto/block/block-parse.h"
#include "crypto/block/mc-config.h"
#include "vm/boc.h"
#include "vm/cellslice.h"
#include <functional>

namespace ton {

namespace gettx {

td::Result<std::vector<ShardBlockIterator::ShardBlockInfo>> ShardBlockIterator::extract_shard_blocks(
    const td::BufferSlice& mc_block_data) {
  std::cerr << "DEBUG: extract_shard_blocks called, block data size=" << mc_block_data.size() << "\n";

  // Deserialize the block BOC
  auto block_root_result = vm::std_boc_deserialize(mc_block_data.as_slice());
  if (block_root_result.is_error()) {
    return td::Status::Error("Failed to deserialize masterchain block BOC");
  }

  auto block_root = block_root_result.move_as_ok();
  std::cerr << "DEBUG: Masterchain block BOC deserialized successfully\n";

  return parse_shard_hashes_from_block(block_root);
}

td::Result<std::vector<ShardBlockIterator::ShardBlockInfo>> ShardBlockIterator::parse_shard_hashes_from_block(
    const vm::Ref<vm::Cell>& block_root) {
  std::vector<ShardBlockInfo> shard_blocks;

  // Unpack the block structure: Block -> BlockExtra -> McBlockExtra -> shard_hashes
  block::gen::Block::Record blk;
  block::gen::BlockExtra::Record extra;
  block::gen::McBlockExtra::Record mc_extra;

  if (!tlb::unpack_cell(block_root, blk)) {
    return td::Status::Error("Cannot unpack masterchain block header");
  }

  if (!tlb::unpack_cell(blk.extra, extra)) {
    return td::Status::Error("Cannot unpack masterchain block extra");
  }

  // Check if custom field exists (it contains McBlockExtra)
  if (!extra.custom->have_refs()) {
    std::cerr << "DEBUG: No custom field in block extra (no shard hashes)\n";
    return shard_blocks;  // Return empty vector
  }

  if (!tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra)) {
    return td::Status::Error("Cannot unpack masterchain block McBlockExtra");
  }

  std::cerr << "DEBUG: Unpacked McBlockExtra, shard_hashes root exists=" << mc_extra.shard_hashes.not_null() << "\n";

  // Create dictionary from shard_hashes (32-bit keys = workchain IDs)
  vm::Dictionary shards_dict(std::move(mc_extra.shard_hashes), 32);

  std::cerr << "DEBUG: Iterating through shard_hashes dictionary\n";

  // Iterate through all workchains in the shard_hashes dictionary
  auto iter = shards_dict.begin();
  while (iter != shards_dict.end()) {
    auto entry = *iter;
    int workchain = static_cast<int>(entry.first.get_int(32));

    std::cerr << "DEBUG: Processing workchain " << workchain << "\n";

    // The value is a BinTree of shard descriptions
    // We need to traverse it to extract all shards
    vm::Ref<vm::Cell> workchain_root = entry.second->prefetch_ref();
    if (workchain_root.is_null()) {
      std::cerr << "DEBUG: Workchain " << workchain << " has null shard_hashes root\n";
      ++iter;
      continue;
    }

    std::cerr << "DEBUG: Workchain " << workchain << " shard_hashes root not null, creating shard dictionary\n";

    // The value is actually a Dictionary (HashmapE with 64-bit keys = shard IDs)
    // Check if the value CellSlice is valid and has data
    auto value_csr = entry.second;
    if (value_csr.is_null()) {
      std::cerr << "DEBUG: Workchain " << workchain << " has null value CellSlice\n";
      ++iter;
      continue;
    }

    std::cerr << "DEBUG: Value CellSlice size=" << value_csr->size()
              << " refs=" << value_csr->size_refs() << "\n";

    // The HashmapE is stored as a reference (size=0, refs=1)
    // Create dictionary directly from the CellSlice with validation disabled
    // This allows the Dictionary parser to handle the HashmapE label format
    std::cerr << "DEBUG: Creating shard_dict for workchain " << workchain << " (validation disabled)\n";

    vm::Dictionary shard_dict(value_csr, 64, false);  // Disable validation for HashmapE

    std::cerr << "DEBUG: Created shard_dict successfully for workchain " << workchain << "\n";
    std::cerr << "DEBUG: Checking if shard_dict is empty\n";

    if (shard_dict.is_empty()) {
      std::cerr << "DEBUG: Shard dictionary for workchain " << workchain << " is empty, skipping\n";
      ++iter;
      continue;
    }

    std::cerr << "DEBUG: Shard dictionary is not empty, beginning iteration\n";

    // Iterate through all shards in this workchain
    try {
      for (auto shard_iter = shard_dict.begin(); shard_iter != shard_dict.end(); ++shard_iter) {
      auto shard_entry = *shard_iter;
      td::uint64 shard = shard_entry.first.get_int(64);

      std::cerr << "DEBUG: Found shard wc=" << workchain
                << " shard=0x" << std::hex << shard << std::dec << "\n";

      // shard_entry.second is a Ref<CellSlice>, load it properly
      auto shard_csr = shard_entry.second;
      if (shard_csr.is_null()) {
        std::cerr << "DEBUG: shard_entry CellSlice is null\n";
        continue;
      }

      vm::CellSlice cs = *shard_csr;  // Create mutable copy for unpack()
      auto shard_info = block::McShardHash::unpack(cs, ton::ShardIdFull{workchain, shard});

      if (shard_info.not_null()) {
        ShardBlockInfo info;
        info.workchain = workchain;
        info.shard = shard;
        info.seqno = shard_info->blk_.seqno();
        info.root_hash = shard_info->blk_.root_hash;
        info.file_hash = shard_info->blk_.file_hash;

        shard_blocks.push_back(info);
        std::cerr << "DEBUG: Successfully extracted shard block wc=" << workchain
                  << " shard=0x" << std::hex << shard << std::dec
                  << " seqno=" << info.seqno << "\n";
      } else {
        std::cerr << "DEBUG: Failed to unpack McShardHash for shard\n";
      }
    }
    } catch (const std::exception& e) {
      std::cerr << "DEBUG: Exception while iterating shards for workchain " << workchain
                << ": " << e.what() << "\n";
    } catch (...) {
      std::cerr << "DEBUG: Unknown exception while iterating shards for workchain " << workchain << "\n";
    }

    ++iter;
  }

  std::cerr << "DEBUG: Extracted " << shard_blocks.size() << " shard blocks total\n";
  return shard_blocks;
}

}  // namespace gettx

}  // namespace ton
