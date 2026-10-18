#include "block-tx-extractor.h"
#include "crypto/block/block-auto.h"
#include "crypto/block/block-parse.h"
#include "vm/boc.h"
#include "vm/cellslice.h"

namespace ton {

namespace gettx {

td::Result<std::vector<BlockTransactionExtractor::TransactionInfo>> BlockTransactionExtractor::extract_all_transactions(
    const ton::BlockIdExt& block_id,
    const td::BufferSlice& block_data) {
  std::cerr << "DEBUG: extract_all_transactions called for block " << block_id.to_str()
            << ", data size=" << block_data.size() << "\n";

  // Deserialize the block BOC
  auto block_root_result = vm::std_boc_deserialize(block_data.as_slice());
  if (block_root_result.is_error()) {
    return td::Status::Error("Failed to deserialize block BOC");
  }

  auto block_root = block_root_result.move_as_ok();
  std::cerr << "DEBUG: Block BOC deserialized successfully\n";

  return parse_transactions_from_block(block_id, block_root);
}

td::Result<std::vector<BlockTransactionExtractor::TransactionInfo>> BlockTransactionExtractor::parse_transactions_from_block(
    const ton::BlockIdExt& block_id,
    const vm::Ref<vm::Cell>& block_root) {
  std::vector<TransactionInfo> transactions;

  // Unpack the block structure: Block -> BlockExtra -> account_blocks
  block::gen::Block::Record blk;
  block::gen::BlockExtra::Record extra;

  if (!tlb::unpack_cell(block_root, blk)) {
    return td::Status::Error("Cannot unpack block header");
  }

  if (!tlb::unpack_cell(blk.extra, extra)) {
    return td::Status::Error("Cannot unpack block extra");
  }

  std::cerr << "DEBUG: Unpacked block extra, account_blocks root exists=" << extra.account_blocks.not_null() << "\n";

  // Create augmented dictionary from account_blocks
  vm::AugmentedDictionary acc_dict{vm::load_cell_slice_ref(extra.account_blocks), 256,
                                     block::tlb::aug_ShardAccountBlocks};

  // Iterate through all accounts in the account_blocks dictionary
  int account_count = 0;
  int tx_count = 0;

  for (auto acc_iter = acc_dict.begin(); acc_iter != acc_dict.end(); ++acc_iter) {
    account_count++;
    auto acc_entry = *acc_iter;

    // Parse AccountBlock
    block::gen::AccountBlock::Record acc_blk;
    if (!tlb::csr_unpack(std::move(acc_entry.second), acc_blk)) {
      std::cerr << "DEBUG: Warning: Failed to unpack AccountBlock\n";
      continue;
    }

    ton::StdSmcAddress account_addr = acc_blk.account_addr;
    std::cerr << "DEBUG: Processing account " << account_addr.to_hex()
              << " with transactions dict\n";

    // Create dictionary from transactions (64-bit keys = logical times)
    vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                        block::tlb::aug_AccountTransactions};

    // Iterate through all transactions for this account
    for (auto tx_iter = trans_dict.begin(); tx_iter != trans_dict.end(); ++tx_iter) {
      auto tx_entry = *tx_iter;

      // Extract LT from key
      ton::LogicalTime lt = tx_entry.first.get_int(64);

      // Get transaction cell
      auto tx_csr = tx_entry.second;
      if (tx_csr.is_null() || tx_csr->size_refs() == 0) {
        continue;
      }

      vm::Ref<vm::Cell> tx_root = tx_csr->prefetch_ref();
      if (tx_root.is_null()) {
        continue;
      }

      // Parse transaction to get metadata
      block::gen::Transaction::Record trans;
      if (!tlb::unpack_cell(tx_root, trans)) {
        std::cerr << "DEBUG: Warning: Failed to unpack transaction LT=" << lt << "\n";
        continue;
      }

      // Calculate transaction hash
      ton::Bits256 tx_hash;
      tx_hash.as_slice().copy_from(tx_root->get_hash().as_slice());

      // Extract total_fees
      td::uint64 total_fees = 0;
      if (trans.total_fees.not_null()) {
        td::RefInt256 fee_value;
        td::Ref<vm::Cell> fee_extra;
        if (block::unpack_CurrencyCollection(trans.total_fees, fee_value, fee_extra)) {
          if (fee_value.not_null()) {
            unsigned char bytes[32];
            fee_value->export_bytes(bytes, 32, false);
            // Little-endian conversion from bytes 0-7 to uint64
            for (int i = 0; i < 8 && i < 32; i++) {
              total_fees |= ((td::uint64)bytes[i]) << (8 * i);
            }
          }
        }
      }

      // Create transaction info
      TransactionInfo info;
      info.workchain = block_id.id.workchain;
      info.account_addr = account_addr;
      info.lt = lt;
      info.hash = tx_hash;
      info.utime = trans.now;
      info.total_fees = total_fees;
      info.root = tx_root;
      info.block_id = block_id;

      transactions.push_back(std::move(info));
      tx_count++;
    }
  }

  std::cerr << "DEBUG: Extracted " << tx_count << " transactions from " << account_count << " accounts\n";
  return transactions;
}

}  // namespace gettx

}  // namespace ton
