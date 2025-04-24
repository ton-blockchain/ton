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
#include "ext-client.h"
#include "td/utils/Random.h"
#include "ton/ton-shard.h"

namespace liteclient {

class ExtClientImpl : public ExtClient {
 public:
  ExtClientImpl(std::vector<LiteServerConfig> liteservers, td::unique_ptr<Callback> callback, bool connect_to_all)
      : callback_(std::move(callback)), connect_to_all_(connect_to_all) {
    CHECK(!liteservers.empty());
    servers_.resize(liteservers.size());
    for (size_t i = 0; i < servers_.size(); ++i) {
      servers_[i].config = std::move(liteservers[i]);
      servers_[i].idx = i;
    }
  }

  void start_up() override {
    LOG(INFO) << "Started ext client, " << servers_.size() << " liteservers";
    td::Random::Fast rnd;
    td::random_shuffle(td::as_mutable_span(servers_), rnd);
    server_indices_.resize(servers_.size());
    for (size_t i = 0; i < servers_.size(); ++i) {
      server_indices_[servers_[i].idx] = i;
    }

    if (connect_to_all_) {
      for (size_t i = 0; i < servers_.size(); ++i) {
        prepare_server(i, nullptr);
      }
    }
  }

  void send_query(std::string name, td::BufferSlice data, td::Timestamp timeout,
                  td::Promise<td::BufferSlice> promise) override {
    QueryInfo query_info = get_query_info(data);
    TRY_RESULT_PROMISE(promise, server_idx, select_server(query_info));
    send_query_internal(std::move(name), std::move(data), std::move(query_info), server_idx, timeout,
                        std::move(promise));
  }

  void send_query_to_server(std::string name, td::BufferSlice data, size_t server_idx, td::Timestamp timeout,
                            td::Promise<td::BufferSlice> promise) override {
    if (server_idx >= servers_.size()) {
      promise.set_error(td::Status::Error(PSTRING() << "server idx " << server_idx << " is too big"));
      return;
    }
    server_idx = server_indices_[server_idx];
    QueryInfo query_info = get_query_info(data);
    prepare_server(server_idx, &query_info);
    send_query_internal(std::move(name), std::move(data), std::move(query_info), server_idx, timeout,
                        std::move(promise));
  }

  void get_servers_status(td::Promise<std::vector<bool>> promise) override {
    std::vector<bool> status(servers_.size());
    for (const Server& s : servers_) {
      status[s.idx] = s.alive;
    }
    promise.set_result(std::move(status));
  }

  void reset_servers() override {
    LOG(INFO) << "Force resetting all liteservers";
    for (Server& server : servers_) {
      server.alive = false;
      server.timeout = {};
      server.ignore_until = {};
      server.client.reset();
    }
  }

 private:
  void send_query_internal(std::string name, td::BufferSlice data, QueryInfo query_info, size_t server_idx,
                           td::Timestamp timeout, td::Promise<td::BufferSlice> promise) {
    auto& server = servers_[server_idx];
    CHECK(!server.client.empty());
    if (!connect_to_all_) {
      alarm_timestamp().relax(server.timeout = td::Timestamp::in(MAX_NO_QUERIES_TIMEOUT));
    }
    td::Promise<td::BufferSlice> P = [SelfId = actor_id(this), server_idx,
                                      promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
      if (R.is_error() &&
          (R.error().code() == ton::ErrorCode::timeout || R.error().code() == ton::ErrorCode::cancelled)) {
        td::actor::send_closure(SelfId, &ExtClientImpl::on_server_error, server_idx);
      }
      promise.set_result(std::move(R));
    };
    LOG(DEBUG) << "Sending query " << query_info.to_str() << " to server #" << server.idx << " ("
               << server.config.addr.get_ip_str() << ":" << server.config.addr.get_port() << ")";
    send_closure(server.client, &ton::adnl::AdnlExtClient::send_query, std::move(name), std::move(data), timeout,
                 std::move(P));
  }

  td::Result<size_t> select_server(const QueryInfo& query_info) {
    for (size_t i = 0; i < servers_.size(); ++i) {
      if (servers_[i].alive && servers_[i].config.accepts_query(query_info)) {
        return i;
      }
    }
    size_t server_idx = servers_.size();
    int cnt = 0;
    int best_priority = -1;
    for (size_t i = 0; i < servers_.size(); ++i) {
      Server& server = servers_[i];
      if (!server.config.accepts_query(query_info)) {
        continue;
      }
      int priority = 0;
      priority += (server.ignore_until && !server.ignore_until.is_in_past() ? 0 : 10);
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
      return td::Status::Error(PSTRING() << "no liteserver for query " << query_info.to_str());
    }
    prepare_server(server_idx, &query_info);
    return server_idx;
  }

  void prepare_server(size_t server_idx, const QueryInfo* query_info) {
    Server& server = servers_[server_idx];
    if (server.alive) {
      return;
    }
    server.alive = true;
    server.ignore_until = {};
    if (!connect_to_all_) {
      alarm_timestamp().relax(server.timeout = td::Timestamp::in(MAX_NO_QUERIES_TIMEOUT));
    }
    if (!server.client.empty()) {
      return;
    }

    class Callback : public ton::adnl::AdnlExtClient::Callback {
     public:
      explicit Callback(td::actor::ActorId<ExtClientImpl> parent, size_t idx) : parent_(std::move(parent)), idx_(idx) {
      }
      void on_ready() override {
      }
      void on_stop_ready() override {
        td::actor::send_closure(parent_, &ExtClientImpl::on_server_error, idx_);
      }

     private:
      td::actor::ActorId<ExtClientImpl> parent_;
      size_t idx_;
    };
    LOG(INFO) << "Connecting to liteserver #" << server.idx << " (" << server.config.addr.get_ip_str() << ":"
              << server.config.addr.get_port() << ") for query " << (query_info ? query_info->to_str() : "[none]");
    server.client = ton::adnl::AdnlExtClient::create(server.config.adnl_id, server.config.addr,
                                                     std::make_unique<Callback>(actor_id(this), server_idx));
  }

  struct Server {
    LiteServerConfig config;
    size_t idx = 0;
    td::actor::ActorOwn<ton::adnl::AdnlExtClient> client;
    bool alive = false;
    td::Timestamp timeout = td::Timestamp::never();
    td::Timestamp ignore_until = td::Timestamp::never();
  };
  std::vector<Server> servers_;
  std::vector<size_t> server_indices_;

  td::unique_ptr<Callback> callback_;
  bool connect_to_all_ = false;
  static constexpr double MAX_NO_QUERIES_TIMEOUT = 100.0;
  static constexpr double BAD_SERVER_TIMEOUT = 30.0;

  void alarm() override {
    if (connect_to_all_) {
      return;
    }
    for (Server& server : servers_) {
      if (server.timeout && server.timeout.is_in_past()) {
        LOG(INFO) << "Closing connection to liteserver #" << server.idx << " (" << server.config.addr.get_ip_str()
                  << ":" << server.config.addr.get_port() << ")";
        server.client.reset();
        server.alive = false;
        server.ignore_until = {};
      }
    }
  }

  void on_server_error(size_t idx) {
    servers_[idx].alive = false;
    servers_[idx].ignore_until = td::Timestamp::in(BAD_SERVER_TIMEOUT);
  }
};

td::actor::ActorOwn<ExtClient> ExtClient::create(ton::adnl::AdnlNodeIdFull dst, td::IPAddress dst_addr,
                                                 td::unique_ptr<Callback> callback) {
  return create({LiteServerConfig{dst, dst_addr}}, std::move(callback));
}

td::actor::ActorOwn<ExtClient> ExtClient::create(std::vector<LiteServerConfig> liteservers,
                                                 td::unique_ptr<Callback> callback, bool connect_to_all) {
  return td::actor::create_actor<ExtClientImpl>("ExtClient", std::move(liteservers), std::move(callback),
                                                connect_to_all);
}
}  // namespace liteclient
