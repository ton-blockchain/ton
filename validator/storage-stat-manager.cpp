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
#include "blockchain-explorer/blockchain-explorer-query.hpp"
#include "db/db-utils.h"
#include "td/utils/port/path.h"

#include "block-parse.h"
#include "stats-provider.h"
#include "storage-stat-manager.hpp"
#include "transaction.h"

namespace ton::validator {

namespace {

td::BufferSlice db_key_state() {
  return create_serialize_tl_object<ton_api::db_storageStat_key_state>();
}

td::BufferSlice db_key_account(WorkchainId wc, StdSmcAddress addr) {
  return create_serialize_tl_object<ton_api::db_storageStat_key_accountInfo>(wc, addr);
}

struct Account {
  td::uint64 cells = 0;
  td::uint64 bits = 0;
  td::optional<td::Bits256> storage_dict_hash;
  td::Ref<vm::CellSlice> storage;
};

Account unpack_account(const td::Ref<vm::CellSlice>& cs) {
  if (cs.is_null()) {
    return Account{};
  }
  td::Ref<vm::Cell> account_cell = cs->prefetch_ref();
  CHECK(account_cell.not_null());
  block::gen::Account::Record_account rec;
  block::gen::StorageInfo::Record storage_stat;
  block::gen::StorageUsed::Record storage_used;
  block::gen::StorageExtraInfo::Record_storage_extra_info storage_extra;
  CHECK(block::gen::unpack_cell(account_cell, rec));
  CHECK(block::gen::csr_unpack(std::move(rec.storage_stat), storage_stat));
  CHECK(block::gen::csr_unpack(std::move(storage_stat.used), storage_used));

  Account account;
  account.cells = block::tlb::t_VarUInteger_7.as_uint(*storage_used.cells);
  account.bits = block::tlb::t_VarUInteger_7.as_uint(*storage_used.bits);
  if (storage_stat.storage_extra.write().fetch_long(3) == 1) {
    storage_stat.storage_extra->prefetch_bits_to(account.storage_dict_hash.value_force());
  }
  account.storage = block::Account::storage_without_extra_currencies(rec.storage);
  CHECK(account.storage.not_null());
  return account;
}

}  // namespace

td::Ref<vm::Cell> StorageStatDbIn::StorageStatLoaderImpl::get_storage_dict(WorkchainId wc, StdSmcAddress addr,
                                                                           td::Bits256 dict_hash,
                                                                           td::Ref<vm::CellSlice> storage) {
  if (auto cached = get_from_cache(dict_hash); cached.not_null()) {
    return cached;
  }
  if (wc != basechainId) {
    LOG(DEBUG) << "get_storage_dict " << wc << ":" << addr.to_hex() << " : workchain != " << basechainId;
    return {};
  }
  if (storage.is_null()) {
    LOG(DEBUG) << "get_storage_dict " << wc << ":" << addr.to_hex() << " : storage is null";
    return {};
  }
  auto state = state_.load();
  if (state.is_null()) {
    LOG(DEBUG) << "get_storage_dict " << wc << ":" << addr.to_hex() << " : not inited";
    return {};
  }
  std::string value;
  if (state->meta_reader->get(db_key_account(wc, addr), value).ensure().move_as_ok() ==
      td::KeyValueReader::GetStatus::NotFound) {
    LOG(DEBUG) << "get_storage_dict " << wc << ":" << addr.to_hex() << " : not stored";
    return {};
  }
  td::Bits256 stored_dict_hash =
      fetch_tl_object<ton_api::db_storageStat_accountInfo>(value, true).ok()->dict_root_hash_;
  td::Ref<vm::Cell> stored_dict_root =
      state->cell_db_reader->load_cell(stored_dict_hash.as_slice()).ensure().move_as_ok();
  if (stored_dict_hash == dict_hash) {
    return stored_dict_root;
  }
  Account account = unpack_account(state->accounts_dict->lookup(addr));
  CHECK(account.storage_dict_hash);
  CHECK(account.storage_dict_hash.value() == stored_dict_hash);
  CHECK(account.storage.not_null());
  block::AccountStorageStat stat{std::move(stored_dict_root), account.storage->prefetch_all_refs(), account.cells,
                                 account.bits};
  auto S = stat.replace_roots(storage->prefetch_all_refs());
  if (S.is_error()) {
    LOG(DEBUG) << "get_storage_dict " << wc << ":" << addr.to_hex() << " : " << S;
    return {};
  }
  auto r_root = stat.get_dict_root();
  if (r_root.is_error()) {
    LOG(DEBUG) << "get_storage_dict " << wc << ":" << addr.to_hex() << " : " << r_root.move_as_error();
    return {};
  }
  auto root = r_root.move_as_ok();
  if (root->get_hash().bits256() != dict_hash) {
    LOG(DEBUG) << "get_storage_dict " << wc << ":" << addr.to_hex()
               << " : hash mismatch: computed=" << root->get_hash().to_hex() << " expected=" << dict_hash.to_hex();
    return {};
  }
  return root;
}

td::Ref<vm::Cell> StorageStatDbIn::StorageStatLoaderImpl::get_from_cache(td::Bits256 dict_hash) {
  auto root = vm::Dictionary{cache_.load(), 256}.lookup_ref(dict_hash);
  if (root.not_null()) {
    CHECK(root->get_hash().bits256() == dict_hash);
  }
  return root;
}

void StorageStatDbIn::StorageStatLoaderImpl::update(std::shared_ptr<vm::AugmentedDictionary> accounts_dict,
                                                    std::shared_ptr<vm::CellDbReader> cell_db_reader,
                                                    std::shared_ptr<td::KeyValueReader> meta_reader) {
  state_.store(
      td::make_ref<td::Cnt<State>>(State{std::move(accounts_dict), std::move(cell_db_reader), std::move(meta_reader)}));
}

void StorageStatDbIn::StorageStatLoaderImpl::update_cache(td::Ref<vm::Cell> cache_root) {
  cache_.store(std::move(cache_root));
}

void StorageStatDbIn::start_up() {
  async_executor_ = std::make_shared<CellDbAsyncExecutor>(actor_id(this));
  td::rmrf(db_dir_to_remove()).ignore();  // Maybe left from earlier run
  auto run_outer = [](StorageStatDbIn* self) -> td::actor::Task<> {
    auto result = co_await self->run().wrap();
    result.ensure();
    co_return {};
  };
  run_outer(this).start().detach();
}

void StorageStatDbIn::open_db() {
  td::RocksDbOptions db_options;
  if (!opts_->get_disable_rocksdb_stats()) {
    statistics_ = td::RocksDb::create_statistics();
    snapshot_statistics_ = std::make_shared<td::RocksDbSnapshotStatistics>();
    db_options.snapshot_statistics = snapshot_statistics_;
  }
  db_options.statistics = statistics_;
  db_options.merge_operator = std::make_shared<MergeOperatorAddCellRefcnt>();
  td::mkpath(db_dir()).ensure();
  db_ = std::make_shared<td::RocksDb>(td::RocksDb::open(db_dir(), std::move(db_options)).ensure().move_as_ok());
}

void StorageStatDbIn::init_boc() {
  vm::DynamicBagOfCellsDb::CreateV2Options boc_v2_options{
      .extra_threads = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 8u),
      .executor = {},
      .cache_ttl_max = 2000,
      .cache_size_max = 1000000};
  boc_ = vm::DynamicBagOfCellsDb::create_v2(boc_v2_options);
  boc_->set_loader(std::make_unique<vm::CellLoader>(db_->snapshot())).ensure();
}

td::actor::Task<> StorageStatDbIn::run() {
  open_db();
  std::string value;
  if (CO_TRY(db_->get(db_key_state(), value)) == td::KeyValueReader::GetStatus::Ok) {
    auto f = CO_TRY(fetch_tl_object<ton_api::db_storageStat_state>(value, true));
    current_mc_seqno_ = f->mc_seqno_;
    if (f->flags_ & 1) {
      processing_cur_addr_ = f->processing_cur_addr_;
    }
    BlockSeqno gc_seqno = co_await td::actor::ask(manager_, &ValidatorManager::get_gc_masterchain_seqno);
    if (gc_seqno > current_mc_seqno_) {
      LOG(ERROR) << "Current seqno = " << current_mc_seqno_ << ", but gc seqno = " << gc_seqno << ". Clearing db";
      db_ = {};
      td::rename(db_dir(), db_dir_to_remove()).ensure();
      td::rmrf(db_dir_to_remove()).ensure();
      open_db();
      LOG(ERROR) << "Cleared db";
    }
    LOG(INFO) << "Current mc seqno = " << current_mc_seqno_;
  } else {
    current_mc_seqno_ = (co_await td::actor::ask(manager_, &ValidatorManager::get_shard_client_state, false)).seqno();
    processing_cur_addr_ = StdSmcAddress::zero();
    LOG(INFO) << "Initializing db: current mc seqno = " << current_mc_seqno_;
  }
  init_boc();
  db_commit();
  current_account_dict_ = co_await load_account_dict(current_mc_seqno_);
  update_loader();

  if (processing_cur_addr_) {
    auto run_outer = [](StorageStatDbIn* self) -> td::actor::Task<> {
      auto result = co_await self->process_all_accounts().wrap();
      result.ensure();
      co_return {};
    };
    run_outer(this).start().detach();
  }

  while (true) {
    td::actor::send_closure(manager_, &ValidatorManager::update_storage_stat_db_masterchain_seqno, current_mc_seqno_);
    auto S = co_await td::actor::ask(manager_, &ValidatorManager::wait_shard_client_state, current_mc_seqno_ + 1,
                                     td::Timestamp::in(3600.0))
                 .wrap();
    if (S.is_error()) {
      continue;
    }
    auto next_account_dict = co_await load_account_dict(current_mc_seqno_ + 1);
    co_await advance_mc_seqno(std::move(next_account_dict));
  }
  co_return {};
}

td::actor::Task<> StorageStatDbIn::advance_mc_seqno(std::shared_ptr<vm::AugmentedDictionary> next_account_dict) {
  auto lock = co_await mutex_.lock();
  LOG(DEBUG) << "Advancing to seqno " << current_mc_seqno_ + 1;
  td::Timer timer;

  size_t total_diffs = 0;
  size_t updated = 0;
  bool ok = current_account_dict_->scan_diff(
      *next_account_dict, [&](td::ConstBitPtr addr, int key_len, Ref<vm::CellSlice> old_cs, Ref<vm::CellSlice> new_cs) {
        ++total_diffs;
        CHECK(key_len == 256);
        Account old_acc = unpack_account(current_account_dict_->extract_value(std::move(old_cs)));
        Account new_acc = unpack_account(next_account_dict->extract_value(std::move(new_cs)));
        if (old_acc.storage_dict_hash == new_acc.storage_dict_hash) {
          return true;
        }
        if (old_acc.cells < BIG_ACCOUNT_CELLS_LOWER && new_acc.cells < BIG_ACCOUNT_CELLS_LOWER) {
          return true;
        }
        auto key = db_key_account(0, StdSmcAddress{addr});
        std::string value;
        bool stored = boc_->meta_get(key, value).ensure().move_as_ok() == td::KeyValueReader::GetStatus::Ok;
        bool store = new_acc.storage_dict_hash &&
                     (new_acc.cells >= BIG_ACCOUNT_CELLS || (stored && new_acc.cells >= BIG_ACCOUNT_CELLS_LOWER));
        if (!stored && !store) {
          return true;
        }
        td::Ref<vm::Cell> old_dict_root;
        if (stored) {
          auto f = fetch_tl_object<ton_api::db_storageStat_accountInfo>(value, true).ensure().move_as_ok();
          CHECK(old_acc.storage_dict_hash);
          CHECK(f->dict_root_hash_ == old_acc.storage_dict_hash.value());
          old_dict_root = boc_->load_cell(f->dict_root_hash_.as_slice()).ensure().move_as_ok();
          boc_->dec(old_dict_root);
        }
        if (store) {
          block::AccountStorageStat stat;
          if (stored) {
            stat = block::AccountStorageStat{std::move(old_dict_root), old_acc.storage->prefetch_all_refs(),
                                             old_acc.cells, old_acc.bits};
          }
          stat.replace_roots(new_acc.storage->prefetch_all_refs()).ensure();
          auto new_dict_root = stat.get_dict_root().ensure().move_as_ok();
          CHECK(new_dict_root->get_hash().bits256() == new_acc.storage_dict_hash.value());
          boc_->inc(new_dict_root);
          boc_->meta_set(key, create_serialize_tl_object<ton_api::db_storageStat_accountInfo>(
                                  new_acc.storage_dict_hash.value()))
              .ensure();
        } else {
          boc_->meta_erase(key).ensure();
        }
        FLOG(DEBUG) {
          sb << "Update account 0:" << addr.to_hex(256) << " : ";
          if (stored && store) {
            sb << "update";
          } else if (stored) {
            sb << "remove";
          } else {
            sb << "add";
          }
          sb << " dict (size " << old_acc.cells << " -> " << new_acc.cells << ")";
        };
        ++updated;
        return true;
      });
  CHECK(ok);

  co_await boc_->prepare_commit_async(async_executor_);
  ++current_mc_seqno_;
  current_account_dict_ = std::move(next_account_dict);
  db_commit();
  update_loader();
  LOG(INFO) << "Advanced to seqno " << current_mc_seqno_ << " in " << timer.elapsed()
            << " s : total_diffs=" << total_diffs << ", updated " << updated << " dicts";

  co_return {};
}

td::actor::Task<> StorageStatDbIn::process_all_accounts() {
  CHECK(processing_cur_addr_);
  StdSmcAddress cur_addr = processing_cur_addr_.value();
  bool is_first = true;
  LOG(INFO) << "Processing all accounts starting from 0:" << cur_addr.to_hex();
  td::Timestamp pause_at = td::Timestamp::in(0.05);

  td::Timestamp log_stats_at = td::Timestamp::now();
  size_t stats_processed_accounts = 0, stats_big_accounts = 0, stats_stored_dict = 0;
  ProcessStatus status{manager_, "storage_stat_db.processing_all_accounts"};

  while (true) {
    if (log_stats_at.is_in_past()) {
      log_stats_at = td::Timestamp::in(60.0);
      std::string s = PSTRING() << "cur_addr=0:" << cur_addr.to_hex() << " processed total=" << stats_processed_accounts
                                << " big=" << stats_big_accounts << " stored=" << stats_stored_dict;
      LOG(INFO) << "Processing all accounts: " << s;
      status.set_status(std::move(s));
    }
    if (pause_at.is_in_past()) {
      processing_cur_addr_ = cur_addr;
      {
        auto lock = co_await mutex_.lock();
        db_commit();
      }
      co_await td::actor::coro_sleep(td::Timestamp::in(0.2));
      pause_at = td::Timestamp::in(0.05);
    }
    td::Ref<vm::CellSlice> cs = current_account_dict_->lookup_nearest_key(cur_addr, true, is_first);
    is_first = false;
    if (cs.is_null()) {
      break;
    }
    ++stats_processed_accounts;
    Account account = unpack_account(current_account_dict_->extract_value(cs));
    if (account.cells < BIG_ACCOUNT_CELLS || !account.storage_dict_hash) {
      continue;
    }
    auto lock = co_await mutex_.lock();
    ++stats_big_accounts;
    std::string value;
    auto key = db_key_account(0, cur_addr);
    if (boc_->meta_get(key, value).ensure().move_as_ok() == td::KeyValueReader::GetStatus::Ok) {
      LOG(DEBUG) << "Processing all accounts: 0:" << cur_addr.to_hex() << " (" << account.cells
                 << " cells) already stored";
      continue;
    }
    ++stats_stored_dict;
    td::Timer timer;
    block::AccountStorageStat stat;
    stat.replace_roots(account.storage->prefetch_all_refs()).ensure();
    auto new_dict_root = stat.get_dict_root().ensure().move_as_ok();
    CHECK(new_dict_root->get_hash().bits256() == account.storage_dict_hash.value());
    boc_->inc(new_dict_root);
    co_await boc_->prepare_commit_async(async_executor_);
    boc_->meta_set(key,
                   create_serialize_tl_object<ton_api::db_storageStat_accountInfo>(account.storage_dict_hash.value()))
        .ensure();
    processing_cur_addr_ = cur_addr;
    db_commit();
    update_loader();
    LOG(DEBUG) << "Processing all accounts: 0:" << cur_addr.to_hex() << " (" << account.cells
               << " cells) storing dict took " << timer.elapsed() << " s";
    lock.release();
    co_await td::actor::coro_sleep(td::Timestamp::in(std::min(5.0, timer.elapsed() * 3)));
    pause_at = td::Timestamp::in(0.05);
  }
  LOG(INFO) << "Processing all accounts: done";
  processing_cur_addr_ = {};
  db_commit();
  co_return {};
}

td::actor::Task<std::shared_ptr<vm::AugmentedDictionary>> StorageStatDbIn::load_account_dict(BlockSeqno seqno) {
  if (seqno == 0) {
    co_return std::make_shared<vm::AugmentedDictionary>(256, block::tlb::aug_ShardAccounts);
  }
  // TODO: support monitoring not all shards
  auto mc_block_handle = co_await td::actor::ask(manager_, &ValidatorManager::get_block_by_seqno_from_db,
                                                 AccountIdPrefixFull{masterchainId, 0}, seqno);
  CHECK(mc_block_handle->id().seqno() == seqno);
  auto mc_state = td::Ref<MasterchainState>{
      co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db, mc_block_handle)};
  std::vector<td::actor::StartedTask<td::Ref<ShardState>>> shard_state_tasks;
  for (auto shard : mc_state->get_shards()) {
    CHECK(shard->shard().workchain == basechainId);
    shard_state_tasks.push_back(
        td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db_short, shard->top_block_id()));
  }
  std::vector<td::Ref<ShardState>> shard_states = co_await td::actor::all(std::move(shard_state_tasks));
  auto dict = std::make_shared<vm::AugmentedDictionary>(256, block::tlb::aug_ShardAccounts);

  std::sort(shard_states.begin(), shard_states.end(),
            [](const td::Ref<ShardState>& a, const td::Ref<ShardState>& b) { return a->get_shard() < b->get_shard(); });
  std::vector<std::pair<ShardIdFull, vm::AugmentedDictionary>> stack;
  for (const auto& state : shard_states) {
    block::gen::ShardStateUnsplit::Record rec;
    CHECK(block::gen::unpack_cell(state->root_cell(), rec));
    vm::AugmentedDictionary dict2{vm::load_cell_slice_ref(rec.accounts), 256, block::tlb::aug_ShardAccounts};
    CHECK(dict->combine_with(dict2));
  }
  co_return dict;
}

void StorageStatDbIn::db_commit() {
  CHECK(db_);
  CHECK(boc_);
  auto f = create_tl_object<ton_api::db_storageStat_state>();
  f->mc_seqno_ = current_mc_seqno_;
  if (processing_cur_addr_) {
    f->flags_ = 1;
    f->processing_cur_addr_ = processing_cur_addr_.value();
  }
  boc_->meta_set(db_key_state(), serialize_tl_object(f, true)).ensure();

  db_->begin_write_batch().ensure();
  vm::CellStorer stor{*db_};
  boc_->commit(stor).ensure();
  db_->commit_write_batch().ensure();
  boc_->set_loader(std::make_unique<vm::CellLoader>(db_->snapshot())).ensure();
}

void StorageStatDbIn::update_loader() {
  if (!boc_reader_) {
    boc_reader_ = vm::DynamicBagOfCellsDb::create();
  }
  boc_reader_->set_loader(std::make_unique<vm::CellLoader>(db_->snapshot()));
  loader_->update(current_account_dict_, boc_reader_->get_cell_db_reader(), db_->snapshot());
}

void StorageStatManager::start_up() {
  if (opts_->get_storage_stat_db_enabled()) {
    db_ = td::actor::create_actor<StorageStatDbIn>("StorageStatDbIn", loader_, db_path_, opts_, manager_);
  }
}

void StorageStatManager::update_cache(std::vector<std::pair<td::Ref<vm::Cell>, td::uint32>> data) {
  for (auto& [cell, size] : data) {
    if (size < CACHE_MIN_ACCOUNT_CELLS) {
      continue;
    }
    td::Bits256 hash = cell->get_hash().bits();
    LOG(DEBUG) << "storage stat cache update " << hash.to_hex() << " " << size;
    cache_.set_ref(hash, cell);
    cache_lru_.put(hash, Deleter{hash, &cache_}, true, size);
  }
  loader_->update_cache(cache_.get_root_cell());
}

}  // namespace ton::validator
