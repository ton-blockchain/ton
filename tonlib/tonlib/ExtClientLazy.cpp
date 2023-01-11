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
#include "ExtClientLazy.h"
#include "TonlibError.h"
#include "td/utils/Random.h"
#include "ton/ton-shard.h"
#include <map>

namespace tonlib {

class ExtClientLazyImpl : public ExtClientLazy {
 public:
  ExtClientLazyImpl(std::vector<Config::LiteServer> servers, td::unique_ptr<ExtClientLazy::Callback> callback)
      : callback_(std::move(callback)) {
    CHECK(!servers.empty());
    servers_.resize(servers.size());
    for (size_t i = 0; i < servers_.size(); ++i) {
      servers_[i].s = std::move(servers[i]);
      if (!servers_[i].s.is_full) {
        for (auto shard : servers_[i].s.shards) {
          CHECK(shard.is_valid_ext());
          max_server_shard_depth_ = std::max(max_server_shard_depth_, shard.pfx_len());
        }
      }
    }
  }

  void start_up() override {
    td::Random::Fast rnd;
    td::random_shuffle(td::as_mutable_span(servers_), rnd);
  }

  void send_query(std::string name, td::BufferSlice data, ton::ShardIdFull shard, td::Timestamp timeout,
                  td::Promise<td::BufferSlice> promise) override {
    TRY_RESULT_PROMISE(promise, server_idx, before_query(shard));
    auto& server = servers_[server_idx];
    CHECK(!server.client.empty());
    alarm_timestamp().relax(server.timeout = td::Timestamp::in(MAX_NO_QUERIES_TIMEOUT));
    td::Promise<td::BufferSlice> P = [SelfId = actor_id(this), server_idx,
                                      promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
      if (R.is_error() &&
          (R.error().code() == ton::ErrorCode::timeout || R.error().code() == ton::ErrorCode::cancelled)) {
        td::actor::send_closure(SelfId, &ExtClientLazyImpl::set_server_bad, server_idx);
      }
      promise.set_result(std::move(R));
    };
    send_closure(server.client, &ton::adnl::AdnlExtClient::send_query, std::move(name), std::move(data), timeout,
                 std::move(P));
  }

  void force_change_liteserver() override {
    if (servers_.size() == 1) {
      return;
    }
    auto it = shard_to_server_.find(ton::ShardIdFull(ton::masterchainId));
    if (it != shard_to_server_.end()) {
      set_server_bad(it->second);
    }
  }

 private:
  td::Result<size_t> before_query(ton::ShardIdFull shard) {
    if (!shard.is_valid_ext()) {
      return td::Status::Error("Invalid shard");
    }
    if (is_closing_) {
      return td::Status::Error("Client is closing");
    }
    if (shard.pfx_len() > max_server_shard_depth_) {
      shard = shard_prefix(shard, max_server_shard_depth_);
    }
    auto it = shard_to_server_.find(shard);
    if (it != shard_to_server_.end()) {
      size_t server_idx = it->second;
      if (!servers_[server_idx].client.empty()) {
        return server_idx;
      }
      shard_to_server_.erase(it);
    }

    size_t server_idx = servers_.size();
    int cnt = 0;
    int best_priority = -1;
    for (size_t i = 0; i < servers_.size(); ++i) {
      Server& server = servers_[i];
      if (!server.supports(shard)) {
        continue;
      }
      int priority = 0;
      priority += (server.client.empty() ? 0 : 100);
      priority += (server.ignore_until && !server.ignore_until.is_in_past() ? 0 : 10);
      priority += (server.s.is_full ? 1 : 0);
      if (priority < best_priority) {
        continue;
      }
      if (priority > best_priority) {
        best_priority = priority;
        cnt = 0;
      }
      if (td::Random::fast(0, cnt) == 0) {
        server_idx = i;
      }
      ++cnt;
    }
    if (server_idx == servers_.size()) {
      return td::Status::Error(PSTRING() << "No liteserver for shard " << shard.to_str());
    }
    Server& server = servers_[server_idx];
    if (!server.client.empty()) {
      return server_idx;
    }

    class Callback : public ton::adnl::AdnlExtClient::Callback {
     public:
      explicit Callback(td::actor::ActorShared<ExtClientLazyImpl> parent, size_t idx)
          : parent_(std::move(parent)), idx_(idx) {
      }
      void on_ready() override {
      }
      void on_stop_ready() override {
        td::actor::send_closure(parent_, &ExtClientLazyImpl::set_server_bad, idx_);
      }

     private:
      td::actor::ActorShared<ExtClientLazyImpl> parent_;
      size_t idx_;
    };
    ref_cnt_++;
    if (shard.is_masterchain()) {
      LOG(INFO) << "Connecting to liteserver " << server.s.address << " for masterchain";
    } else {
      LOG(INFO) << "Connecting to liteserver " << server.s.address << " for shard " << shard.to_str();
    }
    server.client = ton::adnl::AdnlExtClient::create(
        server.s.adnl_id, server.s.address, std::make_unique<Callback>(td::actor::actor_shared(this), server_idx));
    alarm_timestamp().relax(server.timeout = td::Timestamp::in(MAX_NO_QUERIES_TIMEOUT));
    return server_idx;
  }

  struct Server {
    Config::LiteServer s;
    td::actor::ActorOwn<ton::adnl::AdnlExtClient> client;
    td::Timestamp timeout = td::Timestamp::never();
    td::Timestamp ignore_until = td::Timestamp::never();

    bool supports(const ton::ShardIdFull& shard) const {
      return s.is_full || shard.is_masterchain() ||
             std::any_of(s.shards.begin(), s.shards.end(),
                         [&](const ton::ShardIdFull s_shard) { return ton::shard_intersects(shard, s_shard); });
    }
  };
  std::vector<Server> servers_;
  std::map<ton::ShardIdFull, size_t> shard_to_server_;
  int max_server_shard_depth_ = 0;

  td::unique_ptr<ExtClientLazy::Callback> callback_;
  static constexpr double MAX_NO_QUERIES_TIMEOUT = 100;

  bool is_closing_{false};
  td::uint32 ref_cnt_{1};

  void alarm() override {
    for (Server& server : servers_) {
      if (server.timeout && server.timeout.is_in_past()) {
        server.client.reset();
      }
    }
  }
  void set_server_bad(size_t idx) {
    servers_[idx].client.reset();
    servers_[idx].timeout = td::Timestamp::never();
    servers_[idx].ignore_until = td::Timestamp::in(60.0);
  }
  void hangup_shared() override {
    ref_cnt_--;
    try_stop();
  }
  void hangup() override {
    is_closing_ = true;
    ref_cnt_--;
    for (Server& server : servers_) {
      server.client.reset();
    }
    try_stop();
  }
  void try_stop() {
    if (is_closing_ && ref_cnt_ == 0) {
      stop();
    }
  }
};

td::actor::ActorOwn<ExtClientLazy> ExtClientLazy::create(ton::adnl::AdnlNodeIdFull dst, td::IPAddress dst_addr,
                                                         td::unique_ptr<Callback> callback) {
  return create({Config::LiteServer{dst, dst_addr, true, {}}}, std::move(callback));
}

td::actor::ActorOwn<ExtClientLazy> ExtClientLazy::create(std::vector<Config::LiteServer> servers,
                                                         td::unique_ptr<Callback> callback) {
  return td::actor::create_actor<ExtClientLazyImpl>("ExtClientLazy", std::move(servers), std::move(callback));
}
}  // namespace tonlib
