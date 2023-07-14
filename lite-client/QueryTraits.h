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
*/
#pragma once
#include "ton/ton-types.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/lite_api.hpp"
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "auto/tl/lite_api.hpp"

namespace liteclient {

template <typename Query>
struct QueryTraits {
  static ton::ShardIdFull get_shard(const Query& q) {
    return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getMasterchainInfo> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getMasterchainInfo& q) {
    return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getMasterchainInfoExt> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getMasterchainInfoExt& q) {
    return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getTime> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getTime& q) {
    return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getVersion> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getVersion& q) {
    return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getBlock> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getBlock& q) {
    return ton::ShardIdFull(q.id_->workchain_, q.id_->shard_);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getState> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getState& q) {
    return ton::ShardIdFull(q.id_->workchain_, q.id_->shard_);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getBlockHeader> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getBlockHeader& q) {
    return ton::ShardIdFull(q.id_->workchain_, q.id_->shard_);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_sendMessage> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_sendMessage& q) {
    auto shard = [&]() -> td::Result<ton::ShardIdFull> {
      vm::BagOfCells boc;
      TRY_STATUS(boc.deserialize(q.body_.as_slice()));
      if (boc.get_root_count() != 1) {
        return td::Status::Error("external message is not a valid bag of cells");
      }
      block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
      if (!tlb::unpack_cell_inexact(boc.get_root_cell(), info)) {
        return td::Status::Error("cannot unpack external message header");
      }
      auto dest_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.dest);
      if (!dest_prefix.is_valid()) {
        return td::Status::Error("destination of an inbound external message is an invalid blockchain address");
      }
      return dest_prefix.as_leaf_shard();
    }();
    if (shard.is_error()) {
      LOG(DEBUG) << "Failed to get shard from query liteServer.sendMessage: " << shard.move_as_error();
      return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
    }
    return shard.move_as_ok();
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getAccountState> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getAccountState& q) {
    return ton::AccountIdPrefixFull(q.account_->workchain_, q.account_->id_.bits().get_uint(64)).as_leaf_shard();
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getAccountStatePrunned> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getAccountStatePrunned& q) {
    return ton::AccountIdPrefixFull(q.account_->workchain_, q.account_->id_.bits().get_uint(64)).as_leaf_shard();
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_runSmcMethod> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_runSmcMethod& q) {
    return ton::AccountIdPrefixFull(q.account_->workchain_, q.account_->id_.bits().get_uint(64)).as_leaf_shard();
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getShardInfo> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getShardInfo& q) {
    return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getAllShardsInfo> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getAllShardsInfo& q) {
    return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getOneTransaction> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getOneTransaction& q) {
    return ton::AccountIdPrefixFull(q.account_->workchain_, q.account_->id_.bits().get_uint(64)).as_leaf_shard();
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getTransactions> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getTransactions& q) {
    return ton::AccountIdPrefixFull(q.account_->workchain_, q.account_->id_.bits().get_uint(64)).as_leaf_shard();
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_lookupBlock> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_lookupBlock& q) {
    return ton::ShardIdFull(q.id_->workchain_, q.id_->shard_);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_listBlockTransactions> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_listBlockTransactions& q) {
    return ton::ShardIdFull(q.id_->workchain_, q.id_->shard_);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_listBlockTransactionsExt> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_listBlockTransactionsExt& q) {
    return ton::ShardIdFull(q.id_->workchain_, q.id_->shard_);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getBlockProof> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getBlockProof& q) {
    return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getConfigAll> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getConfigAll& q) {
    return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getConfigParams> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getConfigParams& q) {
    return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getValidatorStats> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getValidatorStats& q) {
    return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getLibraries> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getLibraries& q) {
    return ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
  }
};

template<>
struct QueryTraits<ton::lite_api::liteServer_getShardBlockProof> {
  static ton::ShardIdFull get_shard(const ton::lite_api::liteServer_getShardBlockProof& q) {
    return ton::ShardIdFull(q.id_->workchain_, q.id_->shard_);
  }
};

template<typename Query>
inline ton::ShardIdFull get_query_shard(const Query& q) {
  return QueryTraits<Query>::get_shard(q);
}

}  // namespace tonlib
