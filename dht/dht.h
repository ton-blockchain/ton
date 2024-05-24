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

#include "td/utils/int_types.h"

#include "td/actor/PromiseFuture.h"

#include "td/actor/actor.h"

#include "adnl/adnl.h"

#include "auto/tl/ton_api.hpp"

#include "dht-types.h"

namespace ton {

namespace dht {

class DhtGlobalConfig;

class Dht : public td::actor::Actor {
 public:
  static td::Result<td::actor::ActorOwn<Dht>> create(adnl::AdnlNodeIdShort id, std::string db_root,
                                                     std::shared_ptr<DhtGlobalConfig> conf,
                                                     td::actor::ActorId<keyring::Keyring> keyring,
                                                     td::actor::ActorId<adnl::Adnl> adnl);
  static td::Result<td::actor::ActorOwn<Dht>> create_client(adnl::AdnlNodeIdShort id, std::string db_root,
                                                            std::shared_ptr<DhtGlobalConfig> conf,
                                                            td::actor::ActorId<keyring::Keyring> keyring,
                                                            td::actor::ActorId<adnl::Adnl> adnl);
  static td::Result<std::shared_ptr<DhtGlobalConfig>> create_global_config(
      tl_object_ptr<ton_api::dht_config_Global> conf);

  virtual adnl::AdnlNodeIdShort get_id() const = 0;

  virtual void set_value(DhtValue key_value, td::Promise<td::Unit> result) = 0;
  virtual void get_value(DhtKey key, td::Promise<DhtValue> result) = 0;

  virtual void register_reverse_connection(adnl::AdnlNodeIdFull client, td::Promise<td::Unit> promise) = 0;
  virtual void request_reverse_ping(adnl::AdnlNode target, adnl::AdnlNodeIdShort client,
                                    td::Promise<td::Unit> promise) = 0;

  virtual void dump(td::StringBuilder &sb) const = 0;

  virtual ~Dht() = default;
};

}  // namespace dht

}  // namespace ton
