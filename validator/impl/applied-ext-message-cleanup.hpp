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

#include "interfaces/validator-manager.h"
#include "td/actor/actor.h"

#include "ext-message-pool.hpp"

namespace ton::validator {

class AppliedExtMessageCleanupActor : public td::actor::Actor {
 public:
  explicit AppliedExtMessageCleanupActor(td::actor::ActorId<ExtMessagePool> ext_message_pool,
                                         td::actor::ActorId<ValidatorManager> manager)
      : ext_message_pool_(ext_message_pool), manager_(manager) {
  }

  void cleanup_applied_block(BlockHandle handle, td::Ref<BlockData> block);
  void got_block_data(BlockIdExt block_id, td::Result<td::Ref<BlockData>> block);

 private:
  td::actor::ActorId<ExtMessagePool> ext_message_pool_;
  td::actor::ActorId<ValidatorManager> manager_;
};

}  // namespace ton::validator
