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

#include "Torrent.h"

#include "td/utils/Status.h"
#include "td/utils/crypto.h"
#include "td/utils/port/Stat.h"
#include "td/utils/tl_helpers.h"
#include "td/utils/port/path.h"

namespace ton {

td::Result<Torrent> Torrent::open(Options options, td::Bits256 hash) {
  Torrent res(hash);
  if (!options.in_memory) {
    if (options.root_dir.empty()) {
      options.root_dir = ".";
    }
    res.set_root_dir(options.root_dir);
  }
  return std::move(res);
}

td::Result<Torrent> Torrent::open(Options options, TorrentMeta meta) {
  Torrent res(td::Bits256(meta.info.get_hash()));
  TRY_STATUS(res.init_info(std::move(meta.info)));
  if (meta.header) {
    TRY_STATUS(res.set_header(meta.header.unwrap()));
  }
  if (meta.root_proof.not_null()) {
    TRY_STATUS(res.merkle_tree_.add_proof(meta.root_proof));
  }
  if (!options.in_memory) {
    if (options.root_dir.empty()) {
      options.root_dir = ".";
    }
    res.set_root_dir(options.root_dir);
  }
  if (options.validate) {
    res.validate();
  }
  return std::move(res);
}

td::Result<Torrent> Torrent::open(Options options, td::Slice meta_str) {
  TRY_RESULT(meta, TorrentMeta::deserialize(meta_str));
  return open(std::move(options), std::move(meta));
}

const Torrent::Info &Torrent::get_info() const {
  CHECK(inited_info_);
  return info_;
}

struct IterateInfo {
  td::uint64 piece_offset;
  td::uint64 chunk_offset;
  td::uint64 size;
};

template <class F>
td::Status Torrent::iterate_piece(Info::PieceInfo piece, F &&f) {
  auto chunk_it = std::lower_bound(chunks_.begin(), chunks_.end(), piece.offset, [](auto &chunk, auto &piece_offset) {
    return chunk.offset + chunk.size <= piece_offset;
  });

  td::uint64 size = 0;
  for (; chunk_it != chunks_.end(); chunk_it++) {
    if (chunk_it->offset >= piece.offset + piece.size) {
      break;
    }
    if (chunk_it->size == 0) {
      continue;
    }
    auto l = td::max(chunk_it->offset, piece.offset);
    auto r = td::min(chunk_it->offset + chunk_it->size, piece.offset + piece.size);
    CHECK(l < r);

    IterateInfo info;
    info.piece_offset = l - piece.offset;
    info.chunk_offset = l - chunk_it->offset;
    info.size = r - l;
    size += info.size;
    TRY_STATUS(f(chunk_it, info));
  }
  LOG_CHECK(size == piece.size) << size << " vs " << piece.size;
  return td::Status::OK();
}

bool Torrent::is_piece_ready(td::uint64 piece_i) const {
  if (!inited_info_) {
    return false;
  }
  CHECK(piece_i < info_.pieces_count());
  return piece_is_ready_[piece_i];
}

td::optional<size_t> Torrent::get_files_count() const {
  if (header_) {
    return header_.value().files_count;
  }
  return {};
}
td::CSlice Torrent::get_file_name(size_t i) const {
  return chunks_.at(i + 1).name;
}
td::uint64 Torrent::get_file_size(size_t i) const {
  return chunks_.at(i + 1).size;
}
td::uint64 Torrent::get_file_ready_size(size_t i) const {
  return chunks_.at(i + 1).ready_size;
}

Torrent::PartsRange Torrent::get_file_parts_range(size_t i) {
  auto begin = chunks_.at(i + 1).offset;
  auto end = begin + chunks_.at(i + 1).size;

  PartsRange res;
  res.begin = begin / info_.piece_size;
  res.end = (end + info_.piece_size - 1) / info_.piece_size;
  return res;
}

Torrent::PartsRange Torrent::get_header_parts_range() const {
  CHECK(inited_info_);
  PartsRange res;
  res.begin = 0;
  res.end = header_pieces_count_;
  return res;
}

TD_WARN_UNUSED_RESULT td::Status Torrent::ChunkState::get_piece(td::MutableSlice dest, td::uint64 offset,
                                                                Cache *cache) {
  if (dest.empty()) {
    return td::Status::OK();
  }

  if (cache != nullptr) {
    auto global_offset = offset + this->offset;
    if (cache->offset > global_offset || cache->offset + cache->size < global_offset + dest.size()) {
      auto load_size = td::min(size - offset, (td::uint64)cache->slice.size());
      cache->size = 0;
      TRY_STATUS(get_piece(cache->slice.as_slice().truncate(load_size), offset));
      cache->offset = global_offset;
      cache->size = load_size;
    }

    dest.copy_from(cache->slice.as_slice().substr(global_offset - cache->offset, dest.size()));
    CHECK(cache->slice.size() >= dest.size());
    return td::Status::OK();
  }

  TRY_RESULT(size, data.view_copy(dest, offset));
  if (size != dest.size()) {
    return td::Status::Error("Failed to read the whole chunk");
  }
  return td::Status::OK();
}

std::string Torrent::get_stats_str() const {
  td::StringBuilder sb;
  auto o_n = get_files_count();
  if (!o_n) {
    return "NO HEADER YET\n";
  }
  for (size_t i = 0, n = o_n.unwrap(); i < n; i++) {
    auto size = get_file_size(i);
    auto ready_size = get_file_ready_size(i);
    sb << get_file_name(i) << "\t" << 100 * ready_size / size << "%%  " << td::format::as_size(ready_size) << "/"
       << td::format::as_size(size) << "\n";
  }
  return sb.as_cslice().str();
}

void Torrent::validate() {
  if (!inited_info_ || !header_) {
    return;
  }

  std::fill(piece_is_ready_.begin(), piece_is_ready_.end(), false);
  not_ready_piece_count_ = info_.pieces_count();

  included_ready_size_ = 0;
  for (auto &chunk : chunks_) {
    chunk.ready_size = 0;
    if (root_dir_) {
      if (td::stat(get_chunk_path(chunk.name)).is_error()) {
        continue;
      }
    }
    init_chunk_data(chunk);
  }

  std::vector<td::UInt256> hashes;
  std::vector<std::pair<size_t, td::Bits256>> pieces;

  auto flush = [&] {
    for (size_t piece_i : merkle_tree_.add_pieces(std::move(pieces))) {
      auto piece = info_.get_piece_info(piece_i);
      iterate_piece(piece, [&](auto it, auto info) {
        it->ready_size += info.size;
        if (!it->excluded) {
          included_ready_size_ += info.size;
        }
        return td::Status::OK();
      });
      piece_is_ready_[piece_i] = true;
      ready_parts_count_++;
      CHECK(not_ready_piece_count_);
      not_ready_piece_count_--;
    }
    hashes.clear();
    pieces.clear();
  };

  td::BufferSlice buf(info_.piece_size);
  ChunkState::Cache cache;
  cache.slice = td::BufferSlice(td::max(8u << 20, info_.piece_size));
  for (size_t piece_i = 0; piece_i < info_.pieces_count(); piece_i++) {
    auto piece = info_.get_piece_info(piece_i);
    td::Sha256State sha256;
    sha256.init();
    bool skipped = false;
    auto is_ok = iterate_piece(piece, [&](auto it, auto info) {
      if (!it->data) {
        skipped = true;
        return td::Status::Error("No such file");
      }
      if (!it->has_piece(info.chunk_offset, info.size)) {
        return td::Status::Error("Don't have piece");
      }
      auto dest = buf.as_slice().truncate(info.size);
      TRY_STATUS(it->get_piece(dest, info.chunk_offset, &cache));
      sha256.feed(dest);
      return td::Status::OK();
    });
    if (is_ok.is_error()) {
      LOG_IF(ERROR, !skipped) << "Failed: " << is_ok;
      continue;
    }
    td::Bits256 hash;
    sha256.extract(hash.as_slice());
    pieces.emplace_back(piece_i, hash);
  }
  flush();
}

td::Result<std::string> Torrent::get_piece_data(td::uint64 piece_i) {
  if (!inited_info_) {
    return td::Status::Error("Torrent info not inited");
  }
  CHECK(piece_i < info_.pieces_count());
  if (!piece_is_ready_[piece_i]) {
    return td::Status::Error("Piece is not ready");
  }
  auto it = pending_pieces_.find(piece_i);
  if (it != pending_pieces_.end()) {
    return it->second;
  }
  auto it2 = in_memory_pieces_.find(piece_i);
  if (it2 != in_memory_pieces_.end()) {
    return it2->second.data;
  }
  auto piece = info_.get_piece_info(piece_i);

  std::string res(piece.size, '\0');
  iterate_piece(piece, [&](auto it, auto info) {
    return it->get_piece(td::MutableSlice(res).substr(info.piece_offset, info.size), info.chunk_offset);
  });
  return res;
}

td::Result<td::Ref<vm::Cell>> Torrent::get_piece_proof(td::uint64 piece_i) {
  if (!inited_info_) {
    return td::Status::Error("Torrent info not inited");
  }
  CHECK(piece_i < info_.pieces_count());
  return merkle_tree_.gen_proof(piece_i, piece_i);
}

td::Status Torrent::add_piece(td::uint64 piece_i, td::Slice data, td::Ref<vm::Cell> proof) {
  if (fatal_error_.is_error()) {
    return fatal_error_.clone().move_as_error_prefix("Fatal error: ");
  }
  if (!inited_info_) {
    return td::Status::Error("Torrent info not inited");
  }
  if (!proof.is_null()) {
    TRY_STATUS(merkle_tree_.add_proof(proof));
  }
  CHECK(piece_i < info_.pieces_count());
  if (piece_is_ready_[piece_i]) {
    return td::Status::OK();
  }
  td::Bits256 hash;
  td::sha256(data, hash.as_slice());
  TRY_RESULT(expected_hash, merkle_tree_.get_piece_hash(piece_i));
  if (expected_hash != hash) {
    return td::Status::Error("Hash mismatch");
  }
  piece_is_ready_[piece_i] = true;
  ready_parts_count_++;

  if (chunks_.empty() || !enabled_wirte_to_files_) {
    return add_pending_piece(piece_i, data);
  }
  return add_validated_piece(piece_i, data);
}

td::Status Torrent::add_proof(td::Ref<vm::Cell> proof) {
  if (!inited_info_) {
    return td::Status::Error("Torrent info not inited");
  }
  return merkle_tree_.add_proof(std::move(proof));
}

td::Status Torrent::add_pending_piece(td::uint64 piece_i, td::Slice data) {
  pending_pieces_[piece_i] = data.str();

  if (piece_i < header_pieces_count_) {
    auto piece = info_.get_piece_info(piece_i);
    auto dest = header_str_.as_slice().substr(piece.offset);
    data.truncate(dest.size());
    dest.copy_from(data);
    not_ready_pending_piece_count_--;
    if (not_ready_pending_piece_count_ == 0) {
      TorrentHeader header;
      auto S = td::unserialize(header, header_str_.as_slice());
      if (S.is_ok()) {
        S = set_header(std::move(header));
      }
      if (S.is_error()) {
        S = S.move_as_error_prefix("Invalid torrent header: ");
        fatal_error_ = S.clone();
        return S;
      }
      if (enabled_wirte_to_files_) {
        add_pending_pieces();
      }
    }
  }

  return td::Status::OK();
}

void Torrent::enable_write_to_files() {
  if (enabled_wirte_to_files_) {
    return;
  }
  enabled_wirte_to_files_ = true;
  if (header_) {
    add_pending_pieces();
  }
}

void Torrent::add_pending_pieces() {
  for (auto &p : pending_pieces_) {
    td::Status S = add_validated_piece(p.first, std::move(p.second));
    if (S.is_error()) {
      LOG(WARNING) << "Failed to add pending piece #" << p.first << ": " << S;
    }
  }
  pending_pieces_.clear();
}

std::string Torrent::get_chunk_path(td::Slice name) const {
  return PSTRING() << root_dir_.value() << TD_DIR_SLASH << header_.value().dir_name << TD_DIR_SLASH << name;
}

std::string Torrent::get_file_path(size_t i) const {
  return get_chunk_path(chunks_.at(i + 1).name);
}

td::Status Torrent::init_chunk_data(ChunkState &chunk) {
  if (chunk.data) {
    return td::Status::OK();
  }
  if (root_dir_) {
    std::string path = get_chunk_path(chunk.name);
    TRY_STATUS(td::mkpath(path));
    TRY_RESULT(data, td::FileNoCacheBlobView::create(path, chunk.size, true));
    chunk.data = std::move(data);
  } else {
    chunk.data = td::BufferSliceBlobView::create(td::BufferSlice(chunk.size));
  }
  return td::Status::OK();
}

td::Status Torrent::add_validated_piece(td::uint64 piece_i, td::Slice data) {
  CHECK(!chunks_.empty());

  auto piece = info_.get_piece_info(piece_i);
  std::set<size_t> excluded;

  TRY_STATUS(iterate_piece(piece, [&](auto it, auto info) {
    if (it->excluded) {
      excluded.insert(it - chunks_.begin());
      return td::Status::OK();
    }
    TRY_STATUS(init_chunk_data(*it));
    TRY_STATUS(it->write_piece(data.substr(info.piece_offset, info.size), info.chunk_offset));
    return td::Status::OK();
  }));
  TRY_STATUS(iterate_piece(piece, [&](auto it, auto info) {
    if (!it->excluded) {
      it->ready_size += info.size;
      included_ready_size_ += info.size;
    }
    return td::Status::OK();
  }));
  piece_is_ready_[piece_i] = true;
  not_ready_piece_count_--;
  if (!excluded.empty()) {
    in_memory_pieces_[piece_i] = {data.str(), std::move(excluded)};
  }

  return td::Status::OK();
}

bool Torrent::is_completed() const {
  return inited_info_ && enabled_wirte_to_files_ && included_ready_size_ == included_size_;
}

td::Result<td::BufferSlice> Torrent::read_file(td::Slice name) {
  if (!inited_info_) {
    return td::Status::Error("Torrent info not inited");
  }
  for (auto &chunk : chunks_) {
    if (chunk.name == name) {
      td::BufferSlice res(chunk.size);
      TRY_STATUS(chunk.get_piece(res.as_slice(), 0));
      return std::move(res);
    }
  }
  return td::Status::Error("Unknown name");
}

Torrent::GetMetaOptions::GetMetaOptions() = default;
std::string Torrent::get_meta_str(const GetMetaOptions &options) const {
  CHECK(inited_info_);
  return get_meta(options).serialize();
}

TorrentMeta Torrent::get_meta(const GetMetaOptions &options) const {
  CHECK(inited_info_);
  TorrentMeta torrent_file;
  if (options.with_header) {
    torrent_file.header = header_;
  }
  torrent_file.info = info_;
  torrent_file.info.init_cell();
  if (options.with_proof) {
    torrent_file.root_proof = merkle_tree_.get_root(options.proof_depth_limit);
  }
  return torrent_file;
}

Torrent::Torrent(td::Bits256 hash) : hash_(hash), inited_info_(false) {
}

Torrent::Torrent(Info info, td::optional<TorrentHeader> header, ton::MerkleTree tree, std::vector<ChunkState> chunks,
                 std::string root_dir)
    : hash_(info.get_hash())
    , inited_info_(true)
    , info_(info)
    , root_dir_(std::move(root_dir))
    , header_(std::move(header))
    , enabled_wirte_to_files_(true)
    , merkle_tree_(std::move(tree))
    , piece_is_ready_(info_.pieces_count(), true)
    , ready_parts_count_{info_.pieces_count()}
    , chunks_(std::move(chunks))
    , included_size_(info_.file_size)
    , included_ready_size_(info_.file_size) {
}

td::Status Torrent::set_header(TorrentHeader header) {
  if (header_) {
    return td::Status::OK();
  }
  auto header_str = header.serialize();
  td::Bits256 header_hash;
  td::sha256(header_str.as_slice(), header_hash.as_slice());
  if (header_hash != info_.header_hash) {
    return td::Status::Error("Incorrect header hash");
  }
  TRY_STATUS_PREFIX(header.validate(info_.file_size, info_.header_size), "Invalid torrent header: ");
  auto add_chunk = [&](td::Slice name, td::uint64 offset, td::uint64 size) {
    ChunkState chunk;
    chunk.name = name.str();
    chunk.ready_size = 0;
    chunk.size = size;
    chunk.offset = offset;
    chunks_.push_back(std::move(chunk));
    included_size_ += size;
  };
  add_chunk("", 0, header_str.size());
  chunks_.back().data = td::BufferSliceBlobView::create(std::move(header_str));
  for (size_t i = 0; i < header.files_count; i++) {
    auto l = header.get_data_begin(i);
    auto r = header.get_data_end(i);
    add_chunk(header.get_name(i), l, r - l);
  }
  header_ = std::move(header);
  return td::Status::OK();
}

size_t Torrent::get_ready_parts_count() const {
  return ready_parts_count_;
}

std::vector<size_t> Torrent::chunks_by_piece(td::uint64 piece_id) {
  CHECK(inited_info_);
  std::vector<size_t> res;
  auto piece = info_.get_piece_info(piece_id);
  auto is_ok = iterate_piece(piece, [&](auto it, auto info) {
    res.push_back(it - chunks_.begin());
    return td::Status::OK();
  });
  return res;
}

td::Status Torrent::init_info(Info info) {
  if (hash_ != info.get_hash()) {
    return td::Status::Error("Hash mismatch");
  }
  if (inited_info_) {
    return td::Status::OK();
  }
  auto S = info.validate();
  if (S.is_error()) {
    S = S.move_as_error_prefix("Invalid torrent info: ");
    fatal_error_ = S.clone();
    return S;
  }
  inited_info_ = true;
  info_ = std::move(info);
  merkle_tree_ = MerkleTree(info_.pieces_count(), info_.root_hash);
  piece_is_ready_.resize(info_.pieces_count(), false);
  not_ready_piece_count_ = piece_is_ready_.size();
  header_pieces_count_ = (info_.header_size + info_.piece_size - 1) / info_.piece_size;
  not_ready_pending_piece_count_ = header_pieces_count_;
  header_str_ = td::BufferSlice(info_.header_size);
  return td::Status::OK();
}

void Torrent::set_file_excluded(size_t i, bool excluded) {
  CHECK(header_);
  CHECK(i + 1 < chunks_.size());
  if (!root_dir_) {
    return;  // All files are in-memory, nothing to do
  }
  size_t chunk_i = i + 1;
  auto &chunk = chunks_[chunk_i];
  if (chunk.excluded == excluded) {
    return;
  }
  if (excluded) {
    included_size_ -= chunk.size;
    included_ready_size_ -= chunk.ready_size;
  } else {
    included_size_ += chunk.size;
    included_ready_size_ += chunk.ready_size;
  }
  chunk.excluded = excluded;
  if (!enabled_wirte_to_files_ || excluded) {
    return;
  }
  auto range = get_file_parts_range(i);
  for (auto it = in_memory_pieces_.lower_bound(range.begin); it != in_memory_pieces_.end() && it->first < range.end;) {
    if (!it->second.pending_chunks.count(chunk_i)) {
      ++it;
      continue;
    }
    auto piece_i = it->first;
    auto piece = info_.get_piece_info(piece_i);
    auto S = [&]() {
      auto l = td::max(chunk.offset, piece.offset);
      auto r = td::min(chunk.offset + chunk.size, piece.offset + piece.size);
      TRY_STATUS(init_chunk_data(chunk));
      TRY_STATUS(chunk.write_piece(it->second.data.substr(l - piece.offset, r - l), l - chunk.offset));
      chunk.ready_size += r - l;
      included_ready_size_ += r - l;
      return td::Status::OK();
    }();
    if (S.is_error()) {
      // Erase piece completely
      piece_is_ready_[piece_i] = false;
      not_ready_piece_count_++;
      iterate_piece(piece, [&](auto it2, auto info) {
        if (!it2->excluded) {
          included_ready_size_ -= info.size;
        }
        if (!it2->excluded || !it->second.pending_chunks.count(it2 - chunks_.begin())) {
          it2->ready_size -= info.size;
        }
        return td::Status::OK();
      });
      it = in_memory_pieces_.erase(it);
      continue;
    }
    it->second.pending_chunks.erase(chunk_i);
    if (it->second.pending_chunks.empty()) {
      it = in_memory_pieces_.erase(it);
    } else {
      ++it;
    }
  }
}

void Torrent::load_from_files(std::string files_path) {
  CHECK(inited_header());
  std::vector<td::optional<td::BlobView>> new_blobs;
  new_blobs.push_back(td::BufferSliceBlobView::create(get_header().serialize()));
  size_t files_count = get_files_count().unwrap();
  for (size_t i = 0; i < files_count; ++i) {
    std::string new_path = PSTRING() << files_path << TD_DIR_SLASH << header_.value().dir_name << TD_DIR_SLASH
                                     << get_file_name(i);
    auto R = td::FileNoCacheBlobView::create(new_path, get_file_size(i), false);
    if (R.is_error() && files_count == 1) {
      R = td::FileNoCacheBlobView::create(files_path, get_file_size(i), false);
    }
    if (R.is_error()) {
      new_blobs.emplace_back();
    } else {
      new_blobs.push_back(R.move_as_ok());
    }
  }
  auto load_new_piece = [&](size_t piece_i) -> td::Result<std::string> {
    auto piece = info_.get_piece_info(piece_i);
    bool included = false;
    TRY_STATUS(iterate_piece(piece, [&](auto it, IterateInfo info) {
      if (!it->excluded) {
        included = true;
      }
      return td::Status::OK();
    }));
    if (!included) {
      return td::Status::Error("Piece is excluded");
    }
    std::string data(piece.size, '\0');
    TRY_STATUS(iterate_piece(piece, [&](auto it, IterateInfo info) {
      size_t chunk_i = it - chunks_.begin();
      if (!new_blobs[chunk_i]) {
        return td::Status::Error("No such file");
      }
      TRY_RESULT(s, new_blobs[chunk_i].value().view_copy(td::MutableSlice(data).substr(info.piece_offset, info.size),
                                                         info.chunk_offset));
      if (s != info.size) {
        return td::Status::Error("Can't read file");
      }
      return td::Status::OK();
    }));
    return data;
  };
  std::vector<std::pair<size_t, td::Bits256>> new_pieces;
  for (size_t i = 0; i < piece_is_ready_.size(); ++i) {
    if (piece_is_ready_[i]) {
      continue;
    }
    auto r_data = load_new_piece(i);
    if (r_data.is_error()) {
      continue;
    }
    td::Bits256 hash;
    td::sha256(r_data.ok(), hash.as_slice());
    new_pieces.emplace_back(i, hash);
  }
  size_t added_cnt = 0;
  for (size_t i : merkle_tree_.add_pieces(std::move(new_pieces))) {
    auto r_data = load_new_piece(i);
    if (r_data.is_error()) {
      continue;
    }
    if (add_piece(i, r_data.ok(), {}).is_ok()) {
      ++added_cnt;
    }
  }
  if (added_cnt > 0) {
    LOG(INFO) << "Loaded " << added_cnt << " new pieces for " << get_hash().to_hex();
  }
}

td::Status Torrent::copy_to(const std::string &new_root_dir) {
  if (!is_completed() || included_size_ != info_.file_size) {
    return td::Status::Error("Torrent::copy_to is allowed only for fully completed torrents");
  }
  auto get_new_chunk_path = [&](td::Slice name) -> std::string {
    return PSTRING() << new_root_dir << TD_DIR_SLASH << header_.value().dir_name << TD_DIR_SLASH << name;
  };
  std::vector<td::BlobView> new_blobs;
  for (size_t i = 1; i < chunks_.size(); ++i) {
    auto &chunk = chunks_[i];
    std::string new_path = get_new_chunk_path(chunk.name);
    TRY_STATUS(td::mkpath(new_path));
    TRY_RESULT(new_blob, td::FileNoCacheBlobView::create(new_path, chunk.size, true));
    static const td::uint64 BUF_SIZE = 1 << 17;
    td::BufferSlice buf(BUF_SIZE);
    for (td::uint64 l = 0; l < chunk.size; l += BUF_SIZE) {
      td::uint64 r = std::min(chunk.size, l + BUF_SIZE);
      TRY_RESULT_PREFIX(s, chunk.data.view(buf.as_slice().substr(0, r - l), l),
                        PSTRING() << "Failed to read " << chunk.name << ": ");
      if (s.size() != r - l) {
        return td::Status::Error(PSTRING() << "Failed to read " << chunk.name);
      }
      TRY_STATUS_PREFIX(new_blob.write(s, l), PSTRING() << "Failed to write " << chunk.name << ": ");
    }
    new_blobs.push_back(std::move(new_blob));
  }
  root_dir_ = new_root_dir;
  for (size_t i = 1; i < chunks_.size(); ++i) {
    chunks_[i].data = std::move(new_blobs[i - 1]);
  }
  return td::Status::OK();
}

}  // namespace ton
