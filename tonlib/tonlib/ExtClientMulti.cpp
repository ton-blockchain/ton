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
#include "ExtClientMulti.h"
#include "ton/ton-shard.h"
#include "td/utils/Random.h"

namespace tonlib {

static const double MAX_NO_QUERIES_TIMEOUT = 120;

ExtClientMulti::ExtClientMulti(std::vector<Config::LiteClient> clients, td::unique_ptr<Callback> callback)
    : callback_(std::move(callback)) {
  for (auto &desc : clients) {
    servers_.emplace_back();
    servers_.back().desc = std::move(desc);
  }
}

void ExtClientMulti::start_up() {
  alarm_timestamp() = td::Timestamp::in(60.0);
}

void ExtClientMulti::send_query(std::string name, td::BufferSlice data, ton::ShardIdFull shard, td::Timestamp timeout,
                                td::Promise<td::BufferSlice> promise) {
  if (shard.is_masterchain() && mc_server_idx_ != -1 && !servers_[mc_server_idx_].is_bad()) {
    send_query_to_server(std::move(name), std::move(data), mc_server_idx_, timeout, std::move(promise));
    return;
  }
  auto it = shard_server_idx_cached_.find(shard);
  if (it != shard_server_idx_cached_.end() && !servers_[it->second].is_bad()) {
    send_query_to_server(std::move(name), std::move(data), it->second, timeout, std::move(promise));
    return;
  }
  int server_idx = -1;
  int random_idx = -1;
  int cnt = 0;
  int best_prefix = -1;
  for (int i = 0; i < (int)servers_.size(); ++i) {
    const Server &server = servers_[i];
    if (server.is_bad()) {
      continue;
    }
    int len = server.desc.is_full ? 65 : server.max_supported_prefix(shard);
    if (len > best_prefix) {
      best_prefix = len;
      server_idx = -1;
      random_idx = -1;
      cnt = 0;
    } else if (len < best_prefix) {
      continue;
    }
    if (!server.client.empty()) {
      server_idx = i;
    }
    if (td::Random::fast(0, cnt) == 0) {
      random_idx = i;
    }
    ++cnt;
  }
  if (server_idx == -1) {
    server_idx = random_idx;
  }
  if (server_idx == -1) {
    promise.set_error(td::Status::Error("failed to select a suitable server"));
    return;
  }
  if (shard.pfx_len() <= ton::max_shard_pfx_len) {
    shard_server_idx_cached_[shard] = server_idx;
  }
  if (shard.is_masterchain() || servers_[server_idx].desc.is_full) {
    mc_server_idx_ = server_idx;
  }
  send_query_to_server(std::move(name), std::move(data), server_idx, timeout, std::move(promise));
}

void ExtClientMulti::send_query_to_server(std::string name, td::BufferSlice data, int server_idx, td::Timestamp timeout,
                                          td::Promise<td::BufferSlice> promise) {
  Server &server = servers_.at(server_idx);
  if (server.client.empty()) {
    start_client(server_idx);
  }
  server.ttl = td::Timestamp::in(MAX_NO_QUERIES_TIMEOUT);
  td::Promise<td::BufferSlice> P = [SelfId = actor_id(this), idx = server_idx,
      promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error() &&
        (R.error().code() == ton::ErrorCode::timeout || R.error().code() == ton::ErrorCode::cancelled)) {
      td::actor::send_closure(SelfId, &ExtClientMulti::set_server_bad, idx);
    }
    promise.set_result(std::move(R));
  };
  send_closure(server.client, &ton::adnl::AdnlExtClient::send_query, std::move(name), std::move(data), timeout,
               std::move(P));
}

void ExtClientMulti::force_change_liteserver() {
  if (mc_server_idx_ != -1) {
    set_server_bad(mc_server_idx_);
    mc_server_idx_ = -1;
  }
}

void ExtClientMulti::start_client(int server_idx) {
  class Callback : public ton::adnl::AdnlExtClient::Callback {
   public:
    Callback(td::actor::ActorId<ExtClientMulti> parent, int idx) : parent_(std::move(parent)), idx_(idx) {
    }
    void on_ready() override {
    }
    void on_stop_ready() override {
      td::actor::send_closure(parent_, &ExtClientMulti::set_server_bad, idx_);
    }

   private:
    td::actor::ActorId<ExtClientMulti> parent_;
    int idx_;
  };
  Server &server = servers_.at(server_idx);
  server.client = ton::adnl::AdnlExtClient::create(server.desc.adnl_id, server.desc.address,
                                                   std::make_unique<Callback>(actor_id(this), server_idx));
}

void ExtClientMulti::alarm() {
  for (Server& server : servers_) {
    if (server.ttl && server.ttl.is_in_past()) {
      server.client.reset();
    }
  }
  alarm_timestamp() = td::Timestamp::in(60.0);
}

void ExtClientMulti::set_server_bad(int idx) {
  Server& server = servers_.at(idx);
  server.client.reset();
  server.ttl = td::Timestamp::never();
  server.ignore_until = td::Timestamp::in(10.0);
}

int ExtClientMulti::Server::max_supported_prefix(ton::ShardIdFull shard) const {
  if (desc.is_full || shard.is_masterchain()) {
    return shard.pfx_len();
  }
  int res = -1;
  for (const ton::ShardIdFull &our_shard : desc.shards) {
    if (ton::shard_is_ancestor(our_shard, shard)) {
      return shard.pfx_len();
    }
    if (shard.workchain == our_shard.workchain) {
      int x = std::min({shard.pfx_len(), our_shard.pfx_len(), ton::count_matching_bits(shard.shard, our_shard.shard)});
      res = std::max(res, x);
    }
  }
  return res;
}

}  // namespace tonlib
