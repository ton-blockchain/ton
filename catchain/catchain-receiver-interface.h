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
#include "adnl/adnl.h"
#include "overlay/overlays.h"
#include "catchain-types.h"

namespace ton {

namespace catchain {

class CatChainReceiverInterface : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual void new_block(td::uint32 src_id, td::uint32 fork_id, CatChainBlockHash hash, CatChainBlockHeight height,
                           CatChainBlockHash prev, std::vector<CatChainBlockHash> deps,
                           std::vector<CatChainBlockHeight> vt, td::SharedSlice data) = 0;
    virtual void blame(td::uint32 src_id) = 0;
    virtual void on_custom_message(PublicKeyHash src, td::BufferSlice data) = 0;
    virtual void on_custom_query(PublicKeyHash src, td::BufferSlice data, td::Promise<td::BufferSlice> promise) = 0;
    virtual void on_broadcast(PublicKeyHash src, td::BufferSlice data) = 0;
    virtual void start() = 0;
    virtual ~Callback() = default;
  };
  virtual void add_block(td::BufferSlice payload, std::vector<CatChainBlockHash> deps) = 0;
  virtual void debug_add_fork(td::BufferSlice payload, CatChainBlockHeight height,
                              std::vector<CatChainBlockHash> deps) = 0;
  virtual void blame_node(td::uint32 idx) = 0;
  virtual void send_fec_broadcast(td::BufferSlice data) = 0;
  virtual void send_custom_query_data(PublicKeyHash dst, std::string name, td::Promise<td::BufferSlice> promise,
                                      td::Timestamp timeout, td::BufferSlice query) = 0;
  virtual void send_custom_query_data_via(PublicKeyHash dst, std::string name, td::Promise<td::BufferSlice> promise,
                                          td::Timestamp timeout, td::BufferSlice query, td::uint64 max_answer_size,
                                          td::actor::ActorId<adnl::AdnlSenderInterface> via) = 0;
  virtual void send_custom_message_data(PublicKeyHash dst, td::BufferSlice query) = 0;

  virtual void destroy() = 0;

  static td::actor::ActorOwn<CatChainReceiverInterface> create(std::unique_ptr<Callback> callback, CatChainOptions opts,
                                                               td::actor::ActorId<keyring::Keyring> keyring,
                                                               td::actor::ActorId<adnl::Adnl> adnl,
                                                               td::actor::ActorId<overlay::Overlays> overlay_manager,
                                                               std::vector<CatChainNode> ids, PublicKeyHash local_id,
                                                               CatChainSessionId unique_hash, std::string db_root,
                                                               bool allow_unsafe_self_blocks_resync);

  virtual ~CatChainReceiverInterface() = default;
};

}  // namespace catchain

}  // namespace ton
