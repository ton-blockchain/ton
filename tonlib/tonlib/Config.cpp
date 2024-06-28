/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "Config.h"
#include "adnl/adnl-node-id.hpp"
#include "td/utils/JsonBuilder.h"
#include "auto/tl/ton_api_json.h"
#include "ton/ton-tl.hpp"

namespace tonlib {
td::Result<ton::BlockIdExt> parse_block_id_ext(td::JsonObject &obj) {
  ton::WorkchainId zero_workchain_id;
  {
    TRY_RESULT(wc, td::get_json_object_int_field(obj, "workchain"));
    zero_workchain_id = wc;
  }
  ton::ShardId zero_shard_id;  // uint64
  {
    TRY_RESULT(shard_id, td::get_json_object_long_field(obj, "shard"));
    zero_shard_id = static_cast<ton::ShardId>(shard_id);
  }
  ton::BlockSeqno zero_seqno;
  {
    TRY_RESULT(seqno, td::get_json_object_int_field(obj, "seqno"));
    zero_seqno = seqno;
  }

  ton::RootHash zero_root_hash;
  {
    TRY_RESULT(hash_b64, td::get_json_object_string_field(obj, "root_hash"));
    TRY_RESULT(hash, td::base64_decode(hash_b64));
    if (hash.size() * 8 != ton::RootHash::size()) {
      return td::Status::Error("Invalid config (8)");
    }
    zero_root_hash = ton::RootHash(td::ConstBitPtr(td::Slice(hash).ubegin()));
  }
  ton::FileHash zero_file_hash;
  {
    TRY_RESULT(hash_b64, td::get_json_object_string_field(obj, "file_hash"));
    TRY_RESULT(hash, td::base64_decode(hash_b64));
    if (hash.size() * 8 != ton::FileHash::size()) {
      return td::Status::Error("Invalid config (9)");
    }
    zero_file_hash = ton::RootHash(td::ConstBitPtr(td::Slice(hash).ubegin()));
  }

  return ton::BlockIdExt(zero_workchain_id, zero_shard_id, zero_seqno, std::move(zero_root_hash),
                         std::move(zero_file_hash));
}
td::Result<Config> Config::parse(std::string str) {
  TRY_RESULT(json, td::json_decode(str));
  if (json.type() != td::JsonValue::Type::Object) {
    return td::Status::Error("Invalid config: json is not an object");
  }
  Config res;
  ton::ton_api::liteclient_config_global conf;
  TRY_STATUS(ton::ton_api::from_json(conf, json.get_object()));
  TRY_RESULT_ASSIGN(res.lite_servers, liteclient::LiteServerConfig::parse_global_config(conf));

  if (!conf.validator_) {
    return td::Status::Error("Invalid config: no 'validator' section");
  }
  if (!conf.validator_->zero_state_) {
    return td::Status::Error("Invalid config: no zerostate");
  }
  res.zero_state_id = ton::create_block_id(conf.validator_->zero_state_);
  if (conf.validator_->init_block_) {
    res.init_block_id = ton::create_block_id(conf.validator_->init_block_);
  }

  for (auto &fork : conf.validator_->hardforks_) {
    res.hardforks.push_back(ton::create_block_id(fork));
  }

  for (auto hardfork : res.hardforks) {
    if (!res.init_block_id.is_valid() || hardfork.seqno() > res.init_block_id.seqno()) {
      LOG(INFO) << "Replace init_block with hardfork: " << res.init_block_id.to_str() << " -> " << hardfork.to_str();
      res.init_block_id = hardfork;
    }
  }

  return res;
}
}  // namespace tonlib
