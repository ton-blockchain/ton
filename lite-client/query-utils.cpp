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
#include "query-utils.hpp"

#include "block-parse.h"
#include "td/utils/overloaded.h"
#include "tl-utils/common-utils.hpp"

#include "block/block-auto.h"
#include "auto/tl/lite_api.hpp"
#include "overlay/overlay-broadcast.hpp"
#include "tl-utils/lite-utils.hpp"
#include "ton/lite-tl.hpp"
#include "ton/ton-shard.h"

#include <ton/ton-tl.hpp>

namespace liteclient {

using namespace ton;

std::string QueryInfo::to_str() const {
  td::StringBuilder sb;
  sb << "[ " << lite_query_name_by_id(query_id) << " " << shard_id.to_str();
  switch (type) {
    case t_simple:
      break;
    case t_seqno:
      sb << " seqno=" << value;
      break;
    case t_utime:
      sb << " utime=" << value;
      break;
    case t_lt:
      sb << " lt=" << value;
      break;
    case t_mc_seqno:
      sb << " mc_seqno=" << value;
      break;
  }
  sb << " ]";
  return sb.as_cslice().str();
}

QueryInfo get_query_info(td::Slice data) {
  auto F = fetch_tl_object<lite_api::liteServer_query>(data, true);
  if (F.is_ok()) {
    data = F.ok()->data_;
  } else {
    fetch_tl_prefix<lite_api::liteServer_queryPrefix>(data, true).ignore();
  }
  fetch_tl_prefix<lite_api::liteServer_waitMasterchainSeqno>(data, true).ignore();
  auto Q = fetch_tl_object<lite_api::Function>(data, true);
  if (Q.is_error()) {
    return {};
  }
  return get_query_info(*Q.ok());
}

QueryInfo get_query_info(const lite_api::Function& f) {
  QueryInfo info;
  info.query_id = f.get_id();
  auto from_block_id = [&](const tl_object_ptr<lite_api::tonNode_blockIdExt>& id) {
    BlockIdExt block_id = create_block_id(id);
    info.shard_id = block_id.shard_full();
    info.type = QueryInfo::t_seqno;
    info.value = block_id.seqno();
  };
  downcast_call(
      const_cast<lite_api::Function&>(f),
      td::overloaded([&](const lite_api::liteServer_getTime& q) { /* t_simple */ },
                     [&](const lite_api::liteServer_getVersion& q) { /* t_simple */ },
                     [&](const lite_api::liteServer_getMasterchainInfo& q) { /* t_simple */ },
                     [&](const lite_api::liteServer_getMasterchainInfoExt& q) { /* t_simple */ },
                     [&](const lite_api::liteServer_getBlock& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_getBlockHeader& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_getState& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_getAccountState& q) {
                       BlockIdExt block_id = create_block_id(q.id_);
                       AccountIdPrefixFull acc_id_prefix = extract_addr_prefix(q.account_->workchain_, q.account_->id_);
                       info.shard_id = acc_id_prefix.as_leaf_shard();
                       // See LiteQuery::perform_getAccountState
                       if (block_id.id.workchain != masterchainId) {
                         info.type = QueryInfo::t_seqno;
                         info.value = block_id.seqno();
                       } else if (block_id.id.seqno != ~0U) {
                         info.type = QueryInfo::t_mc_seqno;
                         info.value = block_id.seqno();
                       } else {
                         info.type = QueryInfo::t_simple;
                       }
                     },
                     [&](const lite_api::liteServer_getAccountStatePrunned& q) {
                       BlockIdExt block_id = create_block_id(q.id_);
                       AccountIdPrefixFull acc_id_prefix = extract_addr_prefix(q.account_->workchain_, q.account_->id_);
                       info.shard_id = acc_id_prefix.as_leaf_shard();
                       // See LiteQuery::perform_getAccountState
                       if (block_id.id.workchain != masterchainId) {
                         info.type = QueryInfo::t_seqno;
                         info.value = block_id.seqno();
                       } else if (block_id.id.seqno != ~0U) {
                         info.type = QueryInfo::t_mc_seqno;
                         info.value = block_id.seqno();
                       } else {
                         info.type = QueryInfo::t_simple;
                       }
                     },
                     [&](const lite_api::liteServer_getOneTransaction& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_getTransactions& q) {
                       AccountIdPrefixFull acc_id_prefix = extract_addr_prefix(q.account_->workchain_, q.account_->id_);
                       info.shard_id = acc_id_prefix.as_leaf_shard();
                       info.type = QueryInfo::t_lt;
                       info.value = q.lt_;
                     },
                     [&](const lite_api::liteServer_sendMessage& q) {
                       info.type = QueryInfo::t_simple;
                       auto r_root = vm::std_boc_deserialize(q.body_);
                       if (r_root.is_error()) {
                         return;
                       }
                       block::gen::CommonMsgInfo::Record_ext_in_msg_info msg_info;
                       if (!tlb::unpack_cell_inexact(r_root.ok(), msg_info)) {
                         return;
                       }
                       auto dest_prefix = block::tlb::MsgAddressInt::get_prefix(msg_info.dest);
                       if (!dest_prefix.is_valid()) {
                         return;
                       }
                       info.shard_id = dest_prefix.as_leaf_shard();
                     },
                     [&](const lite_api::liteServer_getShardInfo& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_getAllShardsInfo& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_lookupBlock& q) {
                       BlockId block_id = create_block_id_simple(q.id_);
                       info.shard_id = block_id.shard_full();
                       // See LiteQuery::perform_lookupBlock
                       if (q.mode_ & 1) {
                         info.type = QueryInfo::t_seqno;
                         info.value = block_id.seqno;
                       } else if (q.mode_ == 2) {
                         info.type = QueryInfo::t_lt;
                         info.value = q.lt_;
                       } else if (q.mode_ == 4) {
                         info.type = QueryInfo::t_utime;
                         info.value = q.utime_;
                       }
                     },
                     [&](const lite_api::liteServer_lookupBlockWithProof& q) {
                       BlockId block_id = create_block_id_simple(q.id_);
                       info.shard_id = block_id.shard_full();
                       // See LiteQuery::perform_lookupBlockWithProof
                       if (q.mode_ & 1) {
                         info.type = QueryInfo::t_seqno;
                         info.value = block_id.seqno;
                       } else if (q.mode_ == 2) {
                         info.type = QueryInfo::t_lt;
                         info.value = q.lt_;
                       } else if (q.mode_ == 4) {
                         info.type = QueryInfo::t_utime;
                         info.value = q.utime_;
                       }
                     },
                     [&](const lite_api::liteServer_listBlockTransactions& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_listBlockTransactionsExt& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_getConfigParams& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_getConfigAll& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_getBlockProof& q) {
                       info.shard_id = ShardIdFull{masterchainId};
                       BlockIdExt from = create_block_id(q.known_block_);
                       // See LiteQuery::perform_getBlockProof
                       if ((q.mode_ & 1) && (q.mode_ & 0x1000)) {
                         BlockIdExt to = create_block_id(q.target_block_);  // target_block is non-null if (mode & 1)
                         info.type = QueryInfo::t_seqno;
                         info.value = std::max(from.seqno(), to.seqno());
                       } else {
                         info.type = QueryInfo::t_simple;
                       }
                     },
                     [&](const lite_api::liteServer_getValidatorStats& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_runSmcMethod& q) {
                       BlockIdExt block_id = create_block_id(q.id_);
                       AccountIdPrefixFull acc_id_prefix = extract_addr_prefix(q.account_->workchain_, q.account_->id_);
                       info.shard_id = acc_id_prefix.as_leaf_shard();
                       // See LiteQuery::perform_getAccountState
                       if (block_id.id.workchain != masterchainId) {
                         info.type = QueryInfo::t_seqno;
                         info.value = block_id.seqno();
                       } else if (block_id.id.seqno != ~0U) {
                         info.type = QueryInfo::t_mc_seqno;
                         info.value = block_id.seqno();
                       } else {
                         info.type = QueryInfo::t_simple;
                       }
                     },
                     [&](const lite_api::liteServer_getLibraries& q) { /* t_simple */ },
                     [&](const lite_api::liteServer_getLibrariesWithProof& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_getShardBlockProof& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_nonfinal_getCandidate& q) { /* t_simple */ },
                     [&](const lite_api::liteServer_nonfinal_getValidatorGroups& q) { /* t_simple */ },
                     [&](const lite_api::liteServer_getOutMsgQueueSizes& q) {
                       // This query is expected to be removed, as it is not fully compatible with separated liteservers
                       /* t_simple */
                     },
                     [&](const lite_api::liteServer_getBlockOutMsgQueueSize& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_getDispatchQueueInfo& q) { from_block_id(q.id_); },
                     [&](const lite_api::liteServer_getDispatchQueueMessages& q) { from_block_id(q.id_); },
                     [&](const auto&) { /* t_simple */ }));
  if (info.shard_id.workchain == masterchainId) {
    info.shard_id.shard = shardIdAll;
  }
  if (!info.shard_id.is_valid_ext()) {
    info.shard_id = ShardIdFull{masterchainId};
    info.type = QueryInfo::t_simple;
    info.value = 0;
  }
  return info;
}

bool LiteServerConfig::accepts_query(const QueryInfo& query_info) const {
  if (is_full) {
    return true;
  }
  for (const Slice& s : slices) {
    if (s.accepts_query(query_info)) {
      return true;
    }
  }
  return false;
}

bool LiteServerConfig::Slice::accepts_query(const QueryInfo& query_info) const {
  if (unlimited) {
    for (const ShardInfo& shard : shards_from) {
      if (shard_intersects(shard.shard_id, query_info.shard_id)) {
        return true;
      }
    }
    return false;
  }
  if (!shards_from.empty()) {
    bool from_ok = false;
    DCHECK(shards_from[0].shard_id.is_masterchain());
    for (const ShardInfo& shard : shards_from) {
      if (shard_intersects(shard.shard_id, query_info.shard_id)) {
        switch (query_info.type) {
          case QueryInfo::t_simple:
            from_ok = true;
            break;
          case QueryInfo::t_seqno:
            from_ok = shard.seqno <= query_info.value;
            break;
          case QueryInfo::t_utime:
            from_ok = shard.utime <= query_info.value;
            break;
          case QueryInfo::t_lt:
            from_ok = shard.lt <= query_info.value;
            break;
          case QueryInfo::t_mc_seqno:
            from_ok = shards_from[0].seqno <= query_info.value;
            break;
        }
        if (from_ok) {
          break;
        }
      }
    }
    if (!from_ok) {
      return false;
    }
  }
  if (!shards_to.empty()) {
    bool to_ok = false;
    DCHECK(shards_to[0].shard_id.is_masterchain());
    for (const ShardInfo& shard : shards_to) {
      if (shard_intersects(shard.shard_id, query_info.shard_id)) {
        switch (query_info.type) {
          case QueryInfo::t_simple:
            break;
          case QueryInfo::t_seqno:
            to_ok = shard.seqno >= query_info.value;
            break;
          case QueryInfo::t_utime:
            to_ok = shard.utime >= query_info.value;
            break;
          case QueryInfo::t_lt:
            to_ok = shard.lt >= query_info.value;
            break;
          case QueryInfo::t_mc_seqno:
            to_ok = shards_from[0].seqno >= query_info.value;
            break;
        }
        if (to_ok) {
          break;
        }
      }
    }
    if (!to_ok) {
      return false;
    }
  }
  return true;
}

td::Result<std::vector<LiteServerConfig>> LiteServerConfig::parse_global_config(
    const ton_api::liteclient_config_global& config) {
  std::vector<LiteServerConfig> servers;
  for (const auto& f : config.liteservers_) {
    LiteServerConfig server;
    TRY_STATUS(server.addr.init_host_port(td::IPAddress::ipv4_to_str(f->ip_), f->port_));
    server.adnl_id = adnl::AdnlNodeIdFull{PublicKey{f->id_}};
    server.is_full = true;
    servers.push_back(std::move(server));
  }
  for (const auto& f : config.liteservers_v2_) {
    LiteServerConfig server;
    TRY_STATUS(server.addr.init_host_port(td::IPAddress::ipv4_to_str(f->ip_), f->port_));
    server.adnl_id = adnl::AdnlNodeIdFull{PublicKey{f->id_}};
    server.is_full = false;
    for (const auto& slice_obj : f->slices_) {
      Slice slice;
      td::Status S = td::Status::OK();
      downcast_call(*slice_obj,
                    td::overloaded(
                        [&](const ton_api::liteserver_descV2_sliceSimple& s) {
                          slice.unlimited = true;
                          slice.shards_from.push_back({ShardIdFull{masterchainId}, 0, 0, 0});
                          for (const auto& shard_obj : s.shards_) {
                            ShardIdFull shard_id = create_shard_id(shard_obj);
                            if (!shard_id.is_valid_ext()) {
                              S = td::Status::Error(PSTRING() << "invalid shard id " << shard_id.to_str());
                              break;
                            }
                            if (!shard_id.is_masterchain()) {
                              slice.shards_from.push_back({shard_id, 0, 0, 0});
                            }
                          }
                        },
                        [&](const ton_api::liteserver_descV2_sliceTimed& s) {
                          auto parse_shards =
                              [](const std::vector<tl_object_ptr<ton_api::liteserver_descV2_shardInfo>>& shard_objs,
                                 std::vector<ShardInfo>& shards) -> td::Status {
                            if (shard_objs.empty()) {
                              return td::Status::OK();
                            }
                            size_t i = 0;
                            int mc_idx = -1;
                            for (const auto& shard_obj : shard_objs) {
                              ShardIdFull shard_id = create_shard_id(shard_obj->shard_id_);
                              if (!shard_id.is_valid_ext()) {
                                return td::Status::Error(PSTRING() << "invalid shard id " << shard_id.to_str());
                              }
                              if (shard_id.is_masterchain()) {
                                shard_id = ShardIdFull{masterchainId};
                                if (mc_idx != -1) {
                                  return td::Status::Error("duplicate masterchain shard in sliceTimed");
                                }
                                mc_idx = (int)i;
                              }
                              shards.push_back({shard_id, (BlockSeqno)shard_obj->seqno_, (UnixTime)shard_obj->utime_,
                                                (LogicalTime)shard_obj->lt_});
                              ++i;
                            }
                            if (mc_idx == -1) {
                              return td::Status::Error("no masterchain shard in sliceTimed");
                            }
                            std::swap(shards[0], shards[mc_idx]);
                            return td::Status::OK();
                          };
                          S = parse_shards(s.shards_from_, slice.shards_from);
                          if (S.is_ok()) {
                            S = parse_shards(s.shards_to_, slice.shards_to);
                          }
                          if (S.is_ok() && slice.shards_from.empty() && slice.shards_to.empty()) {
                            S = td::Status::Error("shards_from and shards_to are both empty");
                          }
                        }));
      TRY_STATUS(std::move(S));
      server.slices.push_back(slice);
    }

    servers.push_back(std::move(server));
  }
  return servers;
}

}  // namespace liteclient