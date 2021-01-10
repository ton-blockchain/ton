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

#include "interfaces/validator-manager.h"
#include "interfaces/db.h"
#include "shard-client.hpp"

namespace ton {

namespace validator {

struct ValidatorManagerInitResult {
  BlockHandle handle;
  td::Ref<MasterchainState> state;
  td::actor::ActorOwn<ShardClient> clients;

  BlockHandle gc_handle;
  td::Ref<MasterchainState> gc_state;

  BlockHandle last_key_block_handle_;
};

void validator_manager_init(td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager,
                            td::actor::ActorId<Db> db, td::Promise<ValidatorManagerInitResult> promise);

}  // namespace validator

}  // namespace ton
