/* 
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission 
    to link the code of portions of this program with the OpenSSL library. 
    You must obey the GNU General Public License in all respects for all 
    of the code used other than OpenSSL. If you modify file(s) with this 
    exception, you may extend this exception to your version of the file(s), 
    but you are not obligated to do so. If you do not wish to do so, delete this 
    exception statement from your version. If you delete this exception statement 
    from all source files in the program, then also delete it here.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include "td/actor/actor.h"
#include "td/utils/buffer.h"
#include "ton/ton-types.h"
#include "td/utils/port/IPAddress.h"

#define MAX_POST_SIZE (64 << 10)

extern bool local_scripts_;

class CoreActorInterface : public td::actor::Actor {
 public:
  struct RemoteNodeStatus {
    std::vector<ton::BlockIdExt> values_;
    td::Timestamp ts_;
    RemoteNodeStatus(size_t size, td::Timestamp ts) : ts_(ts) {
      values_.resize(size);
    }
  };

  struct RemoteNodeStatusList {
    std::vector<td::IPAddress> ips;
    std::vector<std::shared_ptr<RemoteNodeStatus>> results;
  };
  virtual ~CoreActorInterface() = default;

  virtual void send_lite_query(td::BufferSlice data, td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_last_result(td::Promise<std::shared_ptr<RemoteNodeStatus>> promise) = 0;
  virtual void get_results(td::uint32 max, td::Promise<RemoteNodeStatusList> promise) = 0;

  static td::actor::ActorId<CoreActorInterface> instance_actor_id();
};
