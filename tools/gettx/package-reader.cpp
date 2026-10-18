#include "package-reader.h"
#include "td/db/RocksDb.h"
#include "tl/tl/tl_object_parse.h"
#include "tl/tl/TlObject.h"
#include "crypto/block/block.h"

namespace ton {

namespace gettx {

PackageReader::PackageReader(std::string db_root, const DbIndexReader::PackageInfo* pkg_info)
    : db_root_(std::move(db_root)), pkg_info_(pkg_info) {}

PackageReader::~PackageReader() = default;

td::Status PackageReader::open() {
  TRY_STATUS(open_index());
  TRY_STATUS(open_package());
  return td::Status::OK();
}

td::Status PackageReader::open_index() {
  // Construct path to .index/ directory
  std::string index_path = PSTRING() << db_root_ << pkg_info_->id.path()
                                      << pkg_info_->id.name() << ".index";

  std::cerr << "DEBUG: Opening package index at: " << index_path << std::endl;

  td::RocksDbOptions db_options;
  auto res = td::RocksDb::open(index_path, std::move(db_options));
  if (res.is_error()) {
    auto error = res.move_as_error();
    return td::Status::Error(PSTRING() << "Failed to open package index at " << index_path << ": " << error.message());
  }
  kv_ = std::make_shared<td::RocksDb>(res.move_as_ok());

  return td::Status::OK();
}

td::Status PackageReader::open_package() {
  // Construct path to .pack file
  std::string package_path = PSTRING() << db_root_ << pkg_info_->id.path()
                                        << pkg_info_->id.name() << ".pack";

  std::cerr << "DEBUG: Opening package file at: " << package_path << "\n";
  std::cerr << "DEBUG: Package ID: " << pkg_info_->id.id << " key=" << pkg_info_->id.key << " temp=" << pkg_info_->id.temp << "\n";

  auto R = Package::open(package_path, true, false);
  if (R.is_error()) {
    return R.move_as_error();
  }

  package_ = std::make_unique<Package>(std::move(R.move_as_ok()));
  return td::Status::OK();
}

td::BufferSlice PackageReader::get_db_key_lt_desc(ton::ShardIdFull shard) {
  return create_serialize_tl_object<ton_api::db_lt_desc_key>(shard.workchain, shard.shard);
}

td::BufferSlice PackageReader::get_db_key_lt_el(ton::ShardIdFull shard, td::uint32 idx) {
  return create_serialize_tl_object<ton_api::db_lt_el_key>(shard.workchain, shard.shard, idx);
}

td::Result<ton::BlockIdExt> PackageReader::get_block_by_lt(ton::AccountIdPrefixFull account_id, ton::LogicalTime lt) const {
  if (!kv_) {
    return td::Status::Error("Index not opened");
  }

  // Try different shard prefix depths
  bool found = false;
  ton::BlockIdExt result_block;

  for (int len = 0; len <= 60; len++) {
    auto s = ton::shard_prefix(account_id, len);
    auto key = get_db_key_lt_desc(s);
    std::string value;

    auto F = kv_->get(key.as_slice(), value);
    if (F.is_error() || F.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
      if (!found) {
        continue;
      } else {
        break;
      }
    }

    found = true;

    auto G = fetch_tl_object<ton_api::db_lt_desc_value>(value, true);
    if (G.is_error()) {
      return G.move_as_error();
    }

    auto g = G.move_as_ok();

    // Check if the LT is within range
    if (lt > static_cast<ton::LogicalTime>(g->last_lt_)) {
      continue;
    }

    std::cerr << "DEBUG: Binary search in shard wc=" << s.workchain << " shard=" << s.shard
              << " first_idx=" << g->first_idx_ << " last_idx=" << g->last_idx_
              << " last_lt=" << g->last_lt_ << std::endl;

    // Binary search for the block
    td::uint32 l = g->first_idx_ - 1;
    ton::BlockIdExt lseq;
    td::uint32 r = g->last_idx_;

    while (r - l > 1) {
      auto x = (r + l) / 2;
      std::cerr << "DEBUG: Binary search step: l=" << l << " r=" << r << " x=" << x << std::endl;

      auto db_key = get_db_key_lt_el(s, x);

      F = kv_->get(db_key.as_slice(), value);
      if (F.is_error() || F.move_as_ok() != td::KeyValue::GetStatus::Ok) {
        return td::Status::Error("Failed to read LT entry");
      }

      auto E = fetch_tl_object<ton_api::db_lt_el_value>(td::BufferSlice{value}, true);
      if (E.is_error()) {
        return E.move_as_error();
      }

      auto e = E.move_as_ok();
      std::cerr << "DEBUG: Entry at x=" << x << " has lt=" << e->lt_ << " seqno=" << e->id_->seqno_ << std::endl;

      int cmp_val = lt > static_cast<ton::LogicalTime>(e->lt_) ? 1 : lt == static_cast<ton::LogicalTime>(e->lt_) ? 0 : -1;
      std::cerr << "DEBUG: Comparing target lt=" << lt << " with entry lt=" << e->lt_ << " cmp=" << cmp_val << std::endl;

      if (cmp_val >= 0) {
        l = x;
        // Convert tl_api blockId to BlockId then BlockIdExt
        ton::BlockId blk_id(e->id_->workchain_, e->id_->shard_, e->id_->seqno_);
        lseq = ton::BlockIdExt{blk_id, e->id_->root_hash_, e->id_->file_hash_};
        std::cerr << "DEBUG: Updated l=" << l << " to seqno=" << e->id_->seqno_ << std::endl;
      } else {
        r = x;
        std::cerr << "DEBUG: Updated r=" << r << std::endl;
      }
    }

    // Return the block at position r (first block with lt >= target_lt)
    if (r <= g->last_idx_) {
      std::cerr << "DEBUG: Binary search complete, returning block at r=" << r << std::endl;
      auto db_key = get_db_key_lt_el(s, r);
      F = kv_->get(db_key.as_slice(), value);
      if (F.is_error() || F.move_as_ok() != td::KeyValue::GetStatus::Ok) {
        return td::Status::Error("Failed to read LT entry for r");
      }

      auto E = fetch_tl_object<ton_api::db_lt_el_value>(td::BufferSlice{value}, true);
      if (E.is_error()) {
        return E.move_as_error();
      }

      auto e = E.move_as_ok();
      ton::BlockId blk_id(e->id_->workchain_, e->id_->shard_, e->id_->seqno_);
      return ton::BlockIdExt{blk_id, e->id_->root_hash_, e->id_->file_hash_};
    }
  }

  return td::Status::Error("Block not found for given logical time");
}

td::Result<std::pair<ton::BlockIdExt, td::BufferSlice>> PackageReader::get_block_data_by_lt(
    ton::AccountIdPrefixFull account_id, ton::LogicalTime lt) const {
  std::cerr << "DEBUG: get_block_data_by_lt called with account_id=" << account_id.account_id_prefix << " lt=" << lt << "\n";

  // First, find the block ID using LT index (binary search)
  auto block_id_result = get_block_by_lt(account_id, lt);
  if (block_id_result.is_error()) {
    return block_id_result.move_as_error();
  }
  auto block_id = block_id_result.move_as_ok();

  // Then read the block file directly
  auto data_result = read_block_file(block_id);
  if (data_result.is_error()) {
    return data_result.move_as_error();
  }
  auto block_data = data_result.move_as_ok();

  return std::make_pair(block_id, std::move(block_data));
}

td::Result<ton::BlockIdExt> PackageReader::get_block_by_seqno(ton::ShardIdFull shard, ton::BlockSeqno seqno) const {
  if (!kv_) {
    return td::Status::Error("Index not opened");
  }

  auto key = get_db_key_lt_desc(shard);
  std::string value;

  auto F = kv_->get(key.as_slice(), value);
  if (F.is_error() || F.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    return td::Status::Error("Shard not found in package index");
  }

  auto G = fetch_tl_object<ton_api::db_lt_desc_value>(value, true);
  if (G.is_error()) {
    return G.move_as_error();
  }

  auto g = G.move_as_ok();

  // Check if the seqno is within range
  if (seqno > static_cast<ton::BlockSeqno>(g->last_seqno_)) {
    return td::Status::Error(PSTRING() << "Seqno " << seqno << " exceeds last seqno " << g->last_seqno_ << " in shard");
  }

  std::cerr << "DEBUG: Binary search by seqno in shard wc=" << shard.workchain << " shard=" << shard.shard
            << " first_idx=" << g->first_idx_ << " last_idx=" << g->last_idx_
            << " last_seqno=" << g->last_seqno_ << std::endl;

  // Binary search for the block by seqno
  td::uint32 l = g->first_idx_ - 1;
  ton::BlockIdExt lseq;
  td::uint32 r = g->last_idx_;

  while (r - l > 1) {
    auto x = (r + l) / 2;
    std::cerr << "DEBUG: Binary search step: l=" << l << " r=" << r << " x=" << x << std::endl;

    auto db_key = get_db_key_lt_el(shard, x);

    F = kv_->get(db_key.as_slice(), value);
    if (F.is_error() || F.move_as_ok() != td::KeyValue::GetStatus::Ok) {
      return td::Status::Error("Failed to read LT entry");
    }

    auto E = fetch_tl_object<ton_api::db_lt_el_value>(td::BufferSlice{value}, true);
    if (E.is_error()) {
      return E.move_as_error();
    }

    auto e = E.move_as_ok();
    std::cerr << "DEBUG: Entry at x=" << x << " has seqno=" << e->id_->seqno_ << " lt=" << e->lt_ << std::endl;

    int cmp_val = seqno > static_cast<ton::BlockSeqno>(e->id_->seqno_) ? 1 : seqno == static_cast<ton::BlockSeqno>(e->id_->seqno_) ? 0 : -1;
    std::cerr << "DEBUG: Comparing target seqno=" << seqno << " with entry seqno=" << e->id_->seqno_ << " cmp=" << cmp_val << std::endl;

    if (cmp_val >= 0) {
      l = x;
      // Convert tl_api blockId to BlockId then BlockIdExt
      ton::BlockId blk_id(e->id_->workchain_, e->id_->shard_, e->id_->seqno_);
      lseq = ton::BlockIdExt{blk_id, e->id_->root_hash_, e->id_->file_hash_};
      std::cerr << "DEBUG: Updated l=" << l << " to seqno=" << e->id_->seqno_ << std::endl;
    } else {
      r = x;
      std::cerr << "DEBUG: Updated r=" << r << std::endl;
    }
  }

  // Return the block at position l (last block with seqno <= target_seqno)
  // If l is still first_idx_ - 1, then no block was found
  if (l >= g->first_idx_) {
    std::cerr << "DEBUG: Binary search complete, returning block at l=" << l << std::endl;
    auto db_key = get_db_key_lt_el(shard, l);
    F = kv_->get(db_key.as_slice(), value);
    if (F.is_error() || F.move_as_ok() != td::KeyValue::GetStatus::Ok) {
      return td::Status::Error("Failed to read LT entry for l");
    }

    auto E = fetch_tl_object<ton_api::db_lt_el_value>(td::BufferSlice{value}, true);
    if (E.is_error()) {
      return E.move_as_error();
    }

    auto e = E.move_as_ok();
    ton::BlockId blk_id(e->id_->workchain_, e->id_->shard_, e->id_->seqno_);
    return ton::BlockIdExt{blk_id, e->id_->root_hash_, e->id_->file_hash_};
  }

  return td::Status::Error("Block not found for given seqno");
}

td::Result<std::pair<ton::BlockIdExt, td::BufferSlice>> PackageReader::get_block_data_by_seqno(
    ton::ShardIdFull shard, ton::BlockSeqno seqno) const {
  std::cerr << "DEBUG: get_block_data_by_seqno called with shard wc=" << shard.workchain
            << " shard=" << shard.shard << " seqno=" << seqno << "\n";

  // First, find the block ID using seqno index (binary search)
  auto block_id_result = get_block_by_seqno(shard, seqno);
  if (block_id_result.is_error()) {
    return block_id_result.move_as_error();
  }
  auto block_id = block_id_result.move_as_ok();

  // Then read the block file directly
  auto data_result = read_block_file(block_id);
  if (data_result.is_error()) {
    return data_result.move_as_error();
  }
  auto block_data = data_result.move_as_ok();

  return std::make_pair(block_id, std::move(block_data));
}

td::Result<td::BufferSlice> PackageReader::read_block_file(const ton::BlockIdExt& block_id) const {
  if (!package_) {
    return td::Status::Error("Package not opened");
  }

  // Read the file that contains this block
  // Filename format: block_(workchain,shard,seqno):root_hash:file_hash
  std::string filename = PSTRING() << "block_" << block_id.to_str();
  std::cerr << "DEBUG: Looking for block file: " << filename << "\n";

  td::BufferSlice file_data;
  bool found = false;
  int file_count = 0;

  // Iterate through package to find the matching file
  auto status = package_->iterate([&filename, &file_data, &found, &file_count](std::string fname, td::BufferSlice data, td::uint64 offset) {
    file_count++;
    // List first few files to see the format
    if (file_count <= 10) {
      std::cerr << "DEBUG: File " << file_count << ": " << fname << " (size=" << data.size() << ")\n";
      // Debug: Show first few bytes of file 1 to verify it's not RocksDB metadata
      if (file_count == 1 && data.size() > 20) {
        auto slice = data.as_slice();
        std::cerr << "DEBUG: First 20 bytes: ";
        char buf[3];
        for (size_t i = 0; i < 20 && i < data.size(); i++) {
          snprintf(buf, sizeof(buf), "%02x", (unsigned char)slice.data()[i]);
          std::cerr << buf;
        }
        std::cerr << "\n";
      }
    }
    if (fname == filename) {
      std::cerr << "DEBUG: Found matching block file, size=" << data.size() << "\n";
      file_data = std::move(data);
      found = true;
      return false;  // Stop iteration (return false = stop)
    }
    return true;  // Continue iteration (return true = continue)
  });

  std::cerr << "DEBUG: Total files scanned: " << file_count << "\n";

  if (status.is_error()) {
    return td::Status::Error(PSTRING() << "Package iteration failed: " << status.move_as_error().message());
  }

  if (!found) {
    return td::Status::Error(PSTRING() << "Block file not found: " << filename);
  }

  return file_data;
}

}  // namespace gettx

}  // namespace ton
