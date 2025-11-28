/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "bus.h"
#include "full-node.h"

namespace ton::validator::consensus {

namespace {

class BlockAccepterImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<BlockFinalized> event) {
    const auto& block = std::get<BlockCandidate>(event->candidate->block);
    auto block_data = create_block(block.id, block.data.clone()).move_as_ok();
    auto block_parents = owning_bus()->convert_id_to_blocks(event->candidate->parent_id);

    co_return co_await td::actor::ask(owning_bus()->manager, &ManagerFacade::accept_block, block.id, block_data,
                                      block_parents, event->signatures, fullnode::FullNode::broadcast_mode_public,
                                      true);
  }
};

}  // namespace

void BlockAccepter::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<BlockAccepterImpl>("BlockAccepter");
}

}  // namespace ton::validator::consensus
