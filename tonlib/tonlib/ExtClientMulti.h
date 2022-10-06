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
#include "adnl/adnl-ext-client.h"
#include "Config.h"
#include "ExtClientRaw.h"
#include <map>

namespace tonlib {

class ExtClientMulti : public ExtClientRaw {
 public:
  ExtClientMulti(std::vector<Config::LiteClient> clients, td::unique_ptr<Callback> callback);

  void start_up() override;
  void send_query(std::string name, td::BufferSlice data, ton::ShardIdFull shard, td::Timestamp timeout,
                  td::Promise<td::BufferSlice> promise) override;
  void alarm() override;
  void force_change_liteserver() override;

 private:
  void send_query_to_server(std::string name, td::BufferSlice data, int server_idx, td::Timestamp timeout,
                            td::Promise<td::BufferSlice> promise);
  void start_client(int server_idx);

  struct Server {
    Config::LiteClient desc;
    td::actor::ActorOwn<ton::adnl::AdnlExtClient> client;
    td::Timestamp ttl;
    td::Timestamp ignore_until = td::Timestamp::never();

    int max_supported_prefix(ton::ShardIdFull shard) const;
    bool is_bad() const {
      return ignore_until && !ignore_until.is_in_past();
    }
  };

  void set_server_bad(int idx);

  td::unique_ptr<Callback> callback_;
  std::vector<Server> servers_;
  int mc_server_idx_ = -1;
  std::map<ton::ShardIdFull, int> shard_server_idx_cached_;
};

}  // namespace tonlib
