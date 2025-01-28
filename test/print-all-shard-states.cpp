#include <iostream>
#include <cstring>

#include "td/actor/actor.h"
#include "td/utils/logging.h"

// TON / CellDb includes (проверьте свои пути)
#include "validator/db/celldb.hpp"

// ====================== GLOBAL STORAGE FOR ACTORS =====================
// Чтобы актор LoadCellActor не уничтожился, когда локальная переменная исчезнет.
static td::actor::ActorOwn<ton::validator::CellDb> g_cell_db_actor;
static td::actor::ActorOwn<td::actor::Actor> g_loader_actor;  // LoadCellActor хранить здесь

// ============ 1) Актор, печатающий все ключи (если нужно) ============
class PrintHashesActor : public td::actor::Actor {
 public:
  explicit PrintHashesActor(td::actor::ActorId<ton::validator::CellDb> cell_db)
      : cell_db_(cell_db) {}

  void start_up() override {
    LOG(INFO) << "PrintHashesActor: calling CellDb::print_all_hashes()";
    td::actor::send_closure(cell_db_, &ton::validator::CellDb::print_all_hashes);
    stop();  // завершить работу
  }

 private:
  td::actor::ActorId<ton::validator::CellDb> cell_db_;
};

// ============ Helper: Парсим 64-hex-символов в ton::RootHash ============
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

// ============ MAIN ============

int main(int argc, char* argv[]) {
  // Аргументы: path/to/celldb [64-hex-hash]
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " /path/to/celldb [64-hex-hash]\n";
    return 1;
  }

  // Если нужно, включите логи
  // td::Logger::instance().set_verbosity_level(3);

  std::string celldb_path = argv[1];
  bool load_hash = (argc > 2);
  ton::RootHash cell_hash;
  if (load_hash) {
    cell_hash = parse_hex_hash(argv[2]);
    LOG(INFO) << "We will load hash = " << cell_hash.to_hex();
  }

  // Создаём Scheduler
  td::actor::Scheduler scheduler({1}); // 1-thread

  // Запускаем инициализацию в run_in_context, чтобы всё делалось внутри Actor среды
  scheduler.run_in_context([&] {
    // 1) Строим opts (ValidatorManagerOptions)
    auto opts = ton::validator::ValidatorManagerOptions::create(
        // 2 аргумента, если у вас 2-param create
        ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0, ton::RootHash::zero(), ton::FileHash::zero()},
        ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0, ton::RootHash::zero(), ton::FileHash::zero()}
    );

    // 2) Создаём CellDb
    g_cell_db_actor = td::actor::create_actor<ton::validator::CellDb>(
        "celldb_actor",
        td::actor::ActorId<ton::validator::RootDb>(), // пустой
        celldb_path,
        opts
    );

    // Если захотите печатать все ключи:
     auto printer_actor = td::actor::create_actor<PrintHashesActor>("printer", g_cell_db_actor.get());
  });

  // Главный цикл
  while (scheduler.run(0.1)) {
    // do nothing
  }

  // Останавливаем планировщик
  scheduler.stop();

  LOG(INFO) << "Done. Exiting.";
  return 0;
}
