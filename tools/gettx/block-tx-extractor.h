#pragma once

#include "td/utils/Status.h"
#include "td/utils/buffer.h"
#include "crypto/block/block.h"
#include "vm/cells.h"
#include "ton/ton-types.h"

#include <vector>

namespace ton {

namespace gettx {

/**
 * BlockTransactionExtractor - Extracts all transactions from a block
 *
 * Reads the account_blocks dictionary from a block and extracts
 * all transactions with their metadata.
 */
class BlockTransactionExtractor {
 public:
  struct TransactionInfo {
    ton::WorkchainId workchain;
    ton::StdSmcAddress account_addr;
    ton::LogicalTime lt;
    ton::Bits256 hash;
    ton::UnixTime utime;
    td::uint64 total_fees;
    td::Ref<vm::Cell> root;  // Transaction cell

    ton::BlockIdExt block_id;  // Which block this transaction is from
  };

  // Extract all transactions from a block
  static td::Result<std::vector<TransactionInfo>> extract_all_transactions(
      const ton::BlockIdExt& block_id,
      const td::BufferSlice& block_data);

 private:
  // Parse account_blocks dictionary and extract transactions
  static td::Result<std::vector<TransactionInfo>> parse_transactions_from_block(
      const ton::BlockIdExt& block_id,
      const vm::Ref<vm::Cell>& block_root);
};

}  // namespace gettx

}  // namespace ton
