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
#include <algorithm>
#include <memory>

#include "common/delay.h"
#include "impl/out-msg-queue-proof.hpp"
#include "interfaces/validator-full-id.h"
#include "net/download-archive-slice.hpp"
#include "net/download-block-new.hpp"
#include "net/download-next-blocks.hpp"
#include "net/download-proof.hpp"
#include "net/download-state.hpp"
#include "net/get-next-key-blocks.hpp"
#include "td/actor/MultiPromise.h"
#include "td/actor/coro_utils.h"
#include "td/utils/Random.h"
#include "ton/ton-io.hpp"
#include "ton/ton-tl.hpp"

#include "full-node.h"
#include "full-node.hpp"

DEFINE_LOG_CATEGORY(full_node, VERBOSITY_NAME(INFO))

namespace ton {

namespace validator {

namespace fullnode {

static const double INACTIVE_SHARD_TTL = (double)overlay::Overlays::overlay_peer_ttl() + 60.0;

void FullNodeImpl::add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  if (local_keys_.count(key)) {
    promise.set_value(td::Unit());
    return;
  }

  local_keys_.insert(key);
  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }

  if (!sign_cert_by_.is_zero()) {
    promise.set_value(td::Unit());
    return;
  }

  for (auto &x : all_validators_) {
    if (x == key) {
      sign_cert_by_ = key;
    }
  }

  for (auto &shard : shards_) {
    if (!shard.second.actor.empty()) {
      td::actor::send_closure(shard.second.actor, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
    }
  }
  promise.set_value(td::Unit());
}

void FullNodeImpl::del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  if (!local_keys_.count(key)) {
    promise.set_value(td::Unit());
    return;
  }
  local_keys_.erase(key);
  update_validator_telemetry_collector();
  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }

  if (sign_cert_by_ != key) {
    promise.set_value(td::Unit());
    return;
  }
  sign_cert_by_ = PublicKeyHash::zero();

  for (auto &x : all_validators_) {
    if (local_keys_.count(x)) {
      sign_cert_by_ = x;
    }
  }

  for (auto &shard : shards_) {
    if (!shard.second.actor.empty()) {
      td::actor::send_closure(shard.second.actor, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
    }
  }
  promise.set_value(td::Unit());
}

void FullNodeImpl::add_collator_adnl_id(adnl::AdnlNodeIdShort id) {
  ++local_collator_nodes_[id];
}

void FullNodeImpl::del_collator_adnl_id(adnl::AdnlNodeIdShort id) {
  if (--local_collator_nodes_[id] == 0) {
    local_collator_nodes_.erase(id);
  }
}

void FullNodeImpl::sign_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key, td::uint32 expiry_at,
                                                  td::uint32 max_size, td::Promise<td::BufferSlice> promise) {
  auto it = shards_.find(shard_id);
  if (it == shards_.end() || it->second.actor.empty()) {
    promise.set_error(td::Status::Error(ErrorCode::error, "shard not found"));
    return;
  }
  td::actor::send_closure(it->second.actor, &FullNodeShard::sign_overlay_certificate, signed_key, expiry_at, max_size,
                          std::move(promise));
}

void FullNodeImpl::import_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                                    std::shared_ptr<ton::overlay::Certificate> cert,
                                                    td::Promise<td::Unit> promise) {
  auto it = shards_.find(shard_id);
  if (it == shards_.end() || it->second.actor.empty()) {
    promise.set_error(td::Status::Error(ErrorCode::error, "shard not found"));
    return;
  }
  td::actor::send_closure(it->second.actor, &FullNodeShard::import_overlay_certificate, signed_key, cert,
                          std::move(promise));
}

void FullNodeImpl::update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) {
  adnl_id_ = adnl_id;

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  for (auto &s : shards_) {
    if (!s.second.actor.empty()) {
      td::actor::send_closure(s.second.actor, &FullNodeShard::update_adnl_id, adnl_id, ig.get_promise());
    }
  }

  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }
}

void FullNodeImpl::set_config(FullNodeConfig config) {
  opts_.config_ = config;
  for (auto &s : shards_) {
    if (!s.second.actor.empty()) {
      td::actor::send_closure(s.second.actor, &FullNodeShard::set_config, config);
    }
  }
  for (auto &overlay : custom_overlays_) {
    for (auto &actor : overlay.second.actors_) {
      td::actor::send_closure(actor.second, &FullNodeCustomOverlay::set_config, config);
    }
  }
}

void FullNodeImpl::add_custom_overlay(CustomOverlayParams params, td::Promise<td::Unit> promise) {
  if (params.nodes_.empty()) {
    promise.set_error(td::Status::Error("list of nodes is empty"));
    return;
  }
  std::string name = params.name_;
  if (custom_overlays_.count(name)) {
    promise.set_error(td::Status::Error(PSTRING() << "duplicate custom overlay name \"" << name << "\""));
    return;
  }
  VLOG(full_node, WARNING) << "Adding custom overlay \"" << name << "\", " << params.nodes_.size() << " nodes";
  auto &p = custom_overlays_[name];
  p.params_ = std::move(params);
  update_custom_overlay(p);
  promise.set_result(td::Unit());
}

void FullNodeImpl::del_custom_overlay(std::string name, td::Promise<td::Unit> promise) {
  auto it = custom_overlays_.find(name);
  if (it == custom_overlays_.end()) {
    promise.set_error(td::Status::Error(PSTRING() << "no such overlay \"" << name << "\""));
    return;
  }
  custom_overlays_.erase(it);
  promise.set_result(td::Unit());
}

void FullNodeImpl::initial_read_complete(BlockHandle top_handle) {
  CHECK(!handle_);
  handle_ = std::move(top_handle);
  sync_promise_ = [SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &FullNodeImpl::sync_completed);
  };
  get_next_blocks_loop().start().detach_ensure("get_next_blocks_loop");

  sync_completed_at_ = td::Timestamp::in(opts_.initial_sync_delay_);
  alarm_timestamp().relax(sync_completed_at_);
}

void FullNodeImpl::on_new_masterchain_block(td::Ref<MasterchainState> state, std::set<ShardIdFull> shards_to_monitor) {
  if (!client_.empty()) {
    return;
  }
  CHECK(shards_to_monitor.count(ShardIdFull(masterchainId)));
  bool join_all_overlays = !sign_cert_by_.is_zero();
  std::set<ShardIdFull> all_shards;
  std::set<ShardIdFull> new_active;
  all_shards.insert(ShardIdFull(masterchainId));
  std::set<WorkchainId> workchains;
  wc_monitor_min_split_ = state->monitor_min_split_depth(basechainId);
  auto cut_shard = [&](ShardIdFull shard) -> ShardIdFull {
    return wc_monitor_min_split_ < shard.pfx_len() ? shard_prefix(shard, wc_monitor_min_split_) : shard;
  };
  for (auto &info : state->get_shards()) {
    workchains.insert(info->shard().workchain);
    ShardIdFull shard = cut_shard(info->shard());
    while (true) {
      all_shards.insert(shard);
      if (shard.pfx_len() == 0) {
        break;
      }
      shard = shard_parent(shard);
    }
  }
  for (const auto &[wc, winfo] : state->get_workchain_list()) {
    if (!workchains.contains(wc) && winfo->active && winfo->enabled_since <= state->get_unix_time()) {
      all_shards.insert(ShardIdFull(wc));
    }
  }
  for (ShardIdFull shard : shards_to_monitor) {
    shard = cut_shard(shard);
    while (true) {
      new_active.insert(shard);
      if (shard.pfx_len() == 0) {
        break;
      }
      shard = shard_parent(shard);
    }
  }

  for (auto it = shards_.begin(); it != shards_.end();) {
    if (all_shards.contains(it->first)) {
      ++it;
    } else {
      it = shards_.erase(it);
    }
  }
  for (ShardIdFull shard : all_shards) {
    bool active = new_active.contains(shard);
    bool overlay_exists = !shards_[shard].actor.empty();
    if (active || join_all_overlays || overlay_exists) {
      update_shard_actor(shard, active);
    }
  }

  for (auto &[_, shard_info] : shards_) {
    if (!shard_info.active && shard_info.delete_at && shard_info.delete_at.is_in_past() && !join_all_overlays) {
      shard_info.actor = {};
      shard_info.delete_at = td::Timestamp::never();
    }
  }

  std::set<adnl::AdnlNodeIdShort> my_adnl_ids;
  my_adnl_ids.insert(adnl_id_);
  for (const auto &[adnl_id, _] : local_collator_nodes_) {
    my_adnl_ids.insert(adnl_id);
  }
  for (auto key : local_keys_) {
    auto it = current_validators_.find(key);
    if (it != current_validators_.end()) {
      my_adnl_ids.insert(it->second);
    }
  }
  std::set<ShardIdFull> monitoring_shards;
  for (ShardIdFull shard : shards_to_monitor) {
    monitoring_shards.insert(cut_shard(shard));
  }
  fast_sync_overlays_.update_overlays(state, std::move(my_adnl_ids), std::move(monitoring_shards),
                                      zero_state_file_hash_, opts_.fast_sync_broadcast_speed_multiplier_, keyring_,
                                      adnl_, rldp2_, quic_, overlays_, validator_manager_, actor_id(this));
  update_validator_telemetry_collector();
  update_plumtree_stats_collector();
}

void FullNodeImpl::update_shard_actor(ShardIdFull shard, bool active) {
  CHECK(client_.empty());
  ShardInfo &info = shards_[shard];
  if (info.actor.empty()) {
    info.actor = FullNodeShard::create(shard, local_id_, adnl_id_, zero_state_file_hash_, opts_, keyring_, adnl_,
                                       rldp2_, quic_, overlays_, validator_manager_, actor_id(this), active);
    if (!all_validators_.empty()) {
      td::actor::send_closure(info.actor, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
    }
  } else if (info.active != active) {
    td::actor::send_closure(info.actor, &FullNodeShard::set_active, active);
  }
  info.active = active;
  info.delete_at = active ? td::Timestamp::never() : td::Timestamp::in(INACTIVE_SHARD_TTL);
}

void FullNodeImpl::sync_completed() {
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::sync_complete, [](td::Result<>) {});
}

td::actor::Task<> FullNodeImpl::send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data) {
  if (!client_.empty()) {
    auto query_sender = co_await get_query_sender(dst.as_leaf_shard());
    co_await query_sender->send_query(create_serialize_tl_object<ton_api::tonNode_slave_sendExtMessage>(
                                          create_tl_object<ton_api::tonNode_externalMessage>(std::move(data))),
                                      td::Timestamp::in(1.0), 1024);
    co_return {};
  }
  bool skip_public = false;
  for (auto &[_, private_overlay] : custom_overlays_) {
    if (private_overlay.params_.send_shard(dst.as_leaf_shard())) {
      for (auto &[local_id, actor] : private_overlay.actors_) {
        if (private_overlay.params_.msg_senders_.contains(local_id)) {
          td::actor::send_closure(actor, &FullNodeCustomOverlay::send_external_message, data.clone());
          if (private_overlay.params_.skip_public_msg_send_) {
            skip_public = true;
          }
        }
      }
    }
  }
  if (skip_public || opts_.config_.ext_messages_broadcast_disabled_) {
    co_return {};
  }
  auto shard = get_shard_overlay_actor(dst);
  if (shard.empty()) {
    VLOG(full_node, WARNING) << "dropping OUT ext message to unknown shard";
    co_return {};
  }
  td::actor::send_closure(shard, &FullNodeShard::send_external_message, std::move(data));
  co_return {};
}

void FullNodeImpl::send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  if (!client_.empty()) {
    VLOG(full_node, WARNING) << "dropping OUT shard block info message: full-node is in slave mode";
    return;
  }
  send_shard_block_info_to_custom_overlays(block_id, cc_seqno, data);
  auto shard = get_shard_overlay_actor(ShardIdFull{masterchainId});
  if (shard.empty()) {
    VLOG(full_node, WARNING) << "dropping OUT shard block info message to unknown shard";
    return;
  }
  auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(ShardIdFull(masterchainId), true).first;
  if (!fast_sync_overlay.empty()) {
    td::actor::send_closure(fast_sync_overlay, &FullNodeFastSyncOverlay::send_shard_block_info, block_id, cc_seqno,
                            data.clone());
  }
  td::actor::send_closure(shard, &FullNodeShard::send_shard_block_info, block_id, cc_seqno, std::move(data));
}

void FullNodeImpl::send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                        td::BufferSlice data, int mode) {
  if (!client_.empty()) {
    VLOG(full_node, WARNING) << "dropping OUT block candidate broadcast: full-node is in slave mode";
    return;
  }
  if (mode & broadcast_mode_custom) {
    send_block_candidate_broadcast_to_custom_overlays(block_id, cc_seqno, validator_set_hash, data);
  }
  if (mode & broadcast_mode_fast_sync) {
    auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(block_id.shard_full(), true).first;
    if (!fast_sync_overlay.empty()) {
      td::actor::send_closure(fast_sync_overlay, &FullNodeFastSyncOverlay::send_block_candidate, block_id, cc_seqno,
                              validator_set_hash, data.clone());
    }
  }
  if (mode & broadcast_mode_public) {
    auto shard = get_shard_overlay_actor(block_id.shard_full());
    if (shard.empty()) {
      VLOG(full_node, WARNING) << "dropping OUT Plumtree block candidate message to unknown shard";
    } else {
      td::actor::send_closure(shard, &FullNodeShard::send_block_candidate, block_id, cc_seqno, validator_set_hash,
                              std::move(data));
    }
  }
}

void FullNodeImpl::send_out_msg_queue_proof_broadcast(td::Ref<OutMsgQueueProofBroadcast> broadcast) {
  auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(broadcast->dst_shard).first;
  if (!fast_sync_overlay.empty()) {
    td::actor::send_closure(fast_sync_overlay, &FullNodeFastSyncOverlay::send_out_msg_queue_proof_broadcast,
                            std::move(broadcast));
  }
}

void FullNodeImpl::send_block_finality_broadcast(BlockFinalityBroadcast finality, int mode) {
  if (mode & broadcast_mode_custom) {
    send_block_finality_broadcast_to_custom_overlays(finality);
  }
  if (mode & broadcast_mode_fast_sync) {
    auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(finality.block_id.shard_full(), true).first;
    if (!fast_sync_overlay.empty()) {
      td::actor::send_closure(fast_sync_overlay, &FullNodeFastSyncOverlay::send_block_finality_broadcast,
                              finality.clone());
    }
  }
  if (mode & broadcast_mode_public) {
    auto shard = get_shard_overlay_actor(finality.block_id.shard_full());
    if (shard.empty()) {
      VLOG(full_node, WARNING) << "dropping OUT block finality broadcast to unknown shard";
      return;
    }
    td::actor::send_closure(shard, &FullNodeShard::send_block_finality_broadcast, std::move(finality));
  }
}

static QuerySender get_empty_query_sender() {
  // Some full-node queries sometimes don't actually call send_query,
  // so we give this sender instead of failing in get_query_sender
  class QuerySenderEmptyImpl : public QuerySenderInterface {
   public:
    void send_query(td::BufferSlice query, td::Timestamp timeout, td::uint64 max_answer_size,
                    td::Promise<td::BufferSlice> promise) const override {
      promise.set_error(td::Status::Error(ErrorCode::notready, "no nodes"));
    }

    std::string to_str() const override {
      return "unknown";
    }
  };
  static auto sender = std::make_shared<QuerySenderEmptyImpl>();
  return sender;
}

td::actor::Task<QuerySender> FullNodeImpl::get_query_sender(ShardIdFull shard_id, bool historical) {
  if (!client_.empty()) {
    class QuerySenderMasterImpl : public QuerySenderInterface {
     public:
      QuerySenderMasterImpl(td::actor::ActorId<adnl::AdnlExtClient> client,
                            std::pair<td::uint32, td::uint32> proto_version)
          : client_(std::move(client)), proto_version_(proto_version) {
      }

      void send_query(td::BufferSlice query, td::Timestamp timeout, td::uint64 max_answer_size,
                      td::Promise<td::BufferSlice> promise) const override {
        td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "q",
                                create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(query)), timeout,
                                promise.wrap([=](td::BufferSlice &&res) -> td::Result<td::BufferSlice> {
                                  if (res.size() > max_answer_size) {
                                    return td::Status::Error("too big answer");
                                  }
                                  return std::move(res);
                                }));
      }

      std::string to_str() const override {
        return PSTRING() << "full-node master";
      }

      std::pair<td::uint32, td::uint32> get_proto_version() const override {
        return proto_version_;
      }

     private:
      td::actor::ActorId<adnl::AdnlExtClient> client_;
      std::pair<td::uint32, td::uint32> proto_version_;
    };
    if (!client_query_sender_) {
      auto query = create_serialize_tl_object<ton_api::tonNode_getCapabilities>();
      auto response = co_await td::actor::ask(
          client_, &adnl::AdnlExtClient::send_query, "q",
          create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(query)), td::Timestamp::in(2.0));
      auto capabilities = CO_TRY(fetch_tl_object<ton_api::tonNode_capabilities>(response, true));
      std::pair<td::uint32, td::uint32> proto_version{capabilities->version_major_, capabilities->version_minor_};
      client_query_sender_ = std::make_shared<QuerySenderMasterImpl>(client_, proto_version);
    }
    co_return client_query_sender_;
  }

  {
    std::vector<td::actor::ActorId<FullNodeCustomOverlay>> overlays;
    for (auto &[_, overlay] : custom_overlays_) {
      if (overlay.params_.send_queries_) {
        for (auto &[_, actor] : overlay.actors_) {
          overlays.push_back(actor.get());
        }
      }
    }
    for (auto &actor : overlays) {
      auto R = co_await td::actor::ask(actor, &FullNodeCustomOverlay::get_query_sender).wrap();
      if (R.is_ok()) {
        co_return R.move_as_ok();
      }
    }
  }

  auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(shard_id).first;
  if (!fast_sync_overlay.empty()) {
    auto R = co_await td::actor::ask(fast_sync_overlay, &FullNodeFastSyncOverlay::get_query_sender).wrap();
    if (R.is_ok()) {
      co_return R.move_as_ok();
    }
  }

  auto shard = get_shard_overlay_actor(shard_id, historical);
  if (shard.empty()) {
    co_return get_empty_query_sender();
  }
  auto R = co_await td::actor::ask(shard, &FullNodeShard::get_query_sender).wrap();
  if (R.is_error()) {
    co_return get_empty_query_sender();
  }
  co_return R.move_as_ok();
}

td::actor::Task<ReceivedBlock> FullNodeImpl::download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout) {
  auto query_sender = co_await get_query_sender(id.shard_full());
  auto [task, promise] = td::actor::StartedTask<ReceivedBlock>::make_bridge();
  td::actor::create_actor<DownloadBlockNew>(PSTRING() << "downloadreq" << id.id, id, query_sender, priority, timeout,
                                            validator_manager_, std::move(promise))
      .release();
  auto R = co_await std::move(task).wrap();
  query_sender->query_finished(R.as_status());
  co_return std::move(R);
}

td::actor::Task<td::BufferSlice> FullNodeImpl::download_zero_state(BlockIdExt id, td::uint32 priority,
                                                                   td::Timestamp timeout) {
  auto query_sender = co_await get_query_sender(id.shard_full());
  auto [task, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
  td::actor::create_actor<DownloadState>(PSTRING() << "downloadstatereq" << id.id, id, BlockIdExt{}, UnsplitStateType{},
                                         query_sender, priority, timeout, validator_manager_, std::move(promise))
      .release();
  auto R = co_await std::move(task).wrap();
  query_sender->query_finished(R.as_status());
  co_return std::move(R);
}

td::actor::Task<td::BufferSlice> FullNodeImpl::download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id,
                                                                         PersistentStateType type, td::uint32 priority,
                                                                         td::Timestamp timeout) {
  auto query_sender = co_await get_query_sender(id.shard_full(), /* historical = */ true);
  auto [task, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
  td::actor::create_actor<DownloadState>(PSTRING() << "downloadstatereq" << id.id, id, masterchain_block_id, type,
                                         query_sender, priority, timeout, validator_manager_, std::move(promise))
      .release();
  auto R = co_await std::move(task).wrap();
  query_sender->query_finished(R.as_status());
  co_return std::move(R);
}

td::actor::Task<td::BufferSlice> FullNodeImpl::download_block_proof(BlockIdExt block_id, td::uint32 priority,
                                                                    td::Timestamp timeout) {
  auto query_sender = co_await get_query_sender(block_id.shard_full());
  auto [task, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
  td::actor::create_actor<DownloadProof>(PSTRING() << "downloadproofreq" << block_id.id, block_id, false, false,
                                         query_sender, priority, timeout, validator_manager_, std::move(promise))
      .release();
  auto R = co_await std::move(task).wrap();
  query_sender->query_finished(R.as_status());
  co_return std::move(R);
}

td::actor::Task<td::BufferSlice> FullNodeImpl::download_block_proof_link(BlockIdExt block_id, td::uint32 priority,
                                                                         td::Timestamp timeout) {
  auto query_sender = co_await get_query_sender(block_id.shard_full());
  auto [task, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
  td::actor::create_actor<DownloadProof>(PSTRING() << "downloadproofreq" << block_id.id, block_id, true, false,
                                         query_sender, priority, timeout, validator_manager_, std::move(promise))
      .release();
  auto R = co_await std::move(task).wrap();
  query_sender->query_finished(R.as_status());
  co_return std::move(R);
}

td::actor::Task<std::vector<BlockIdExt>> FullNodeImpl::get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout) {
  auto query_sender = co_await get_query_sender(block_id.shard_full());
  auto [task, promise] = td::actor::StartedTask<std::vector<BlockIdExt>>::make_bridge();
  td::actor::create_actor<GetNextKeyBlocks>(PSTRING() << "getnextkeyblocks" << block_id.id, block_id, 16, query_sender,
                                            1, timeout, validator_manager_, std::move(promise))
      .release();
  auto R = co_await std::move(task).wrap();
  query_sender->query_finished(R.as_status());
  co_return std::move(R);
}

td::actor::Task<std::string> FullNodeImpl::download_archive(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix,
                                                            std::string tmp_dir, td::Timestamp timeout) {
  auto query_sender = co_await get_query_sender(shard_prefix, /* historical = */ true);
  auto [task, promise] = td::actor::StartedTask<std::string>::make_bridge();
  td::actor::create_actor<DownloadArchiveSlice>(
      PSTRING() << "downloadarchive." << masterchain_seqno << "." << shard_prefix, masterchain_seqno, shard_prefix,
      std::move(tmp_dir), query_sender, timeout, validator_manager_, std::move(promise))
      .release();
  auto R = co_await std::move(task).wrap();
  query_sender->query_finished(R.as_status());
  co_return std::move(R);
}

td::actor::Task<> FullNodeImpl::get_next_blocks_loop() {
  CHECK(handle_);
  td::uint32 attempt = 0;
  while (true) {
    ++attempt;
    auto r_query_sender = co_await get_query_sender(ShardIdFull{masterchainId}).wrap();
    if (r_query_sender.is_error()) {
      VLOG(full_node, WARNING) << "Cannot get query sender: " << r_query_sender.move_as_error();
      co_await td::actor::coro_sleep(td::Timestamp::in(1.0));
      continue;
    }
    auto query_sender = r_query_sender.move_as_ok();
    auto [task, promise] = td::actor::StartedTask<BlockHandle>::make_bridge();
    td::actor::create_actor<DownloadNextBlocks>(PSTRING() << "downloadnextblocks" << handle_->id().id, handle_,
                                                query_sender, 1, validator_manager_, std::move(promise))
        .release();
    auto R = co_await std::move(task).wrap();
    // Do not penalize peer when it does not have next blocks if the last block is new enough
    if (R.is_error() && R.error().code() == ErrorCode::notready && handle_->inited_unix_time() &&
        handle_->unix_time() >= (UnixTime)td::Clocks::system() - 5) {
      query_sender->query_finished(td::Status::OK());
    } else {
      query_sender->query_finished(R.as_status());
    }
    if (R.is_error()) {
      auto S = R.move_as_error();
      if (S.code() != ErrorCode::notready && S.code() != ErrorCode::timeout) {
        VLOG(full_node, WARNING) << "failed to download next block after " << handle_->id() << ": " << S;
      } else {
        if ((attempt % 128) == 0) {
          VLOG(full_node, INFO) << "failed to download next block after " << handle_->id() << ": " << S;
        } else {
          VLOG(full_node, DEBUG) << "failed to download next block after " << handle_->id() << ": " << S;
        }
      }
      co_await td::actor::coro_sleep(td::Timestamp::in(0.1));
      continue;
    }
    attempt = 0;
    handle_ = R.move_as_ok();
    if (sync_promise_) {
      if (handle_->unix_time() > td::Clocks::system() - 300) {
        sync_promise_.set_value(td::Unit());
      } else {
        sync_completed_at_ = td::Timestamp::in(opts_.initial_sync_delay_);
        alarm_timestamp().relax(sync_completed_at_);
      }
    }
  }
}

td::actor::ActorId<FullNodeShard> FullNodeImpl::get_shard_overlay_actor(ShardIdFull shard, bool historical) {
  if (!client_.empty()) {
    return {};
  }
  if (shard.is_masterchain()) {
    return shards_[ShardIdFull{masterchainId}].actor.get();
  }
  if (shard.workchain != basechainId) {
    return {};
  }
  int pfx_len = shard.pfx_len();
  int min_split = wc_monitor_min_split_;
  if (historical) {
    min_split = td::Random::fast(0, min_split);
  }
  if (pfx_len > min_split) {
    shard = shard_prefix(shard, min_split);
  }
  while (true) {
    auto it = shards_.find(shard);
    if (it != shards_.end()) {
      update_shard_actor(shard, it->second.active);
      return it->second.actor.get();
    }
    if (shard.pfx_len() == 0) {
      break;
    }
    shard = shard_parent(shard);
  }

  // Special case if shards_ was not yet initialized.
  // This can happen briefly on node startup.
  return shards_[ShardIdFull{masterchainId}].actor.get();
}

td::actor::ActorId<FullNodeShard> FullNodeImpl::get_shard_overlay_actor(AccountIdPrefixFull dst) {
  return get_shard_overlay_actor(shard_prefix(dst, max_shard_pfx_len));
}

void FullNodeImpl::got_key_block_config(td::Ref<ConfigHolder> config) {
  PublicKeyHash l = PublicKeyHash::zero();
  std::vector<PublicKeyHash> keys;
  std::map<PublicKeyHash, adnl::AdnlNodeIdShort> current_validators;
  for (td::int32 i = -1; i <= 1; i++) {
    auto r = config->get_total_validator_set(i < 0 ? i : 1 - i);
    if (r.not_null()) {
      auto vec = r->export_vector();
      for (auto &el : vec) {
        auto key = ValidatorFullId{el.key}.compute_short_id();
        keys.push_back(key);
        if (local_keys_.count(key)) {
          l = key;
        }
        if (i == 1) {
          current_validators[key] = adnl::AdnlNodeIdShort{el.addr.is_zero() ? key.bits256_value() : el.addr};
        }
      }
    }
  }

  if (current_validators != current_validators_) {
    current_validators_ = std::move(current_validators);
    update_private_overlays();
  }

  // Let's turn off this optimization, since keyblocks are rare enough to update on each keyblock
  // if (keys == all_validators_) {
  //   return;
  // }

  all_validators_ = keys;
  sign_cert_by_ = l;
  CHECK(all_validators_.size() > 0);

  for (auto &shard : shards_) {
    if (!shard.second.actor.empty()) {
      td::actor::send_closure(shard.second.actor, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
    }
  }
}

void FullNodeImpl::new_key_block(BlockHandle handle) {
  if (handle->id().seqno() == 0) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        VLOG(full_node, WARNING) << "failed to get zero state: " << R.move_as_error();
      } else {
        auto s = td::Ref<MasterchainState>{R.move_as_ok()};
        CHECK(s.not_null());
        td::actor::send_closure(SelfId, &FullNodeImpl::got_key_block_config, s->get_config_holder().move_as_ok());
      }
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_shard_state_from_db, handle,
                            std::move(P));
  } else {
    CHECK(handle->is_key_block());
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ProofLink>> R) {
      if (R.is_error()) {
        VLOG(full_node, WARNING) << "failed to get key block proof: " << R.move_as_error();
      } else {
        td::actor::send_closure(SelfId, &FullNodeImpl::got_key_block_config,
                                R.ok()->get_key_block_config().move_as_ok());
      }
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_proof_link_from_db, handle,
                            std::move(P));
  }
}

void FullNodeImpl::process_block_finality_broadcast(BlockFinalityBroadcast finality, BroadcastSource source,
                                                    bool send_to_custom) {
  if (send_to_custom) {
    send_block_finality_broadcast_to_custom_overlays(finality);
  }
  td::actor::ask(validator_manager_, &ValidatorManagerInterface::new_block_finality_broadcast, std::move(finality),
                 source)
      .detach();
}

void FullNodeImpl::process_block_candidate_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                     td::uint32 validator_set_hash, td::BufferSlice data,
                                                     BroadcastSource source, bool send_to_custom) {
  if (send_to_custom) {
    send_block_candidate_broadcast_to_custom_overlays(block_id, cc_seqno, validator_set_hash, data);
  }
  td::actor::ask(validator_manager_, &ValidatorManagerInterface::new_block_candidate_broadcast, block_id, cc_seqno,
                 std::move(data), source)
      .detach();
}

void FullNodeImpl::process_shard_block_info_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data,
                                                      bool send_to_custom) {
  if (send_to_custom) {
    send_shard_block_info_to_custom_overlays(block_id, cc_seqno, data);
  }
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_shard_block_description_broadcast,
                          block_id, cc_seqno, std::move(data));
}

void FullNodeImpl::get_out_msg_queue_query_token(td::Promise<std::unique_ptr<ActionToken>> promise) {
  td::actor::send_closure(out_msg_queue_query_token_manager_, &TokenManager::get_token, 1, 0, td::Timestamp::in(10.0),
                          std::move(promise));
}

void FullNodeImpl::set_validator_telemetry_filename(std::string value) {
  validator_telemetry_filename_ = std::move(value);
  update_validator_telemetry_collector();
}

void FullNodeImpl::update_validator_telemetry_collector() {
  if (validator_telemetry_filename_.empty()) {
    validator_telemetry_collector_key_ = PublicKeyHash::zero();
    return;
  }
  if (fast_sync_overlays_.get_masterchain_overlay_for(adnl::AdnlNodeIdShort{validator_telemetry_collector_key_})
          .empty()) {
    auto [actor, adnl_id] = fast_sync_overlays_.choose_overlay(ShardIdFull{masterchainId});
    validator_telemetry_collector_key_ = adnl_id.pubkey_hash();
    if (!actor.empty()) {
      td::actor::send_closure(actor, &FullNodeFastSyncOverlay::collect_validator_telemetry,
                              validator_telemetry_filename_);
    }
  }
}

void FullNodeImpl::set_plumtree_stats_filename(std::string value) {
  plumtree_stats_filename_ = std::move(value);
  plumtree_stats_collector_key_ = PublicKeyHash::zero();
  update_plumtree_stats_collector();
}

void FullNodeImpl::update_plumtree_stats_collector() {
  if (plumtree_stats_filename_.empty()) {
    plumtree_stats_collector_key_ = PublicKeyHash::zero();
    return;
  }
  if (fast_sync_overlays_.get_masterchain_overlay_for(adnl::AdnlNodeIdShort{plumtree_stats_collector_key_}).empty()) {
    auto [actor, adnl_id] = fast_sync_overlays_.choose_overlay(ShardIdFull{masterchainId});
    if (actor.empty()) {
      plumtree_stats_collector_key_ = PublicKeyHash::zero();
      return;
    }
    plumtree_stats_collector_key_ = adnl_id.pubkey_hash();
    td::actor::send_closure(actor, &FullNodeFastSyncOverlay::collect_plumtree_stats, plumtree_stats_filename_);
  }
}

void FullNodeImpl::alarm() {
  alarm_timestamp() = td::Timestamp::never();
  schedule_plumtree_stats_exchange();
  if (sync_completed_at_ && sync_completed_at_.is_in_past()) {
    if (sync_promise_) {
      sync_promise_.set_value(td::Unit());
    }
    sync_completed_at_ = td::Timestamp::never();
  }
  alarm_timestamp().relax(sync_completed_at_);
}

void FullNodeImpl::schedule_plumtree_stats_exchange() {
  bool is_validator = std::any_of(local_keys_.begin(), local_keys_.end(),
                                  [&](const PublicKeyHash &key) { return current_validators_.contains(key); });
  if (!is_validator) {
    // The validator set changes over time; check again later.
    plumtree_stats_exchange_at_ = td::Timestamp::never();
    alarm_timestamp().relax(td::Timestamp::in(60.0));
    return;
  }
  auto now = td::Clocks::system();
  auto epoch_duration = overlay::PlumtreeFecOptions{}.stats_epoch_duration_;
  auto epoch = static_cast<td::int64>(now / epoch_duration);
  double min_delay = epoch_duration * PLUMTREE_STATS_EXCHANGE_FROM;
  double max_delay = epoch_duration * PLUMTREE_STATS_EXCHANGE_TO;
  if (epoch != plumtree_stats_exchange_epoch_) {
    plumtree_stats_exchange_epoch_ = epoch;
    double at = static_cast<double>(epoch) * epoch_duration + min_delay +
                td::Random::fast(0, 1000000) * 1e-6 * (max_delay - min_delay);
    plumtree_stats_exchange_at_ = td::Timestamp::in(std::max(at - now, 0.0));
  }
  if (plumtree_stats_exchange_at_ && plumtree_stats_exchange_at_.is_in_past()) {
    plumtree_stats_exchange_at_ = td::Timestamp::never();
    start_plumtree_stats_exchange();
  }
  if (plumtree_stats_exchange_at_) {
    alarm_timestamp().relax(plumtree_stats_exchange_at_);
  } else {
    // Wake up at the start of the next epoch's exchange window.
    double next = static_cast<double>(epoch + 1) * epoch_duration + min_delay;
    alarm_timestamp().relax(td::Timestamp::in(std::max(next - now, 1.0)));
  }
}

void FullNodeImpl::start_plumtree_stats_exchange() {
  auto masterchain_overlay = fast_sync_overlays_.choose_overlay(ShardIdFull{masterchainId}, true).first;
  if (masterchain_overlay.empty()) {
    return;
  }

  fast_sync_overlays_.send_plumtree_stats(masterchain_overlay, PLUMTREE_STATS_EXCHANGE_OVERLAYS_LIMIT);
  std::size_t public_overlays = 0;
  for (const auto &[shard, info] : shards_) {
    if (info.actor.empty()) {
      continue;
    }
    if (public_overlays >= PLUMTREE_STATS_EXCHANGE_OVERLAYS_LIMIT) {
      break;
    }
    ++public_overlays;
    auto X = create_hash_tl_object<ton_api::tonNode_shardPublicOverlayId>(shard.workchain, shard.shard,
                                                                          zero_state_file_hash_);
    td::BufferSlice b{32};
    b.as_slice().copy_from(as_slice(X));
    auto stats_overlay = overlay::OverlayIdFull{std::move(b)}.compute_short_id();
    td::actor::send_closure(
        overlays_, &overlay::Overlays::get_plumtree_stats_records, adnl_id_, stats_overlay,
        td::PromiseCreator::lambda(
            [masterchain_overlay, stats_overlay,
             shard](td::Result<std::vector<tl_object_ptr<ton_api::overlay_plumtreeStatsRecord>>> R) mutable {
              if (R.is_error()) {
                VLOG(full_node, WARNING) << "Failed to get public Plumtree stats records: " << R.move_as_error();
                return;
              }
              auto records = R.move_as_ok();
              if (records.empty()) {
                return;
              }
              td::actor::send_closure(masterchain_overlay, &FullNodeFastSyncOverlay::send_plumtree_stats, stats_overlay,
                                      std::string{"public"}, shard, std::move(records));
            }));
  }
}

td::actor::Task<td::BufferSlice> FullNodeImpl::handle_query(td::BufferSlice query, adnl::AdnlNodeIdShort src,
                                                            QuerySource source) {
  switch (source) {
    case QuerySource::public_overlay:
      return query_handler_public_.handle_query(std::move(query), src, source);
    case QuerySource::fast_sync_overlay:
      return query_handler_fast_sync_.handle_query(std::move(query), src, source);
    case QuerySource::custom_overlay:
      return query_handler_custom_.handle_query(std::move(query), src, source);
    default:
      UNREACHABLE();  // full-node master queries are processed in FullNodeMaster actor
  }
}

void FullNodeImpl::start_up() {
  if (client_.empty()) {
    update_shard_actor(ShardIdFull{masterchainId}, true);
  }
  if (local_id_.is_zero()) {
    if (adnl_id_.is_zero()) {
      auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
      local_id_ = pk.compute_short_id();

      td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), true, [](td::Result<>) {});
    } else {
      local_id_ = adnl_id_.pubkey_hash();
    }
  }
  class Callback : public ValidatorManagerInterface::Callback {
   public:
    void initial_read_complete(BlockHandle handle) override {
      td::actor::send_closure(id_, &FullNodeImpl::initial_read_complete, handle);
    }
    void on_new_masterchain_block(td::Ref<MasterchainState> state, std::set<ShardIdFull> shards_to_monitor) override {
      td::actor::send_closure(id_, &FullNodeImpl::on_new_masterchain_block, std::move(state),
                              std::move(shards_to_monitor));
    }
    void send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data) override {
      td::actor::ask(id_, &FullNodeImpl::send_ext_message, dst, std::move(data)).detach("send_ext_message");
    }
    void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_shard_block_info, block_id, cc_seqno, std::move(data));
    }
    void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                              td::BufferSlice data, int mode) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_block_candidate, block_id, cc_seqno, validator_set_hash,
                              std::move(data), mode);
    }
    void send_out_msg_queue_proof_broadcast(td::Ref<OutMsgQueueProofBroadcast> broadcast) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_out_msg_queue_proof_broadcast, std::move(broadcast));
    }
    void send_block_finality_broadcast(BlockFinalityBroadcast finality, int mode) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_block_finality_broadcast, std::move(finality), mode);
    }
    void download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                        td::Promise<ReceivedBlock> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_block, id, priority, timeout, std::move(promise));
    }
    void download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                             td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_zero_state, id, priority, timeout, std::move(promise));
    }
    void download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, PersistentStateType type,
                                   td::uint32 priority, td::Timestamp timeout,
                                   td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_persistent_state, id, masterchain_block_id, type, priority,
                              timeout, std::move(promise));
    }
    void download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                              td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_block_proof, block_id, priority, timeout,
                              std::move(promise));
    }
    void download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                   td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_block_proof_link, block_id, priority, timeout,
                              std::move(promise));
    }
    void get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                             td::Promise<std::vector<BlockIdExt>> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::get_next_key_blocks, block_id, timeout, std::move(promise));
    }
    void download_archive(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                          td::Timestamp timeout, td::Promise<std::string> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_archive, masterchain_seqno, shard_prefix, std::move(tmp_dir),
                              timeout, std::move(promise));
    }
    void download_out_msg_queue_proof(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                      block::ImportedMsgQueueLimits limits, td::Timestamp timeout,
                                      td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise) override {
      promise.set_error(td::Status::Error("not implemented"));
    }

    void new_key_block(BlockHandle handle) override {
      td::actor::send_closure(id_, &FullNodeImpl::new_key_block, std::move(handle));
    }

    explicit Callback(td::actor::ActorId<FullNodeImpl> id) : id_(id) {
    }

   private:
    td::actor::ActorId<FullNodeImpl> id_;
  };

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::install_callback,
                          std::make_unique<Callback>(actor_id(this)), std::move(started_promise_));
  alarm_timestamp().relax(td::Timestamp::in(1.0));
}

void FullNodeImpl::update_private_overlays() {
  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }

  update_validator_telemetry_collector();
  update_plumtree_stats_collector();
  if (local_keys_.empty()) {
    return;
  }
}

void FullNodeImpl::update_custom_overlay(CustomOverlayInfo &overlay) {
  auto old_actors = std::move(overlay.actors_);
  overlay.actors_.clear();
  CustomOverlayParams &params = overlay.params_;
  auto try_local_id = [&](const adnl::AdnlNodeIdShort &local_id) {
    if (std::find(params.nodes_.begin(), params.nodes_.end(), local_id) != params.nodes_.end()) {
      auto it = old_actors.find(local_id);
      if (it != old_actors.end()) {
        overlay.actors_[local_id] = std::move(it->second);
        old_actors.erase(it);
      } else {
        auto adnl_sender = (params.use_quic_ ? td::actor::ActorId<adnl::AdnlSenderEx>{quic_} : rldp2_);
        overlay.actors_[local_id] = td::actor::create_actor<FullNodeCustomOverlay>(
            "CustomOverlay", local_id, params, zero_state_file_hash_, opts_, keyring_, adnl_, adnl_sender, overlays_,
            validator_manager_, actor_id(this));
      }
    }
  };
  try_local_id(adnl_id_);
  for (const PublicKeyHash &local_key : local_keys_) {
    auto it = current_validators_.find(local_key);
    if (it != current_validators_.end()) {
      try_local_id(it->second);
    }
  }
}

void FullNodeImpl::send_block_finality_broadcast_to_custom_overlays(const BlockFinalityBroadcast &finality) {
  if (custom_overlays_sent_finality_.contains(finality.block_id)) {
    return;
  }
  custom_overlays_sent_finality_.put(finality.block_id, {});
  for (auto &[_, private_overlay] : custom_overlays_) {
    if (private_overlay.params_.send_shard(finality.block_id.shard_full())) {
      for (auto &[local_id, actor] : private_overlay.actors_) {
        if (private_overlay.params_.block_senders_.contains(local_id)) {
          td::actor::send_closure(actor, &FullNodeCustomOverlay::send_block_finality_broadcast, finality.clone());
        }
      }
    }
  }
}

void FullNodeImpl::send_block_candidate_broadcast_to_custom_overlays(const BlockIdExt &block_id, CatchainSeqno cc_seqno,
                                                                     td::uint32 validator_set_hash,
                                                                     const td::BufferSlice &data) {
  // Same cache of sent broadcasts as in send_block_broadcast_to_custom_overlays
  if (custom_overlays_sent_broadcasts_.contains(block_id)) {
    return;
  }
  custom_overlays_sent_broadcasts_.put(block_id, {});
  for (auto &[_, private_overlay] : custom_overlays_) {
    if (private_overlay.params_.send_shard(block_id.shard_full())) {
      for (auto &[local_id, actor] : private_overlay.actors_) {
        if (private_overlay.params_.block_senders_.contains(local_id)) {
          td::actor::send_closure(actor, &FullNodeCustomOverlay::send_block_candidate, block_id, cc_seqno,
                                  validator_set_hash, data.clone());
        }
      }
    }
  }
}

void FullNodeImpl::send_shard_block_info_to_custom_overlays(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                            const td::BufferSlice &data) {
  if (custom_overlays_sent_shard_block_desc_.contains(block_id)) {
    return;
  }
  custom_overlays_sent_shard_block_desc_.put(block_id, {});
  for (auto &[_, private_overlay] : custom_overlays_) {
    if (private_overlay.params_.send_shard(block_id.shard_full())) {
      for (auto &[local_id, actor] : private_overlay.actors_) {
        if (private_overlay.params_.block_senders_.contains(local_id)) {
          td::actor::send_closure(actor, &FullNodeCustomOverlay::send_shard_block_info, block_id, cc_seqno,
                                  data.clone());
        }
      }
    }
  }
}

FullNodeImpl::FullNodeImpl(PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
                           FullNodeOptions opts, td::actor::ActorId<keyring::Keyring> keyring,
                           td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp2::Rldp> rldp2,
                           td::actor::ActorId<quic::QuicSender> quic, td::actor::ActorId<dht::Dht> dht,
                           td::actor::ActorId<overlay::Overlays> overlays,
                           td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                           td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root,
                           td::Promise<td::Unit> started_promise)
    : local_id_(local_id)
    , adnl_id_(adnl_id)
    , zero_state_file_hash_(zero_state_file_hash)
    , keyring_(keyring)
    , adnl_(adnl)
    , rldp2_(rldp2)
    , quic_(quic)
    , dht_(dht)
    , overlays_(overlays)
    , validator_manager_(validator_manager)
    , client_(client)
    , db_root_(db_root)
    , started_promise_(std::move(started_promise))
    , opts_(opts)
    , query_handler_public_(validator_manager, make_rate_limiter(opts.rate_limit_public_))
    , query_handler_fast_sync_(validator_manager, make_rate_limiter(opts.rate_limit_fast_sync_))
    , query_handler_custom_(validator_manager, make_rate_limiter(opts.rate_limit_custom_)) {
}

td::actor::ActorOwn<FullNode> FullNode::create(
    ton::PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash, FullNodeOptions opts,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp2::Rldp> rldp2, td::actor::ActorId<quic::QuicSender> quic, td::actor::ActorId<dht::Dht> dht,
    td::actor::ActorId<overlay::Overlays> overlays, td::actor::ActorId<ValidatorManagerInterface> validator_manager,
    td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root, td::Promise<td::Unit> started_promise) {
  return td::actor::create_actor<FullNodeImpl>("fullnode", local_id, adnl_id, zero_state_file_hash, opts, keyring, adnl,
                                               rldp2, quic, dht, overlays, validator_manager, client, db_root,
                                               std::move(started_promise));
}

FullNodeConfig::FullNodeConfig(const tl_object_ptr<ton_api::engine_validator_fullNodeConfig> &obj)
    : ext_messages_broadcast_disabled_(obj->ext_messages_broadcast_disabled_) {
}

tl_object_ptr<ton_api::engine_validator_fullNodeConfig> FullNodeConfig::tl() const {
  return create_tl_object<ton_api::engine_validator_fullNodeConfig>(ext_messages_broadcast_disabled_);
}

bool CustomOverlayParams::send_shard(const ShardIdFull &shard) const {
  return sender_shards_.empty() ||
         std::any_of(sender_shards_.begin(), sender_shards_.end(),
                     [&](const ShardIdFull &our_shard) { return shard_intersects(shard, our_shard); });
}

CustomOverlayParams CustomOverlayParams::fetch(const ton_api::engine_validator_customOverlay &f) {
  CustomOverlayParams c;
  c.name_ = f.name_;
  for (const auto &node : f.nodes_) {
    c.nodes_.emplace_back(node->adnl_id_);
    if (node->msg_sender_) {
      c.msg_senders_[adnl::AdnlNodeIdShort{node->adnl_id_}] = node->msg_sender_priority_;
    }
    if (node->block_sender_) {
      c.block_senders_.emplace(node->adnl_id_);
    }
    if (node->accept_queries_) {
      c.accept_queries_.emplace(node->adnl_id_);
    }
  }
  for (const auto &shard : f.sender_shards_) {
    c.sender_shards_.push_back(create_shard_id(shard));
  }
  c.skip_public_msg_send_ = f.skip_public_msg_send_;
  c.use_quic_ = f.use_quic_;
  c.send_queries_ = f.send_queries_;
  return c;
}

std::shared_ptr<RateLimiter<>> FullNodeImpl::make_rate_limiter(const FullNodeOptions::RateLimiterParams &params) {
  double w_size = params.window_size_;
  return std::make_shared<RateLimiter<>>(
      RateLimit{w_size, params.limit_global_}, RateLimit{w_size, params.limit_heavy_},
      std::set{ton_api::tonNode_getArchiveSlice::ID, ton_api::tonNode_downloadPersistentStateSliceV2::ID,
               ton_api::tonNode_downloadZeroState::ID},
      RateLimit{w_size, params.limit_medium_},
      std::set{ton_api::tonNode_downloadBlock::ID, ton_api::tonNode_downloadBlockFull::ID,
               ton_api::tonNode_downloadNextBlockFull::ID, ton_api::tonNode_downloadNextBlocksFull::ID,
               ton_api::tonNode_downloadBlockProof::ID, ton_api::tonNode_downloadBlockProofLink::ID,
               ton_api::tonNode_downloadKeyBlockProof::ID, ton_api::tonNode_downloadKeyBlockProofLink::ID,
               ton_api::tonNode_getOutMsgQueueProof::ID, ton_api::tonNode_prepareKeyBlockProof::ID},
      RateLimit{w_size, params.limit_small_},
      std::set{ton_api::tonNode_getNextBlockDescription::ID, ton_api::tonNode_getNextBlocksDescription::ID,
               ton_api::tonNode_prepareBlockProof::ID, ton_api::tonNode_prepareBlock::ID,
               ton_api::tonNode_prepareZeroState::ID, ton_api::tonNode_getNextKeyBlockIds::ID,
               ton_api::tonNode_getArchiveInfo::ID, ton_api::tonNode_getShardArchiveInfo::ID,
               ton_api::tonNode_preparePersistentState::ID, ton_api::tonNode_getPersistentStateSizeV2::ID});
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
