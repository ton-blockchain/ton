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

namespace liteclient {
class ExtClient : public td::actor::Actor {
 public:
  struct LiteServer {
    ton::adnl::AdnlNodeIdFull adnl_id;
    td::IPAddress address;
    bool is_full = true;
    std::vector<ton::ShardIdFull> shards;
  };

  class Callback {
   public:
    virtual ~Callback() {
    }
  };

  virtual void send_query(std::string name, td::BufferSlice data, ton::ShardIdFull shard, td::Timestamp timeout,
                          td::Promise<td::BufferSlice> promise) = 0;
  virtual void force_change_liteserver() = 0;

  static td::actor::ActorOwn<ExtClient> create(ton::adnl::AdnlNodeIdFull dst, td::IPAddress dst_addr,
                                               td::unique_ptr<Callback> callback);
  static td::actor::ActorOwn<ExtClient> create(std::vector<LiteServer> servers, td::unique_ptr<Callback> callback);
};
}  // namespace liteclient
