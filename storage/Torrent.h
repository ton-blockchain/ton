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
  //TODO: add multiple chunks? Merkle tree supports much more general interface

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

  struct PartsRange {
    td::uint64 begin{0};
    td::uint64 end{0};
  };
  PartsRange get_file_parts_range(size_t i);
  PartsRange get_header_parts_range();

  size_t get_ready_parts_count() const;

  std::vector<size_t> chunks_by_piece(td::uint64 piece_id);

 private:
  Info info_;
  td::optional<std::string> root_dir_;

  // While header is not completly available all pieces are stored in memory
  td::BufferSlice header_str_;
  td::optional<TorrentHeader> header_;
  size_t not_ready_pending_piece_count_{0};
  size_t header_pieces_count_{0};
  std::map<td::uint64, td::string> pending_pieces_;

  ton::MerkleTree merkle_tree_;

  std::vector<bool> piece_is_ready_;
  size_t not_ready_piece_count_{0};
  size_t ready_parts_count_{0};

  struct ChunkState {
    std::string name;
    td::uint64 offset{0};
    td::uint64 size{0};
    td::uint64 ready_size{0};
    td::BlobView data;

    struct Cache {
      td::uint64 offset{0};
      td::uint64 size{0};
      td::BufferSlice slice;
    };

    bool is_ready() const {
      return ready_size == size;
    }

    TD_WARN_UNUSED_RESULT td::Status add_piece(td::Slice piece, td::uint64 offset) {
      TRY_RESULT(written, data.write(piece, offset));
      CHECK(written == piece.size());
      ready_size += written;
      return td::Status::OK();
    }
    bool has_piece(td::uint64 offset, td::uint64 size) {
      return data.size() >= offset + size;
    }
    TD_WARN_UNUSED_RESULT td::Status get_piece(td::MutableSlice dest, td::uint64 offset, Cache *cache = nullptr);
  };
  std::vector<ChunkState> chunks_;

  explicit Torrent(Info info, td::optional<TorrentHeader> header, ton::MerkleTree tree, std::vector<ChunkState> chunk);
  explicit Torrent(TorrentMeta meta);
  void set_root_dir(std::string root_dir) {
    root_dir_ = std::move(root_dir);
  }

  std::string get_chunk_path(td::Slice name);
  td::Status init_chunk_data(ChunkState &chunk);
  template <class F>
  td::Status iterate_piece(Info::PieceInfo piece, F &&f);

  td::Status add_header_piece(td::uint64 piece_i, td::Slice data);
  td::Status add_validated_piece(td::uint64 piece_i, td::Slice data);
  void set_header(const TorrentHeader &header);
};

}  // namespace ton
