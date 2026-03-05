#include "transaction-lookup.h"
#include "shard-iterator.h"
#include "block-tx-extractor.h"
#include "db-index.h"
#include "package-reader.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Status.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/JsonBuilder.h"

#include <iostream>
#include <sstream>

int run_tx_subcommand(int argc, char* argv[]);
int run_block_subcommand(int argc, char* argv[]);

void print_usage(const char* program_name) {
  std::cerr << "Usage: " << program_name << " <subcommand> [options]\n";
  std::cerr << "\nSubcommands:\n";
  std::cerr << "  tx     Look up transactions by account address and LT\n";
  std::cerr << "  block  Get all transactions for all shards at a masterchain seqno\n";
  std::cerr << "\nRun '" << program_name << " <subcommand> --help' for detailed help.\n";
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  std::string subcommand = argv[1];

  if (subcommand == "tx") {
    return run_tx_subcommand(argc - 1, argv + 1);
  } else if (subcommand == "block") {
    return run_block_subcommand(argc - 1, argv + 1);
  } else if (subcommand == "--help" || subcommand == "-h") {
    print_usage(argv[0]);
    return 0;
  } else {
    std::cerr << "Error: Unknown subcommand '" << subcommand << "'\n";
    print_usage(argv[0]);
    return 1;
  }
}

int run_tx_subcommand(int argc, char* argv[]) {
  ton::WorkchainId workchain = 0;
  std::string address_str;
  td::uint64 logical_time = 0;
  std::string hash_str;
  unsigned count = 10;
  std::string db_path = "/var/ton/db";
  std::string format = "json";
  bool workchain_provided = false;

  td::OptionParser p;
  p.set_description("Look up transactions by account address and LT (liteserver.getTransactions equivalent)");
  p.add_option('w', "workchain", "Workchain ID (default: -1 for masterchain)",
      [&](td::Slice arg) {
        workchain = static_cast<ton::WorkchainId>(td::to_integer<td::int32>(arg));
        workchain_provided = true;
      });
  p.add_option('a', "address", "Account address (hex format, 64 chars for -1 workchain or 96 for base64)",
      [&](td::Slice arg) {
        address_str = arg.str();
      });
  p.add_option('l', "lt", "Logical time of transaction",
      [&](td::Slice arg) {
        logical_time = td::to_integer<td::uint64>(arg);
      });
  p.add_option('h', "hash", "Transaction hash (hex, 64 chars)",
      [&](td::Slice arg) {
        hash_str = arg.str();
      });
  p.add_option('c', "count", PSTRING() << "Number of transactions to retrieve (default: " << count,
      [&](td::Slice arg) {
        count = static_cast<unsigned>(td::to_integer<td::uint64>(arg));
      });
  p.add_option('d', "db-path", PSTRING() << "Path to validator database (default: " << db_path << ")",
      [&](td::Slice arg) {
        db_path = arg.str();
      });
  p.add_option('f', "format", "Output format: json or tl (default: json)",
      [&](td::Slice arg) {
        format = td::to_lower(arg.str());
      });
  bool include_deleted = false;
  p.add_option('\0', "include-deleted", "Include packages marked as deleted in search",
      [&](td::Slice arg) {
        include_deleted = true;
      });

  auto parse_result = p.run(argc, argv);
  if (parse_result.is_error()) {
    std::cerr << parse_result.move_as_error().message().str() << std::endl;
    return 1;
  }

  if (!workchain_provided || address_str.empty() || logical_time == 0 || hash_str.empty()) {
    std::cerr << "Error: --workchain, --address, --lt, and --hash are required\n";
    return 1;
  }

  // Parse address (accept hex or base64 format)
  ton::StdSmcAddress addr;
  if (address_str.size() == 64) {
    // Raw hex format
    addr = td::Bits256();
    if (addr.from_hex(td::Slice(address_str)) != 256) {
      std::cerr << "Error: Invalid address hex format\n";
      return 1;
    }
  } else {
    // Base64url format (TON smart contract address)
    std::string addr_body = address_str;
    std::cerr << "Debug: Original address string: '" << addr_body << "' (length=" << addr_body.size() << ")\n";

    // TON uses base64url without padding, add padding for decoding
    while (addr_body.size() % 4) {
      addr_body += '=';
    }
    std::cerr << "Debug: Address string with padding: '" << addr_body << "'\n";

    auto decode_result = td::base64url_decode(td::Slice(addr_body));
    if (decode_result.is_error()) {
      std::cerr << "Error: Invalid address base64url format: " << decode_result.move_as_error().message().str() << "\n";
      return 1;
    }
    auto addr_bytes = decode_result.move_as_ok();
    std::cerr << "Debug: Decoded address bytes: " << addr_bytes.size() << "\n";

    if (addr_bytes.size() == 36) {
      // TON user-friendly address format: tag(1) + wc(1) + address(32) + CRC(2) = 36 bytes
      std::cerr << "Debug: TON address detected, extracting 32-byte address (skipping first 2 bytes: tag + wc)\n";
      addr.as_slice().copy_from(td::Slice{addr_bytes.data() + 2, 32});
    } else if (addr_bytes.size() != 32) {
      std::cerr << "Error: Decoded address must be 32 or 36 bytes (TON format), got " << addr_bytes.size() << "\n";
      return 1;
    } else {
      addr.as_slice().copy_from(td::Slice{addr_bytes.data(), addr_bytes.size()});
    }
  }

  // Parse hash (accept hex or base64 format)
  td::Bits256 hash;
  if (hash_str.size() == 64) {
    // Raw hex format
    if (hash.from_hex(td::Slice(hash_str)) != 256) {
      std::cerr << "Error: Invalid hash hex format\n";
      return 1;
    }
  } else {
    // Base64 format (transaction hash)
    std::string hash_body = hash_str;
    std::cerr << "Debug: Original hash string: '" << hash_body << "' (length=" << hash_body.size() << ")\n";

    // Try base64url decode first (TON format), then regular base64
    auto decode_result = td::base64url_decode(td::Slice(hash_body));
    if (decode_result.is_error()) {
      std::cerr << "Debug: base64url_decode failed, trying standard base64_decode\n";
      decode_result = td::base64_decode(td::Slice(hash_body));
      if (decode_result.is_error()) {
        std::cerr << "Error: Invalid hash base64 format: " << decode_result.move_as_error().message().str() << "\n";
        return 1;
      }
    }
    auto hash_bytes = decode_result.move_as_ok();
    std::cerr << "Debug: Decoded hash bytes: " << hash_bytes.size() << "\n";

    if (hash_bytes.size() != 32) {
      std::cerr << "Error: Decoded hash must be 32 bytes, got " << hash_bytes.size() << "\n";
      return 1;
    }

    hash.as_slice().copy_from(td::Slice{hash_bytes.data(), hash_bytes.size()});
  }

  // Initialize transaction lookup
  std::cerr << "Creating TransactionLookup with db_path=" << db_path << " include_deleted=" << include_deleted << std::endl;
  ton::gettx::TransactionLookup lookup(db_path, include_deleted);
  std::cerr << "Calling init()" << std::endl;
  td::Status init_status = lookup.init();
  if (init_status.is_error()) {
    std::cerr << "Error: Failed to initialize: " << init_status.message().str() << std::endl;
    return 1;
  }
  std::cerr << "Init successful" << std::endl;

  // Get transactions
  std::cerr << "Looking up transaction at LT=" << logical_time << " for address " << addr.to_hex() << "\n";

  auto result = lookup.get_transactions(workchain, addr, logical_time, hash, count);
  if (result.is_error()) {
    std::cerr << "Error: " << result.move_as_error().message().str() << std::endl;
    return 1;
  }

  auto transactions = result.move_as_ok();

  // Output results
  if (format == "json") {
    td::JsonBuilder jb;
    auto root = jb.enter_object();

    root("transactions", td::json_array([&](auto& arr) {
      for (const auto& tx : transactions) {
        arr(td::json_object([&](auto& tx_obj) {
          // transaction_id
          tx_obj("transaction_id", td::json_object([&](auto& tx_id_obj) {
            tx_id_obj("account", tx.account_addr.to_hex());
            tx_id_obj("lt", static_cast<td::int64>(tx.lt));
            tx_id_obj("hash", tx.hash.to_hex());
          }));

          // other fields
          tx_obj("fee", static_cast<td::int64>(tx.total_fees));
          tx_obj("utime", static_cast<td::int64>(tx.utime));

          // in_msg
          if (!tx.in_msg.empty()) {
            auto in_msg_b64 = td::base64_encode(tx.in_msg.as_slice());
            tx_obj("in_msg", in_msg_b64);
          } else {
            tx_obj("in_msg", td::JsonNull());
          }

          // out_msgs
          tx_obj("out_msgs", td::json_array([&](auto& out_msgs_array) {
            for (const auto& msg : tx.out_msgs) {
              auto msg_b64 = td::base64_encode(msg.as_slice());
              out_msgs_array(msg_b64);
            }
          }));

          // transaction data
          auto data_b64 = td::base64_encode(tx.data.as_slice());
          tx_obj("data", data_b64);
          tx_obj("block", tx.block_id.to_str());
        }));
      }
    }));

    root.leave();

    std::cout << jb.string_builder().as_cslice().c_str() << std::endl;
  } else {
    std::cerr << "Error: Unsupported format '" << format << "'\n";
    return 1;
  }

  return 0;
}

int run_block_subcommand(int argc, char* argv[]) {
  ton::BlockSeqno mc_seqno = 0;
  std::string db_path = "/var/ton/db";
  bool include_deleted = false;

  td::OptionParser p;
  p.set_description("Get all transactions for a masterchain block by seqno");
  p.add_option('\0', "seqno", "Masterchain sequence number",
      [&](td::Slice arg) {
        mc_seqno = td::to_integer<td::uint32>(arg);
      });
  p.add_option('d', "db-path", PSTRING() << "Path to validator database (default: " << db_path << ")",
      [&](td::Slice arg) {
        db_path = arg.str();
      });
  p.add_option('\0', "include-deleted", "Include packages marked as deleted in search",
      [&](td::Slice arg) {
        include_deleted = true;
      });

  auto parse_result = p.run(argc, argv);
  if (parse_result.is_error()) {
    std::cerr << parse_result.move_as_error().message().str() << std::endl;
    return 1;
  }

  if (mc_seqno == 0) {
    std::cerr << "Error: --seqno is required and must be > 0\n";
    return 1;
  }

  std::cerr << "DEBUG: Looking up masterchain block seqno=" << mc_seqno << "\n";

  // Initialize DbIndexReader
  ton::gettx::DbIndexReader db_index(db_path, include_deleted);
  auto init_status = db_index.open();
  if (init_status.is_error()) {
    std::cerr << "Error: Failed to initialize: " << init_status.message().str() << std::endl;
    return 1;
  }

  // Find package containing this masterchain block
  ton::ShardIdFull mc_shard{ton::masterchainId, ton::shardIdAll};
  auto pkg_result = db_index.find_package_by_seqno(mc_shard, mc_seqno);
  if (pkg_result.is_error()) {
    std::cerr << "Error: " << pkg_result.move_as_error().message().str() << std::endl;
    return 1;
  }
  const auto* pkg_info = pkg_result.move_as_ok();
  std::cerr << "DEBUG: Found package id=" << pkg_info->id.id << "\n";

  // Open package
  ton::gettx::PackageReader reader(db_path, pkg_info);
  auto status = reader.open();
  if (status.is_error()) {
    std::cerr << "Error: Failed to open package: " << status.message().str() << std::endl;
    return 1;
  }

  // Get masterchain block data
  auto block_result = reader.get_block_data_by_seqno(mc_shard, mc_seqno);
  if (block_result.is_error()) {
    std::cerr << "Error: Failed to get masterchain block: " << block_result.move_as_error().message().str() << std::endl;
    return 1;
  }
  auto [mc_block_id, mc_block_data] = block_result.move_as_ok();
  std::cerr << "DEBUG: Got masterchain block: " << mc_block_id.to_str() << " size=" << mc_block_data.size() << " bytes\n";

  // Extract shard blocks from this masterchain block
  auto shards_result = ton::gettx::ShardBlockIterator::extract_shard_blocks(mc_block_data);
  if (shards_result.is_error()) {
    std::cerr << "Error: Failed to extract shard blocks: " << shards_result.move_as_error().message().str() << std::endl;
    return 1;
  }
  auto shards = shards_result.move_as_ok();
  std::cerr << "DEBUG: Found " << shards.size() << " shard blocks\n";

  // Collect all transactions from all shards AND the masterchain block itself
  std::vector<ton::gettx::BlockTransactionExtractor::TransactionInfo> all_transactions;

  // First, extract transactions from the masterchain block itself
  std::cerr << "DEBUG: Extracting transactions from masterchain block\n";
  auto mc_tx_result = ton::gettx::BlockTransactionExtractor::extract_all_transactions(mc_block_id, mc_block_data);
  if (mc_tx_result.is_error()) {
    std::cerr << "Warning: Failed to extract transactions from masterchain block: "
              << mc_tx_result.move_as_error().message().str() << "\n";
  } else {
    auto mc_txs = mc_tx_result.move_as_ok();
    std::cerr << "DEBUG: Extracted " << mc_txs.size() << " transactions from masterchain block\n";
    all_transactions.insert(all_transactions.end(), mc_txs.begin(), mc_txs.end());
  }

  // Then, extract transactions from each shard block
  for (const auto& shard : shards) {
    std::cerr << "DEBUG: Processing shard wc=" << shard.workchain
              << " shard=0x" << std::hex << shard.shard << std::dec
              << " seqno=" << shard.seqno << "\n";

    // Find package containing this shard block
    ton::ShardIdFull shard_full{shard.workchain, shard.shard};
    auto shard_pkg_result = db_index.find_package_by_seqno(shard_full, shard.seqno);
    if (shard_pkg_result.is_error()) {
      std::cerr << "Warning: Failed to find package for shard " << shard.workchain
                << ":0x" << std::hex << shard.shard << std::dec << ":" << shard.seqno
                << " - " << shard_pkg_result.move_as_error().message().str() << "\n";
      continue;
    }
    const auto* shard_pkg_info = shard_pkg_result.move_as_ok();

    // Open package for this shard
    ton::gettx::PackageReader shard_reader(db_path, shard_pkg_info);
    auto shard_status = shard_reader.open();
    if (shard_status.is_error()) {
      std::cerr << "Warning: Failed to open package for shard: " << shard_status.message().str() << "\n";
      continue;
    }

    // Get shard block data
    auto shard_block_result = shard_reader.get_block_data_by_seqno(shard_full, shard.seqno);
    if (shard_block_result.is_error()) {
      std::cerr << "Warning: Failed to get shard block data: " << shard_block_result.move_as_error().message().str() << "\n";
      continue;
    }
    auto [shard_block_id, shard_block_data] = shard_block_result.move_as_ok();
    std::cerr << "DEBUG: Got shard block: " << shard_block_id.to_str() << " size=" << shard_block_data.size() << " bytes\n";

    // Extract all transactions from this shard block
    auto tx_result = ton::gettx::BlockTransactionExtractor::extract_all_transactions(shard_block_id, shard_block_data);
    if (tx_result.is_error()) {
      std::cerr << "Warning: Failed to extract transactions from shard block: " << tx_result.move_as_error().message().str() << "\n";
      continue;
    }
    auto txs = tx_result.move_as_ok();
    std::cerr << "DEBUG: Extracted " << txs.size() << " transactions from this shard\n";

    // Add to all transactions
    all_transactions.insert(all_transactions.end(), txs.begin(), txs.end());
  }

  std::cerr << "DEBUG: Total " << all_transactions.size() << " transactions across all shards\n";

  // Output results as JSON
  td::JsonBuilder jb;
  auto root = jb.enter_object();
  root("mc_seqno", static_cast<td::int64>(mc_seqno));
  root("mc_block_id", mc_block_id.to_str());
  root("shard_count", static_cast<td::int64>(shards.size()));
  root("total_transactions", static_cast<td::int64>(all_transactions.size()));

  root("transactions", td::json_array([&](auto& arr) {
    for (const auto& tx : all_transactions) {
      arr(td::json_object([&](auto& tx_obj) {
        // transaction_id
        tx_obj("transaction_id", td::json_object([&](auto& tx_id_obj) {
          tx_id_obj("account", tx.account_addr.to_hex());
          tx_id_obj("lt", static_cast<td::int64>(tx.lt));
          tx_id_obj("hash", tx.hash.to_hex());
        }));

        // other fields
        tx_obj("workchain", tx.workchain);
        tx_obj("fee", static_cast<td::int64>(tx.total_fees));
        tx_obj("utime", static_cast<td::int64>(tx.utime));
        tx_obj("block", tx.block_id.to_str());
      }));
    }
  }));

  root.leave();

  std::cout << jb.string_builder().as_cslice().c_str() << std::endl;

  return 0;
}
