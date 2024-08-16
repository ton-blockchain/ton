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
#include "td/actor/actor.h"
#include "ton/ton-types.h"
#include "adnl/adnl-ext-client.h"
#include "query-utils.hpp"

namespace liteclient {
class ExtClient : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual ~Callback() = default;
  };

  virtual void send_query(std::string name, td::BufferSlice data, td::Timestamp timeout,
                          td::Promise<td::BufferSlice> promise) = 0;
  virtual void send_query_to_server(std::string name, td::BufferSlice data, size_t server_idx, td::Timestamp timeout,
                                    td::Promise<td::BufferSlice> promise) {
    promise.set_error(td::Status::Error("not supported"));
  }
  virtual void get_servers_status(td::Promise<std::vector<bool>> promise) {
    promise.set_error(td::Status::Error("not supported"));
  }
  virtual void reset_servers() {
  }

  static td::actor::ActorOwn<ExtClient> create(ton::adnl::AdnlNodeIdFull dst, td::IPAddress dst_addr,
                                               td::unique_ptr<Callback> callback);
  static td::actor::ActorOwn<ExtClient> create(std::vector<LiteServerConfig> liteservers,
                                               td::unique_ptr<Callback> callback, bool connect_to_all = false);
};
}  // namespace liteclient
