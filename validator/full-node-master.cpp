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
#include "adnl/utils.hpp"
#include "auto/tl/lite_api.h"
#include "common/delay.h"
#include "td/utils/SharedSlice.h"
#include "tl-utils/lite-utils.hpp"
#include "ton/ton-shard.h"
#include "ton/ton-tl.hpp"

#include "full-node-master.hpp"
#include "full-node-queries.hpp"

namespace ton {

namespace validator {

namespace fullnode {

td::actor::Task<td::BufferSlice> FullNodeMasterImpl::receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice query) {
  CO_TRY(fetch_tl_prefix<ton_api::tonNode_query>(query, true));
  co_return co_await query_handler_.handle_query(std::move(query), src, QuerySource::full_node_master);
}

void FullNodeMasterImpl::start_up() {
  class Cb : public adnl::Adnl::Callback {
   public:
    Cb(td::actor::ActorId<FullNodeMasterImpl> id) : id_(std::move(id)) {
    }
    void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeMasterImpl::receive_query, src, std::move(data), std::move(promise));
    }

   private:
    td::actor::ActorId<FullNodeMasterImpl> id_;
  };

  td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, adnl_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::tonNode_query::ID),
                          std::make_unique<Cb>(actor_id(this)));

  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::actor::ActorOwn<adnl::AdnlExtServer>> R) {
        R.ensure();
        R.move_as_ok().release();
      });
  td::actor::send_closure(adnl_, &adnl::Adnl::create_ext_server, std::vector<adnl::AdnlNodeIdShort>{adnl_id_},
                          std::vector<td::uint16>{port_}, std::move(P));
}

FullNodeMasterImpl::FullNodeMasterImpl(adnl::AdnlNodeIdShort adnl_id, td::uint16 port, FileHash zero_state_file_hash,
                                       td::actor::ActorId<keyring::Keyring> keyring,
                                       td::actor::ActorId<adnl::Adnl> adnl,
                                       td::actor::ActorId<ValidatorManagerInterface> validator_manager)
    : adnl_id_(adnl_id)
    , port_(port)
    , zero_state_file_hash_(zero_state_file_hash)
    , keyring_(keyring)
    , adnl_(adnl)
    , validator_manager_(validator_manager)
    , query_handler_(validator_manager) {
}

td::actor::ActorOwn<FullNodeMaster> FullNodeMaster::create(
    adnl::AdnlNodeIdShort adnl_id, td::uint16 port, FileHash zero_state_file_hash,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<ValidatorManagerInterface> validator_manager) {
  return td::actor::create_actor<FullNodeMasterImpl>("tonnode", adnl_id, port, zero_state_file_hash, keyring, adnl,
                                                     validator_manager);
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
