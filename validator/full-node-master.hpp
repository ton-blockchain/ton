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

#include "full-node-master.h"
#include "full-node-queries.hpp"

namespace ton {

namespace validator {

namespace fullnode {

class FullNodeMasterImpl : public FullNodeMaster {
 public:
  void start_up() override;

  td::actor::Task<td::BufferSlice> receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice query);

  FullNodeMasterImpl(adnl::AdnlNodeIdShort adnl_id, td::uint16 port, FileHash zero_state_file_hash,
                     td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                     td::actor::ActorId<ValidatorManagerInterface> validator_manager);

 private:
  adnl::AdnlNodeIdShort adnl_id_;
  td::uint16 port_;
  FileHash zero_state_file_hash_;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  FullNodeQueryHandler query_handler_;
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
