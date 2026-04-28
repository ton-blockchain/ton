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

#include "common/AtomicRef.h"
#include "crypto/vm/db/DynamicBagOfCellsDb.h"
#include "interfaces/block-handle.h"
#include "rocksdb/statistics.h"
#include "td/actor/actor.h"
#include "td/actor/coro_primitives.h"
#include "td/db/RocksDb.h"
#include "td/utils/LRUCache.h"
#include "validator/interfaces/validator-manager.h"

#include "validator.h"

namespace ton::validator {

class StorageStatManager;

class StorageStatDbIn : public td::actor::Actor {
 public:
  class StorageStatLoaderImpl : public StorageStatLoader {
   public:
    td::Ref<vm::Cell> get_storage_dict(WorkchainId wc, StdSmcAddress addr, td::Bits256 dict_hash,
                                       td::Ref<vm::CellSlice> storage) override;
    td::Ref<vm::Cell> get_from_cache(td::Bits256 dict_hash);

    void update(std::shared_ptr<vm::AugmentedDictionary> accounts_dict,
                std::shared_ptr<vm::CellDbReader> cell_db_reader, std::shared_ptr<td::KeyValueReader> meta_reader);
    void update_cache(td::Ref<vm::Cell> cache_root);

   private:
    struct State {
      std::shared_ptr<vm::AugmentedDictionary> accounts_dict;
      std::shared_ptr<vm::CellDbReader> cell_db_reader;
      std::shared_ptr<td::KeyValueReader> meta_reader;
    };
    td::AtomicRef<td::Cnt<State>> state_;
    td::AtomicRef<vm::Cell> cache_;
  };

  StorageStatDbIn(std::shared_ptr<StorageStatLoaderImpl> loader, std::string db_path,
                  td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager)
      : loader_(std::move(loader)), db_path_(std::move(db_path)), opts_(std::move(opts)), manager_(std::move(manager)) {
  }

  void start_up() override;

 private:
  std::shared_ptr<StorageStatLoaderImpl> loader_;
  std::string db_path_;
  td::Ref<ValidatorManagerOptions> opts_;
  td::actor::ActorId<ValidatorManager> manager_;

  std::shared_ptr<rocksdb::Statistics> statistics_;
  std::shared_ptr<td::RocksDbSnapshotStatistics> snapshot_statistics_;

  std::shared_ptr<vm::DynamicBagOfCellsDb::AsyncExecutor> async_executor_;
  std::shared_ptr<td::KeyValue> db_;
  std::unique_ptr<vm::DynamicBagOfCellsDb> boc_, boc_reader_;
  td::actor::CoroMutex mutex_;

  BlockSeqno current_mc_seqno_ = 0;
  td::optional<StdSmcAddress> processing_cur_addr_;
  std::shared_ptr<vm::AugmentedDictionary> current_account_dict_;

  void open_db();
  void init_boc();
  td::actor::Task<> run();
  td::actor::Task<> advance_mc_seqno(std::shared_ptr<vm::AugmentedDictionary> next_account_dict);

  td::actor::Task<> process_all_accounts();

  td::actor::Task<std::shared_ptr<vm::AugmentedDictionary>> load_account_dict(BlockSeqno mc_seqno);
  void db_commit();
  void update_loader();

  std::string db_dir() {
    return db_path_ + "/storage-stat-db/";
  }
  std::string db_dir_to_remove() {
    return db_path_ + "/old-storage-stat-db/";
  }

  static constexpr td::uint32 BIG_ACCOUNT_CELLS = 4000;
  static constexpr td::uint32 BIG_ACCOUNT_CELLS_LOWER = 2000;
};

class StorageStatManager : public td::actor::Actor {
 public:
  StorageStatManager(std::string db_path, td::Ref<ValidatorManagerOptions> opts,
                     td::actor::ActorId<ValidatorManager> manager)
      : db_path_(std::move(db_path)), opts_(std::move(opts)), manager_(std::move(manager)) {
  }

  void start_up() override;

  std::shared_ptr<StorageStatLoader> get_loader() {
    return std::shared_ptr<StorageStatLoader>{loader_};
  }

  // (storage dict root, account total cells)
  void update_cache(std::vector<std::pair<td::Ref<vm::Cell>, td::uint32>> data);

 private:
  std::string db_path_;
  td::Ref<ValidatorManagerOptions> opts_;
  td::actor::ActorId<ValidatorManager> manager_;

  td::actor::ActorOwn<StorageStatDbIn> db_;
  std::shared_ptr<StorageStatDbIn::StorageStatLoaderImpl> loader_ =
      std::make_shared<StorageStatDbIn::StorageStatLoaderImpl>();

  struct Deleter {
    Deleter(const td::Bits256& hash, vm::Dictionary* cache) : hash(hash), cache(cache) {
    }
    Deleter(const Deleter&) = delete;
    Deleter(Deleter&& other) noexcept : hash(other.hash), cache(other.cache) {
      other.cache = nullptr;
    }
    Deleter& operator=(const Deleter&) = delete;
    Deleter& operator=(Deleter&& other) noexcept {
      hash = other.hash;
      cache = other.cache;
      other.cache = nullptr;
      return *this;
    }
    ~Deleter() {
      if (cache) {
        CHECK(cache->lookup_delete_ref(hash).not_null());
        LOG(DEBUG) << "storage stat cache remove " << hash.to_hex();
      }
    }

    td::Bits256 hash = td::Bits256::zero();
    vm::Dictionary* cache;
  };
  vm::Dictionary cache_{256};
  td::LRUCache<td::Bits256, Deleter> cache_lru_{CACHE_MAX_TOTAL_CELLS};

  static constexpr td::uint64 CACHE_MAX_TOTAL_CELLS = 1 << 24;

 public:
  static constexpr td::uint64 CACHE_MIN_ACCOUNT_CELLS = 2000;
};

}  // namespace ton::validator
