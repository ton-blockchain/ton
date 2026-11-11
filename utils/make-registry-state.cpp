/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include <iostream>
#include <ton/ton-tl.hpp>

#include "adnl/adnl.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api_json.h"
#include "block/block-auto.h"
#include "block/mc-config.h"
#include "lite-client/ext-client.h"
#include "td/actor/MultiPromise.h"
#include "td/actor/actor.h"
#include "td/utils/FileLog.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Random.h"
#include "td/utils/filesystem.h"
#include "td/utils/overloaded.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"
#include "td/utils/tl_storers.h"
#include "tl-utils/lite-utils.hpp"
#include "ton/lite-tl.hpp"
#include "vm/boc.h"
#include "vm/cells/MerkleProof.h"

using namespace ton;

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  td::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  std::string json_file, output_file;
  td::OptionParser p;
  p.set_description("Generate TLB ValRegistryState with the given set of nodes\n");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_option('h', "help", "print help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('i', "input-json", "json file with input (list of nodes and roles)",
               [&](td::Slice arg) { json_file = arg.str(); });
  p.add_option('o', "output-boc", "output file (boc)", [&](td::Slice arg) { output_file = arg.str(); });

  p.run(argc, argv).ensure();
  CHECK(!json_file.empty());
  CHECK(!output_file.empty());

  LOG(INFO) << "Reading " << json_file;
  auto data = td::read_file(json_file).ensure().move_as_ok();
  auto json = td::json_decode(data.as_slice()).ensure().move_as_ok();
  auto state = ton::create_tl_object<ton_api::registryState_state>();
  ton_api::from_json(*state, json.get_object()).ensure();

  vm::Dictionary dict{256};
  for (auto& node : state->nodes_) {
    LOG(INFO) << "Node " << node->adnl_id_ << " roles=" << node->roles_;
    block::gen::ValRegistryNode::Record rec{.roles = (td::uint32)node->roles_, .is_validator = 0};
    vm::CellBuilder cb;
    CHECK(block::gen::pack(cb, rec));
    bool ok = dict.set_builder(node->adnl_id_, cb, vm::Dictionary::SetMode::Add);
    LOG_CHECK(ok) << "Duplicate node " << node->adnl_id_;
  }
  LOG(INFO) << "Total " << state->nodes_.size() << " nodes";

  block::gen::ValRegistryStorage::Record rec{.nodes = dict.get_root(),
                                             .votings = vm::Dictionary{320}.get_root(),
                                             .votings_gc = vm::Dictionary{352}.get_root()};
  td::Ref<vm::Cell> cell;
  CHECK(block::gen::pack_cell(cell, rec));
  auto serialized = vm::std_boc_serialize(cell).ensure().move_as_ok();
  td::write_file(output_file, serialized).ensure();
  LOG(INFO) << "Wrote " << serialized.size() << " bytes to " << output_file;

  return 0;
}
