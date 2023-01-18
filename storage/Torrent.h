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

#pragma once
#include "MerkleTree.h"
#include "TorrentMeta.h"

#include "td/utils/buffer.h"
#include "td/db/utils/BlobView.h"

#include <map>
#include <set>

namespace ton {
class Torrent {
 public:
  class Creator;
  friend class Creator;
  using Info = TorrentInfo;

  struct Options {
    std::string root_dir;
    bool in_memory{false};
    bool validate{false};
  };

  // creation
  static td::Result<Torrent> open(Options options, td::Bits256 hash);
  static td::Result<Torrent> open(Options options, TorrentMeta meta);
  static td::Result<Torrent> open(Options options, td::Slice meta_str);
  void validate();

  std::string get_stats_str() const;

  const Info &get_info() const;

  // get piece and proof
  td::Result<std::string> get_piece_data(td::uint64 piece_i);
  td::Result<td::Ref<vm::Cell>> get_piece_proof(td::uint64 piece_i);

  // add piece (with an optional proof)
  td::Status add_piece(td::uint64 piece_i, td::Slice data, td::Ref<vm::Cell> proof);
  //TODO: add multiple pieces? Merkle tree supports much more general interface
  td::Status add_proof(td::Ref<vm::Cell> proof);

  bool is_completed() const;

  // Checks that file is ready and returns its content.
  // Intened mostly for in-memory usage and for tests
  td::Result<td::BufferSlice> read_file(td::Slice name);

  struct GetMetaOptions {
    GetMetaOptions();
    size_t proof_depth_limit{std::numeric_limits<size_t>::max()};
    bool with_header{true};
    bool with_proof{true};

    GetMetaOptions &without_header() {
      with_header = false;
      return *this;
    }
    GetMetaOptions &without_proof() {
      with_proof = false;
      return *this;
    }
    GetMetaOptions &with_proof_depth_limit(size_t limit) {
      proof_depth_limit = limit;
      return *this;
    }
  };
  std::string get_meta_str(const GetMetaOptions &options = {}) const;
  TorrentMeta get_meta(const GetMetaOptions &options = {}) const;

  // Some api for inspection of a current state
  bool is_piece_ready(td::uint64 piece_i) const;

  td::optional<size_t> get_files_count() const;
  td::CSlice get_file_name(size_t i) const;
  td::uint64 get_file_size(size_t i) const;
  td::uint64 get_file_ready_size(size_t i) const;
  std::string get_file_path(size_t i) const;

  struct PartsRange {
    td::uint64 begin{0};
    td::uint64 end{0};
    bool contains(td::uint64 i) const {
      return begin <= i && i < end;
    }
  };
  PartsRange get_file_parts_range(size_t i);
  PartsRange get_header_parts_range() const;

  size_t get_ready_parts_count() const;

  std::vector<size_t> chunks_by_piece(td::uint64 piece_id);

  bool inited_info() const {
    return inited_info_;
  }
  bool inited_header() const {
    return (bool)header_;
  }
  td::Bits256 get_hash() const {
    return hash_;
  }
  std::string get_root_dir() const {
    return root_dir_ ? root_dir_.value() : "";
  }
  td::Status init_info(Info info);
  td::Status set_header(TorrentHeader header);

  void enable_write_to_files();
  void set_file_excluded(size_t i, bool excluded);
  bool file_is_excluded(size_t i) const {
    return chunks_.at(i).excluded;
  }
  td::uint64 get_included_size() const {
    return header_ ? included_size_ : info_.file_size;
  }
  td::uint64 get_included_ready_size() const {
    return included_ready_size_;
  }

  bool is_piece_in_memory(td::uint64 i) const {
    return in_memory_pieces_.count(i);
  }
  std::set<td::uint64> get_pieces_in_memory() const {
    std::set<td::uint64> pieces;
    for (const auto &p : in_memory_pieces_) {
      pieces.insert(p.first);
    }
    return pieces;
  }

  const td::Status &get_fatal_error() const {
    return fatal_error_;
  }

  const TorrentHeader &get_header() const {
    CHECK(inited_header())
    return header_.value();
  }

  void load_from_files(std::string files_path);

  td::Status copy_to(const std::string& new_root_dir);

 private:
  td::Bits256 hash_;
  bool inited_info_ = false;
  Info info_;
  td::optional<std::string> root_dir_;

  // While header is not completly available all pieces are stored in memory
  td::BufferSlice header_str_;
  td::optional<TorrentHeader> header_;
  size_t not_ready_pending_piece_count_{0};
  size_t header_pieces_count_{0};
  std::map<td::uint64, td::string> pending_pieces_;
  bool enabled_wirte_to_files_ = false;
  struct InMemoryPiece {
    std::string data;
    std::set<size_t> pending_chunks;
  };
  std::map<td::uint64, InMemoryPiece> in_memory_pieces_;  // Pieces that overlap excluded files

  ton::MerkleTree merkle_tree_;

  std::vector<bool> piece_is_ready_;
  size_t not_ready_piece_count_{0};
  size_t ready_parts_count_{0};

  td::Status fatal_error_ = td::Status::OK();

  struct ChunkState {
    std::string name;
    td::uint64 offset{0};
    td::uint64 size{0};
    td::uint64 ready_size{0};
    td::BlobView data;
    bool excluded{false};

    struct Cache {
      td::uint64 offset{0};
      td::uint64 size{0};
      td::BufferSlice slice;
    };

    bool is_ready() const {
      return ready_size == size;
    }

    TD_WARN_UNUSED_RESULT td::Status write_piece(td::Slice piece, td::uint64 offset) {
      TRY_RESULT(written, data.write(piece, offset));
      if (written != piece.size()) {
        return td::Status::Error("Written less than expected");
      }
      return td::Status::OK();
    }
    bool has_piece(td::uint64 offset, td::uint64 size) {
      return data.size() >= offset + size;
    }
    TD_WARN_UNUSED_RESULT td::Status get_piece(td::MutableSlice dest, td::uint64 offset, Cache *cache = nullptr);
  };
  std::vector<ChunkState> chunks_;
  td::uint64 included_size_{0};
  td::uint64 included_ready_size_{0};

  explicit Torrent(td::Bits256 hash);
  explicit Torrent(Info info, td::optional<TorrentHeader> header, ton::MerkleTree tree, std::vector<ChunkState> chunk,
                   std::string root_dir);
  void set_root_dir(std::string root_dir) {
    root_dir_ = std::move(root_dir);
  }

  std::string get_chunk_path(td::Slice name) const;
  td::Status init_chunk_data(ChunkState &chunk);
  template <class F>
  td::Status iterate_piece(Info::PieceInfo piece, F &&f);
  void add_pending_pieces();

  td::Status add_pending_piece(td::uint64 piece_i, td::Slice data);
  td::Status add_validated_piece(td::uint64 piece_i, td::Slice data);
};

}  // namespace ton
