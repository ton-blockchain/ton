#include "gettx_api.h"
#include "transaction-lookup.h"
#include "transaction-json.h"
#include "block-tx-extractor.h"
#include "td/utils/Status.h"
#include "td/utils/JsonBuilder.h"
#include <memory>

extern "C" {

gettx_handle_t gettx_create(const char* db_path, int include_deleted) {
  if (!db_path) {
    return nullptr;
  }

  try {
    auto* lookup = new ton::gettx::TransactionLookup(std::string(db_path), include_deleted != 0);
    auto status = lookup->init();
    if (status.is_error()) {
      delete lookup;
      return nullptr;
    }
    return static_cast<void*>(lookup);
  } catch (...) {
    return nullptr;
  }
}

gettx_result_t gettx_lookup(gettx_handle_t handle,
                           int workchain,
                           const char* address,
                           unsigned long long logical_time,
                           const char* hash,
                           unsigned count) {
  gettx_result_t result = {nullptr, 0, nullptr};

  if (!handle || !address || !hash) {
    result.error_code = -1;
    result.error_msg = strdup("Invalid arguments");
    return result;
  }

  try {
    auto* lookup = static_cast<ton::gettx::TransactionLookup*>(handle);

    // Parse address (base64url format)
    ton::StdSmcAddress addr;
    std::string addr_str = address;

    // TON uses base64url without padding, add padding for decoding
    while (addr_str.size() % 4) {
      addr_str += '=';
    }

    auto decode_result = td::base64url_decode(td::Slice(addr_str));
    if (decode_result.is_error()) {
      decode_result = td::base64_decode(td::Slice(addr_str));
    }
    if (decode_result.is_error()) {
      result.error_code = -2;
      result.error_msg = strdup("Invalid address format");
      return result;
    }

    auto addr_bytes = decode_result.move_as_ok();
    if (addr_bytes.size() == 36) {
      // TON user-friendly address format: tag(1) + wc(1) + address(32) + CRC(2)
      addr.as_slice().copy_from(td::Slice{addr_bytes.data() + 2, 32});
    } else if (addr_bytes.size() == 32) {
      addr.as_slice().copy_from(td::Slice{addr_bytes.data(), 32});
    } else {
      result.error_code = -3;
      result.error_msg = strdup("Invalid address length");
      return result;
    }

    // Parse hash (base64 or base64url format)
    td::Bits256 tx_hash;
    std::string hash_str = hash;

    auto hash_decode = td::base64url_decode(td::Slice(hash_str));
    if (hash_decode.is_error()) {
      hash_decode = td::base64_decode(td::Slice(hash_str));
    }
    if (hash_decode.is_error()) {
      result.error_code = -4;
      result.error_msg = strdup("Invalid hash format");
      return result;
    }

    auto hash_bytes = hash_decode.move_as_ok();
    if (hash_bytes.size() != 32) {
      result.error_code = -5;
      result.error_msg = strdup("Invalid hash length");
      return result;
    }
    tx_hash.as_slice().copy_from(td::Slice{hash_bytes.data(), 32});

    // Get transactions
    auto tx_result = lookup->get_transactions(
      static_cast<ton::WorkchainId>(workchain),
      addr,
      logical_time,
      tx_hash,
      count
    );

    if (tx_result.is_error()) {
      result.error_code = -6;
      std::string error_msg = tx_result.move_as_error().message().str();
      result.error_msg = strdup(error_msg.c_str());
      return result;
    }

    auto transactions = tx_result.move_as_ok();

    // Build JSON output using JsonBuilder
    auto json_str = td::json_encode<std::string>(td::json_object([&](auto& o) {
      o("transactions", td::json_array([&](auto& arr) {
        for (const auto& tx : transactions) {
          ton::gettx::serialize_transaction(arr.enter_value(), tx);
        }
      }));
    }));

    // Allocate and return the JSON string
    result.json_data = strdup(json_str.c_str());
    result.error_code = 0;
    result.error_msg = nullptr;

    return result;

  } catch (const std::exception& e) {
    result.error_code = -100;
    result.error_msg = strdup(e.what());
    return result;
  } catch (...) {
    result.error_code = -101;
    result.error_msg = strdup("Unknown error");
    return result;
  }
}

gettx_result_t gettx_lookup_block(gettx_handle_t handle, unsigned mc_seqno) {
  gettx_result_t result = {nullptr, 0, nullptr};

  if (!handle) {
    result.error_code = -1;
    result.error_msg = strdup("Invalid handle");
    return result;
  }

  try {
    auto* lookup = static_cast<ton::gettx::TransactionLookup*>(handle);

    // Get block transactions using the new method
    auto block_result = lookup->get_block_transactions(mc_seqno);

    if (block_result.is_error()) {
      result.error_code = -6;
      std::string error_msg = block_result.move_as_error().message().str();
      result.error_msg = strdup(error_msg.c_str());
      return result;
    }

    auto block_data = block_result.move_as_ok();

    // Build JSON output using JsonBuilder
    auto json_str = td::json_encode<std::string>(td::json_object([&](auto& o) {
      o("mc_seqno", td::JsonLong(block_data.mc_seqno));
      o("mc_block_id", block_data.mc_block_id.to_str());
      o("shard_count", td::JsonLong(block_data.shard_count));
      o("total_transactions", td::JsonLong(block_data.total_transactions));
      o("transactions", td::json_array([&](auto& arr) {
        for (const auto& tx : block_data.transactions) {
          ton::gettx::serialize_transaction(arr.enter_value(), tx);
        }
      }));
    }));

    // Allocate and return the JSON string
    result.json_data = strdup(json_str.c_str());
    result.error_code = 0;
    result.error_msg = nullptr;

    return result;

  } catch (const std::exception& e) {
    result.error_code = -100;
    result.error_msg = strdup(e.what());
    return result;
  } catch (...) {
    result.error_code = -101;
    result.error_msg = strdup("Unknown error");
    return result;
  }
}

void gettx_free_result(gettx_result_t* result) {
  if (!result) {
    return;
  }
  if (result->json_data) {
    free(result->json_data);
    result->json_data = nullptr;
  }
  if (result->error_msg) {
    free(result->error_msg);
    result->error_msg = nullptr;
  }
}

void gettx_destroy(gettx_handle_t handle) {
  if (handle) {
    auto* lookup = static_cast<ton::gettx::TransactionLookup*>(handle);
    delete lookup;
  }
}

}  // extern "C"
