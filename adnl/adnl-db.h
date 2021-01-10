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
#pragma once

#include "td/actor/actor.h"

#include "adnl.h"

namespace ton {

namespace adnl {

struct AdnlDbItem {
  AdnlNodeIdFull id;
  AdnlAddressList addr_list;
  AdnlAddressList priority_addr_list;
};

class AdnlDb : public td::actor::Actor {
 public:
  virtual ~AdnlDb() = default;
  virtual void update(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, AdnlDbItem item,
                      td::Promise<td::Unit> promise) = 0;
  virtual void get(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, td::Promise<AdnlDbItem> promise) = 0;

  static td::actor::ActorOwn<AdnlDb> create(std::string path);
};

}  // namespace adnl

}  // namespace ton
