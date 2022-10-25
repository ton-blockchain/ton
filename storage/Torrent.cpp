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

Torrent::Torrent(TorrentMeta meta)
    : hash_(meta.info.get_hash().bits())
    , inited_info_(true)
    , info_(meta.info)
    , merkle_tree_(info_.pieces_count(), info_.root_hash)
    , piece_is_ready_(info_.pieces_count(), false) {
  not_ready_piece_count_ = piece_is_ready_.size();
  header_pieces_count_ = (info_.header_size + info_.piece_size - 1) / info_.piece_size;
  not_ready_pending_piece_count_ = header_pieces_count_;

  if (meta.header) {
    set_header(meta.header.unwrap());
  } else {
    header_str_ = td::BufferSlice(info_.header_size);
  }
  if (meta.root_proof.not_null()) {
    merkle_tree_.add_proof(meta.root_proof);
  }
}

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
  Torrent res(std::move(meta));
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
  if (!inited_info_ && !header_) {
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
    return it2->second.second;
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
  if (!inited_info_) {
    return td::Status::Error("Torrent info not inited");
  }
  TRY_STATUS(merkle_tree_.add_proof(proof));
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
      TRY_STATUS(td::unserialize(header, header_str_.as_slice()));  // TODO: it is a fatal error
      set_header(header);
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
  for (auto it = pending_pieces_.begin(); it != pending_pieces_.end();) {
    td::Status S = add_validated_piece(it->first, it->second);
    if (S.is_ok()) {
      it = pending_pieces_.erase(it);
    } else {
      // TODO: Process errors here
      LOG(FATAL) << "Failed to add piece: " << S;
      ++it;
    }
  }
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
  size_t excluded_cnt = 0;

  TRY_STATUS(iterate_piece(piece, [&](auto it, auto info) {
    if (it->excluded) {
      ++excluded_cnt;
      return td::Status::OK();
    }
    TRY_STATUS(init_chunk_data(*it));
    TRY_STATUS(it->add_piece(data.substr(info.piece_offset, info.size), info.chunk_offset));
    included_ready_size_ += info.size;
    return td::Status::OK();
  }));
  piece_is_ready_[piece_i] = true;
  not_ready_piece_count_--;
  if (excluded_cnt > 0) {
    in_memory_pieces_[piece_i] = {excluded_cnt, data.str()};
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

Torrent::Torrent(Info info, td::optional<TorrentHeader> header, ton::MerkleTree tree, std::vector<ChunkState> chunks)
    : hash_(info.get_hash().bits())
    , inited_info_(true)
    , info_(info)
    , header_(std::move(header))
    , merkle_tree_(std::move(tree))
    , piece_is_ready_(info_.pieces_count(), true)
    , ready_parts_count_{info_.pieces_count()}
    , chunks_(std::move(chunks)) {
}

void Torrent::set_header(const TorrentHeader &header) {
  header_ = header;
  auto add_chunk = [&](td::Slice name, td::uint64 offset, td::uint64 size) {
    ChunkState chunk;
    chunk.name = name.str();
    chunk.ready_size = 0;
    chunk.size = size;
    chunk.offset = offset;
    chunks_.push_back(std::move(chunk));
    included_size_ += size;
  };
  add_chunk("", 0, header.serialization_size());
  chunks_.back().data = td::BufferSliceBlobView::create(header.serialize());
  for (size_t i = 0; i < header.files_count; i++) {
    auto l = header.get_data_begin(i);
    auto r = header.get_data_end(i);
    add_chunk(header.get_name(i), l, r - l);
  }
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
  if (hash_ != info.get_hash().bits()) {
    return td::Status::Error("Hash mismatch");
  }
  if (inited_info_) {
    return td::Status::OK();
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
  auto &chunk = chunks_[i + 1];
  if (chunk.excluded == excluded) {
    return;
  }
  included_size_ += (excluded ? -chunk.size : chunk.size);
  included_ready_size_ += (excluded ? -chunk.ready_size : chunk.ready_size);
  chunk.excluded = excluded;
  if (!enabled_wirte_to_files_ || excluded) {
    return;
  }
  auto range = get_file_parts_range(i);
  for (auto it = in_memory_pieces_.lower_bound(range.begin); it != in_memory_pieces_.end() && it->first < range.end;) {
    auto piece_i = it->first;
    auto piece = info_.get_piece_info(piece_i);
    auto l = td::max(chunk.offset, piece.offset);
    auto r = td::min(chunk.offset + chunk.size, piece.offset + piece.size);
    init_chunk_data(chunk).ensure();  // TODO: Process errors
    chunk.add_piece(it->second.second.substr(l - piece.offset, r - l), l - chunk.offset).ensure();
    included_ready_size_ += r - l;
    if (--it->second.first == 0) {
      it = in_memory_pieces_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace ton
