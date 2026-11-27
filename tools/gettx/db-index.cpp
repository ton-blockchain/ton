#include "db-index.h"
#include "td/db/RocksDb.h"
#include "td/utils/overloaded.h"
#include "tl/tl/tl_object_parse.h"
#include "tl/tl/TlObject.h"

namespace ton {

namespace gettx {

DbIndexReader::DbIndexReader(std::string db_root, bool include_deleted)
    : db_root_(std::move(db_root)), include_deleted_(include_deleted) {}

DbIndexReader::~DbIndexReader() = default;

td::Status DbIndexReader::open() {
  return load_index();
}

td::Status DbIndexReader::load_index() {
  td::RocksDbOptions db_options;
  auto db_path = db_root_ + "/files/globalindex";

  std::cerr << "DEBUG: About to open globalindex at: " << db_path << std::endl;
  auto res = td::RocksDb::open(db_path, std::move(db_options));

  std::cerr << "DEBUG: RocksDb::open returned, is_error=" << res.is_error() << std::endl;

  if (res.is_error()) {
    auto error = res.move_as_error();
    std::cerr << "DEBUG: Database open failed, returning error status" << std::endl;
    return td::Status::Error(PSTRING() << "Failed to open global index at " << db_path << ": " << error.message());
  }

  std::cerr << "DEBUG: Database open succeeded, creating shared_ptr" << std::endl;
  index_ = std::make_shared<td::RocksDb>(res.move_as_ok());
  std::cerr << "DEBUG: Returning OK status" << std::endl;
  return td::Status::OK();
}

td::Result<std::vector<DbIndexReader::PackageInfo>> DbIndexReader::load_packages() const {
  std::vector<PackageInfo> packages;

  // Read db_files_index_key to get list of all packages
  std::string value;
  auto key = create_serialize_tl_object<ton_api::db_files_index_key>().as_slice();

  auto v = index_->get(key, value);
  if (v.is_error()) {
    return td::Status::Error("Failed to read global index");
  }

  if (v.move_as_ok() != td::KeyValue::GetStatus::Ok) {
    return td::Status::Error("Global index not found");
  }

  auto R = fetch_tl_object<ton_api::db_files_index_value>(td::Slice(value), true);
  if (R.is_error()) {
    return td::Status::Error("Failed to parse index value");
  }

  auto index_value = R.move_as_ok();

  // Load regular packages
  for (auto& pkg_id : index_value->packages_) {
    auto key = create_serialize_tl_object<ton_api::db_files_package_key>(
        static_cast<td::uint32>(pkg_id), false, false);

    std::string pkg_value;
    auto v2 = index_->get(key.as_slice(), pkg_value);
    if (v2.is_error() || v2.move_as_ok() != td::KeyValue::GetStatus::Ok) {
      continue;
    }

    auto R2 = fetch_tl_object<ton_api::db_files_package_value>(td::Slice(pkg_value), true);
    if (R2.is_error()) {
      continue;
    }

    auto pkg = R2.move_as_ok();
    if (pkg->deleted_ && !include_deleted_) {
      continue;
    }

    PackageInfo info;
    info.id = ton::validator::PackageId{static_cast<td::uint32>(pkg_id), false, false};
    info.deleted = pkg->deleted_;

    for (auto& fb : pkg->firstblocks_) {
      ton::ShardIdFull shard{static_cast<ton::WorkchainId>(fb->workchain_), static_cast<ton::ShardId>(fb->shard_)};
      info.first_blocks[shard] = FirstBlockInfo(fb->seqno_, fb->unixtime_, fb->lt_);
    }

    packages.push_back(std::move(info));
  }

  // Load key packages
  for (auto& pkg_id : index_value->key_packages_) {
    auto key = create_serialize_tl_object<ton_api::db_files_package_key>(
        static_cast<td::uint32>(pkg_id), true, false);

    std::string pkg_value;
    auto v2 = index_->get(key.as_slice(), pkg_value);
    if (v2.is_error() || v2.move_as_ok() != td::KeyValue::GetStatus::Ok) {
      continue;
    }

    auto R2 = fetch_tl_object<ton_api::db_files_package_value>(td::Slice(pkg_value), true);
    if (R2.is_error()) {
      continue;
    }

    auto pkg = R2.move_as_ok();
    if (pkg->deleted_ && !include_deleted_) {
      continue;
    }

    PackageInfo info;
    info.id = ton::validator::PackageId{static_cast<td::uint32>(pkg_id), true, false};
    info.deleted = pkg->deleted_;

    for (auto& fb : pkg->firstblocks_) {
      ton::ShardIdFull shard{static_cast<ton::WorkchainId>(fb->workchain_), static_cast<ton::ShardId>(fb->shard_)};
      info.first_blocks[shard] = FirstBlockInfo(fb->seqno_, fb->unixtime_, fb->lt_);
    }

    packages.push_back(std::move(info));
  }

  return packages;
}

td::Result<const DbIndexReader::PackageInfo*> DbIndexReader::find_package_by_lt(
    ton::ShardIdFull shard, ton::LogicalTime lt) const {
  std::cerr << "DEBUG: find_package_by_lt called for shard wc=" << shard.workchain
            << " shard=" << shard.shard << " lt=" << lt << std::endl;

  // Load packages into cache if not already loaded
  if (!cache_loaded_) {
    auto packages = load_packages();
    if (packages.is_error()) {
      std::cerr << "DEBUG: load_packages failed" << std::endl;
      return packages.move_as_error();
    }
    packages_cache_ = packages.move_as_ok();
    cache_loaded_ = true;
    std::cerr << "DEBUG: Loaded " << packages_cache_.size() << " packages into cache" << std::endl;
  }

  const PackageInfo* best_match = nullptr;
  ton::LogicalTime best_lt = 0;

  for (const auto& pkg : packages_cache_) {
    if (pkg.deleted) {
      continue;
    }

    // Show non-key packages in arch0000 range
    if (!pkg.id.key && pkg.id.id < 70000 && pkg.first_blocks.size() > 0) {
      std::cerr << "DEBUG: Found non-key package id=" << pkg.id.id << " with "
                << pkg.first_blocks.size() << " shards:" << std::endl;
      for (const auto& [shard, info] : pkg.first_blocks) {
        std::cerr << "DEBUG:   wc=" << shard.workchain << " shard=" << shard.shard
                  << " first_lt=" << info.lt << std::endl;
      }
    }

    std::cerr << "DEBUG: Checking package id=" << pkg.id.id << " key=" << pkg.id.key
              << " temp=" << pkg.id.temp << " first_blocks count=" << pkg.first_blocks.size() << std::endl;

    auto it = pkg.first_blocks.find(shard);
    if (it != pkg.first_blocks.end()) {
      std::cerr << "DEBUG:   Found shard in package, first_block lt=" << it->second.lt << std::endl;
      if (it->second.lt <= lt && (best_match == nullptr || it->second.lt > best_lt)) {
        best_lt = it->second.lt;
        best_match = &pkg;
        std::cerr << "DEBUG:   This is the best match so far!" << std::endl;
      }
    }
  }

  std::cerr << "DEBUG: best_match=" << best_match << std::endl;

  if (best_match) {
    return best_match;
  }

  return td::Status::Error("Package not found for given logical time");
}

td::Result<const DbIndexReader::PackageInfo*> DbIndexReader::find_package_by_seqno(
    ton::ShardIdFull shard, ton::BlockSeqno seqno) const {
  std::cerr << "DEBUG: find_package_by_seqno called for shard wc=" << shard.workchain
            << " shard=" << shard.shard << " seqno=" << seqno << std::endl;

  // Load packages into cache if not already loaded
  if (!cache_loaded_) {
    auto packages = load_packages();
    if (packages.is_error()) {
      std::cerr << "DEBUG: load_packages failed" << std::endl;
      return packages.move_as_error();
    }
    packages_cache_ = packages.move_as_ok();
    cache_loaded_ = true;
    std::cerr << "DEBUG: Loaded " << packages_cache_.size() << " packages into cache" << std::endl;
  }

  const PackageInfo* best_match = nullptr;
  ton::BlockSeqno best_seqno = 0;

  for (const auto& pkg : packages_cache_) {
    if (pkg.deleted) {
      continue;
    }

    std::cerr << "DEBUG: Checking package id=" << pkg.id.id << " key=" << pkg.id.key
              << " temp=" << pkg.id.temp << " first_blocks count=" << pkg.first_blocks.size() << std::endl;

    auto it = pkg.first_blocks.find(shard);
    if (it != pkg.first_blocks.end()) {
      std::cerr << "DEBUG:   Found shard in package, first_block seqno=" << it->second.seqno << std::endl;
      if (it->second.seqno <= seqno && (best_match == nullptr || it->second.seqno > best_seqno)) {
        best_seqno = it->second.seqno;
        best_match = &pkg;
        std::cerr << "DEBUG:   This is the best match so far!" << std::endl;
      }
    }
  }

  std::cerr << "DEBUG: best_match=" << best_match << std::endl;

  if (best_match) {
    return best_match;
  }

  return td::Status::Error("Package not found for given seqno");
}

}  // namespace gettx

}  // namespace ton
