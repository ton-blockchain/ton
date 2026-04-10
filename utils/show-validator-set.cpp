/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <iomanip>
#include <iostream>
#include <string>

#include "crypto/block/mc-config.h"
#include "keys/keys.hpp"
#include "td/utils/OptionParser.h"
#include "td/utils/base64.h"
#include "td/utils/filesystem.h"
#include "vm/boc.h"

int main(int argc, char** argv) {
  std::string filename;
  ton::WorkchainId workchain = ton::workchainInvalid;
  td::uint64 shard = 0;
  ton::CatchainSeqno cc_seqno = 0;

  td::OptionParser p;
  p.set_description("Display validator set for a given shard from an MC key block BOC file");
  p.add_option('h', "help", "print help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, sizeof(b)});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::_Exit(0);
  });
  p.add_option('f', "file", "masterchain key block BOC file", [&](td::Slice arg) { filename = arg.str(); });
  p.add_checked_option('w', "workchain", "workchain id", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(workchain, td::to_integer_safe<ton::WorkchainId>(arg));
    return td::Status::OK();
  });
  p.add_checked_option('s', "shard", "shard id (hex, e.g. 8000000000000000)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(shard, td::hex_to_integer_safe<td::uint64>(arg));
    return td::Status::OK();
  });
  p.add_checked_option('c', "cc-seqno", "catchain seqno", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(cc_seqno, td::to_integer_safe<ton::CatchainSeqno>(arg));
    return td::Status::OK();
  });

  auto status = p.run(argc, argv);
  if (status.is_error()) {
    std::cerr << "Error: " << status.move_as_error().to_string() << std::endl;
    return 1;
  }
  if (filename.empty() || workchain == ton::workchainInvalid) {
    std::cerr << "Usage: show-validator-set -f <key-block.boc> -w <workchain> -s <shard-hex> -c <cc-seqno>"
              << std::endl;
    return 1;
  }

  ton::ShardIdFull shard_id{workchain, static_cast<ton::ShardId>(shard)};

  auto data_r = td::read_file(filename);
  if (data_r.is_error()) {
    std::cerr << "Failed to read file: " << data_r.move_as_error().to_string() << std::endl;
    return 2;
  }

  auto root_r = vm::std_boc_deserialize(data_r.move_as_ok().as_slice());
  if (root_r.is_error()) {
    std::cerr << "Failed to deserialize BOC: " << root_r.move_as_error().to_string() << std::endl;
    return 2;
  }
  auto root = root_r.move_as_ok();

  auto config_r = block::Config::extract_from_key_block(root, block::Config::needValidatorSet);
  if (config_r.is_error()) {
    std::cerr << "Failed to extract config from key block: " << config_r.move_as_error().to_string() << std::endl;
    return 2;
  }
  auto config = config_r.move_as_ok();

  auto vset = config->get_cur_validator_set();
  if (!vset) {
    std::cerr << "No current validator set in config" << std::endl;
    return 2;
  }

  auto ccv_conf = config->get_catchain_validators_config();
  auto nodes = block::Config::do_compute_validator_set(ccv_conf, shard_id, *vset, cc_seqno);

  if (nodes.empty()) {
    std::cerr << "Empty validator set" << std::endl;
    return 2;
  }

  std::cout << "Validator set for " << shard_id.to_str() << " cc_seqno=" << cc_seqno << " (" << nodes.size()
            << " validators):" << std::endl;
  std::cout << std::left << std::setw(4) << "idx" << std::setw(46) << "pub_key_hash_b64" << std::setw(66) << "adnl_hash"
            << "weight" << std::endl;

  for (size_t i = 0; i < nodes.size(); i++) {
    const auto& node = nodes[i];
    auto pubkey = ton::PublicKey{ton::pubkeys::Ed25519{node.key.as_bits256()}};
    auto short_id = pubkey.compute_short_id();
    auto hash = short_id.bits256_value();

    std::cout << std::left << std::setw(4) << i << std::setw(46) << td::base64_encode(hash.as_slice()) << std::setw(66)
              << node.addr.to_hex() << node.weight << std::endl;
  }

  return 0;
}
