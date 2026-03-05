#pragma once

#include "db-index.h"
#include "package-reader.h"
#include "crypto/block/block.h"
#include "vm/cellops.h"

#include <vector>
#include <functional>

namespace ton {

namespace gettx {

/**
 * TransactionLookup - Core getTransactions logic
 *
 * Ported from validator/impl/liteserver.cpp:1626-1705
 */
class TransactionLookup {
 public:
  struct Transaction {
    ton::WorkchainId workchain;
    ton::StdSmcAddress account_addr;
    ton::LogicalTime lt;
    td::Bits256 hash;
    td::Ref<vm::Cell> root;
    ton::BlockIdExt block_id;
    td::uint32 utime;
    td::BufferSlice data;
    td::uint64 total_fees;
    td::BufferSlice in_msg;
    std::vector<td::BufferSlice> out_msgs;
  };

  explicit TransactionLookup(std::string db_root, bool include_deleted = false);
  ~TransactionLookup();

  // Initialize the lookup system
  td::Status init();

  // Get transactions for an account starting from a given logical time
  // Returns up to 'count' transactions going backwards in time
  td::Result<std::vector<Transaction>> get_transactions(
      ton::WorkchainId workchain,
      const ton::StdSmcAddress& addr,
      ton::LogicalTime lt,
      const td::Bits256& hash,
      unsigned count);

 private:
  std::string db_root_;
  bool include_deleted_;
  std::unique_ptr<DbIndexReader> db_index_;
};

}  // namespace gettx

}  // namespace ton
