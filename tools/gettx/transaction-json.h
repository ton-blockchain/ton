#pragma once

#include "transaction-lookup.h"
#include "td/utils/JsonBuilder.h"
#include <functional>

namespace ton {
namespace gettx {

/**
 * Helper function to serialize a transaction to JSON
 * Used by both main.cpp and gettx_api.cpp to avoid code duplication
 */
inline void serialize_transaction(td::JsonValueScope&& value_scope, const TransactionLookup::Transaction& tx) {
  value_scope << td::json_object([&](auto& tx_obj) {
    tx_obj("transaction_id", td::json_object([&](auto& tx_id_obj) {
      tx_id_obj("account", tx.account_addr.to_hex());
      tx_id_obj("lt", td::JsonLong(static_cast<td::int64>(tx.lt)));
      tx_id_obj("hash", tx.hash.to_hex());
    }));

    // workchain (if non-zero)
    if (tx.workchain != 0) {
      tx_obj("workchain", tx.workchain);
    }

    // other fields
    tx_obj("fee", td::JsonLong(static_cast<td::int64>(tx.total_fees)));
    tx_obj("utime", td::JsonLong(static_cast<td::int64>(tx.utime)));

    // in_msg
    if (!tx.in_msg.empty()) {
      tx_obj("in_msg", td::base64_encode(tx.in_msg.as_slice()));
    } else {
      tx_obj("in_msg", td::JsonNull());
    }

    // out_msgs
    tx_obj("out_msgs", td::json_array([&](auto& out_msgs_array) {
      for (const auto& msg : tx.out_msgs) {
        out_msgs_array(td::base64_encode(msg.as_slice()));
      }
    }));

    // transaction data
    tx_obj("data", td::base64_encode(tx.data.as_slice()));
    tx_obj("block", tx.block_id.to_str());
  });
}

}  // namespace gettx
}  // namespace ton
