#include "transaction-lookup.h"
#include "crypto/block/block-parse.h"
#include "crypto/block/block-auto.h"
#include "crypto/block/block-db.h"
#include "tl/tl/TlObject.h"
#include "vm/boc.h"

namespace ton {

namespace gettx {

TransactionLookup::TransactionLookup(std::string db_root, bool include_deleted)
    : db_root_(std::move(db_root)), include_deleted_(include_deleted) {}

TransactionLookup::~TransactionLookup() = default;

td::Status TransactionLookup::init() {
  db_index_ = std::make_unique<DbIndexReader>(db_root_, include_deleted_);
  return db_index_->open();
}

td::Result<std::vector<TransactionLookup::Transaction>> TransactionLookup::get_transactions(
    ton::WorkchainId workchain,
    const ton::StdSmcAddress& addr,
    ton::LogicalTime lt,
    const td::Bits256& hash,
    unsigned count) {
  std::vector<Transaction> transactions;
  ton::LogicalTime current_lt = lt;
  td::Bits256 current_hash = hash;
  unsigned remaining = count;

  while (remaining > 0 && current_lt > 0) {
    // Find package containing this logical time
    ton::AccountIdPrefixFull account_prefix = ton::extract_addr_prefix(workchain, addr);
    ton::ShardIdFull shard_full = workchain == ton::masterchainId ?
      ton::ShardIdFull{workchain, ton::shardIdAll} :
      ton::shard_prefix(account_prefix, 60);
    auto pkg_result = db_index_->find_package_by_lt(shard_full, current_lt);

    if (pkg_result.is_error()) {
      if (!transactions.empty()) {
        break;  // Return what we have
      }
      return pkg_result.move_as_error();
    }

    const auto* pkg_info = pkg_result.move_as_ok();
    PackageReader reader(db_root_, pkg_info);

    auto status = reader.open();
    if (status.is_error()) {
      if (!transactions.empty()) {
        break;
      }
      return status;
    }

    // Get block data by logical time (returns BlockIdExt and block data)
    auto block_result = reader.get_block_data_by_lt(account_prefix, current_lt);
    if (block_result.is_error()) {
      if (!transactions.empty()) {
        break;
      }
      return block_result.move_as_error();
    }

    auto [block_id, block_data] = block_result.move_as_ok();
    std::cerr << "DEBUG: Found block: " << block_id.to_str() << " size=" << block_data.size() << " bytes\n";

    // Parse block
    auto block_root_result = vm::std_boc_deserialize(block_data.as_slice());
    if (block_root_result.is_error()) {
      std::cerr << "DEBUG: Failed to deserialize BOC" << std::endl;
      if (!transactions.empty()) {
        break;
      }
      return td::Status::Error("Failed to deserialize block BOC");
    }

    auto block_root = block_root_result.move_as_ok();
    std::cerr << "DEBUG: Block BOC deserialized successfully\n";
    std::cerr << "DEBUG: Block root cell hash: " << block_root->get_hash().to_hex() << "\n";
    std::cerr << "DEBUG: Block root cell depth: " << block_root->get_depth() << "\n";

    // Sanity check: verify this looks like a block by checking if we can read block info
    block::gen::Block::Record block_rec;
    block::gen::BlockInfo::Record info_rec;
    if (tlb::unpack_cell(block_root, block_rec)) {
      std::cerr << "DEBUG: Successfully unpacked block - global_id=" << block_rec.global_id << "\n";
      if (tlb::unpack_cell(block_rec.info, info_rec)) {
        std::cerr << "DEBUG: Block info: start_lt=" << info_rec.start_lt << " end_lt=" << info_rec.end_lt << "\n";
      }
    } else {
      std::cerr << "DEBUG: WARNING - Failed to unpack as block! This might not be a block.\n";
    }

    // Verify block contains the account
    if (!ton::shard_contains(block_id.shard_full(), account_prefix)) {
      std::cerr << "DEBUG: Block doesn't contain account" << std::endl;
      return td::Status::Error("Obtained block that cannot contain specified account");
    }

    // Extract transaction
    std::cerr << "DEBUG: Looking for transaction wc=" << workchain << " addr=" << addr.to_hex()
              << " lt=" << current_lt << std::endl;
    auto tx_result = block::get_block_transaction_try(block_root, workchain, addr, current_lt);
    if (tx_result.is_error()) {
      std::cerr << "DEBUG: Transaction lookup failed: " << tx_result.move_as_error().message().str() << std::endl;
      if (!transactions.empty()) {
        break;
      }
      return tx_result.move_as_error();
    }

    auto tx_root = tx_result.move_as_ok();

    if (tx_root.not_null()) {
      // Transaction found
      std::cerr << "DEBUG: Transaction found! LT=" << current_lt
                << " tx_root->not_null()=" << tx_root.not_null()
                << " tx_root->get_depth()=" << tx_root->get_depth() << "\n";

      // The hash should be the cell's root hash (Cell::Hash)
      auto cell_hash = tx_root->get_hash();
      td::Bits256 actual_hash;
      actual_hash.as_slice().copy_from(cell_hash.as_slice());

      std::cerr << "DEBUG: expected_hash=" << current_hash.to_hex()
                << " actual_hash=" << actual_hash.to_hex() << "\n";

      // Try to unpack the transaction to see if it's valid
      block::gen::Transaction::Record trans;
      if (tlb::unpack_cell(tx_root, trans)) {
        std::cerr << "DEBUG: Successfully unpacked transaction - account_addr=" << trans.account_addr.to_hex()
                  << " lt=" << trans.lt << " now=" << trans.now << "\n";
      } else {
        std::cerr << "DEBUG: WARNING - Failed to unpack transaction!\n";
      }

      if (current_hash != actual_hash) {
        if (!transactions.empty()) {
          break;  // Hash mismatch, return what we have
        }
        return td::Status::Error(PSTRING() << "Transaction hash mismatch: expected " << current_hash.to_hex()
                                            << " but got " << actual_hash.to_hex());
      }

      if (trans.prev_trans_lt >= current_lt) {
        return td::Status::Error("Previous transaction time is not less than current one");
      }

      // Add to results
      Transaction tx;
      tx.workchain = workchain;
      tx.account_addr = addr;
      tx.lt = current_lt;
      tx.hash = current_hash;
      tx.root = tx_root;
      tx.block_id = block_id;
      tx.utime = trans.now;

      // Serialize transaction to BOC
      auto boc_result = vm::std_boc_serialize(tx_root);
      if (boc_result.is_error()) {
        return td::Status::Error("Failed to serialize transaction to BOC");
      }
      tx.data = boc_result.move_as_ok();

      // Extract total_fees (CurrencyCollection)
      if (trans.total_fees.not_null()) {
        td::RefInt256 fee_value;
        td::Ref<vm::Cell> fee_extra;
        if (block::unpack_CurrencyCollection(trans.total_fees, fee_value, fee_extra)) {
          // Convert RefInt256 to uint64 - assuming fees fit in 64 bits
          if (fee_value.not_null()) {
            unsigned char bytes[32];
            fee_value->export_bytes(bytes, 32, false);
            // Little-endian conversion from bytes 0-7 to uint64
            tx.total_fees = 0;
            for (int i = 0; i < 8 && i < 32; i++) {
              tx.total_fees |= ((td::uint64)bytes[i]) << (8 * i);
            }
          } else {
            tx.total_fees = 0;
          }
        } else {
          tx.total_fees = 0;
        }
      } else {
        tx.total_fees = 0;
      }

      // Extract in_msg
      if (trans.r1.in_msg.not_null() && trans.r1.in_msg->prefetch_ref().not_null()) {
        auto in_msg_ref = trans.r1.in_msg->prefetch_ref();
        auto in_msg_boc = vm::std_boc_serialize(in_msg_ref);
        if (in_msg_boc.is_ok()) {
          tx.in_msg = in_msg_boc.move_as_ok();
        }
      }

      // Extract out_msgs (HashmapE 15)
      if (trans.r1.out_msgs.not_null()) {
        vm::Dictionary out_dict{trans.r1.out_msgs, 15};
        for (auto iter = out_dict.begin(); iter != out_dict.end(); ++iter) {
          // The value in the dictionary is a CellSlice pointing to a Cell
          auto out_msg_cs = (*iter).second;  // This is a CellSlice
          if (out_msg_cs.not_null() && out_msg_cs->size_refs() > 0) {
            auto out_msg_ref = out_msg_cs->prefetch_ref();  // Get the Cell from the CellSlice
            if (out_msg_ref.not_null()) {
              auto out_msg_boc = vm::std_boc_serialize(out_msg_ref);
              if (out_msg_boc.is_ok()) {
                tx.out_msgs.push_back(out_msg_boc.move_as_ok());
              }
            }
          }
        }
      }

      transactions.push_back(std::move(tx));

      // Move to previous transaction
      current_lt = trans.prev_trans_lt;
      current_hash = trans.prev_trans_hash;
      remaining--;
    } else {
      // Transaction not found in this block
      if (!transactions.empty()) {
        break;
      }
      return td::Status::Error("Cannot locate transaction in block with specified logical time");
    }
  }

  return transactions;
}

}  // namespace gettx

}  // namespace ton
