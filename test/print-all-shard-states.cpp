#include <iostream>
#include <cstring>

#include "td/actor/actor.h"
#include "td/utils/logging.h"

#include "validator/db/celldb.hpp"

static td::actor::ActorOwn<ton::validator::CellDb> g_cell_db_actor;
static td::actor::ActorOwn<td::actor::Actor> g_loader_actor;  // LoadCellActor хранить здесь

class PrintHashesActor : public td::actor::Actor {
 public:
  explicit PrintHashesActor(td::actor::ActorId<ton::validator::CellDb> cell_db)
      : cell_db_(cell_db) {}

  void start_up() override {
    LOG(INFO) << "PrintHashesActor: calling CellDb::print_all_hashes()";
    td::actor::send_closure(cell_db_, &ton::validator::CellDb::print_all_hashes);
    stop();
  }

 private:
  td::actor::ActorId<ton::validator::CellDb> cell_db_;
};

ton::RootHash parse_hex_hash(const std::string &hex_str) {
  if (hex_str.size() != 64) {
    throw std::runtime_error("Root hash must be 64 hex chars");
  }
  auto r = td::hex_decode(hex_str);
  if (r.is_error()) {
    throw std::runtime_error("Invalid hex string: " + r.error().message().str());
  }
  auto data = r.move_as_ok();
  if (data.size() != 32) {
    throw std::runtime_error("Hash must be 32 bytes (64 hex characters).");
  }
  ton::RootHash root;
  std::memcpy(root.as_slice().begin(), data.data(), 32);
  return root;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " /path/to/celldb [64-hex-hash]\n";
    return 1;
  }

  std::string celldb_path = argv[1];
  bool load_hash = (argc > 2);
  ton::RootHash cell_hash;
  if (load_hash) {
    cell_hash = parse_hex_hash(argv[2]);
    LOG(INFO) << "We will load hash = " << cell_hash.to_hex();
  }

  td::actor::Scheduler scheduler({1}); // 1-thread

  scheduler.run_in_context([&] {
    auto opts = ton::validator::ValidatorManagerOptions::create(
        ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0, ton::RootHash::zero(), ton::FileHash::zero()},
        ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0, ton::RootHash::zero(), ton::FileHash::zero()}
    );

    g_cell_db_actor = td::actor::create_actor<ton::validator::CellDb>(
        "celldb_actor",
        td::actor::ActorId<ton::validator::RootDb>(), // пустой
        celldb_path,
        opts
    );

     auto printer_actor = td::actor::create_actor<PrintHashesActor>("printer", g_cell_db_actor.get());
  });

  while (scheduler.run(1)) {
  }

  scheduler.stop();

  LOG(INFO) << "Done. Exiting.";
  return 0;
}
