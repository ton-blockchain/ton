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

#include "TorrentCreator.h"

#include "td/db/utils/CyclicBuffer.h"

#include "td/utils/crypto.h"
#include "td/utils/PathView.h"
#include "td/utils/port/path.h"
#include "td/utils/tl_helpers.h"
#include "MicrochunkTree.h"
#include "TorrentHeader.hpp"

namespace ton {
td::Result<Torrent> Torrent::Creator::create_from_path(Options options, td::CSlice raw_path) {
  TRY_RESULT(path, td::realpath(raw_path));
  TRY_RESULT(stat, td::stat(path));
  std::string root_dir = path;
  while (!root_dir.empty() && root_dir.back() == TD_DIR_SLASH) {
    root_dir.pop_back();
  }
  while (!root_dir.empty() && root_dir.back() != TD_DIR_SLASH) {
    root_dir.pop_back();
  }
  if (stat.is_dir_) {
    if (!path.empty() && path.back() != TD_DIR_SLASH) {
      path += TD_DIR_SLASH;
    }
    if (!options.dir_name) {
      options.dir_name = td::PathView::dir_and_file(path).str();
    }
    Torrent::Creator creator(options);
    td::Status status;
    auto walk_status = td::WalkPath::run(path, [&](td::CSlice name, td::WalkPath::Type type) {
      if (type == td::WalkPath::Type::NotDir) {
        status = creator.add_file(td::PathView::relative(name, path), name);
        if (status.is_error()) {
          return td::WalkPath::Action::Abort;
        }
      }
      return td::WalkPath::Action::Continue;
    });
    TRY_STATUS(std::move(status));
    TRY_STATUS(std::move(walk_status));
    creator.root_dir_ = std::move(root_dir);
    std::sort(creator.files_.begin(), creator.files_.end(),
              [](const Torrent::Creator::File& a, const Torrent::Creator::File& b) { return a.name < b.name; });
    return creator.finalize();
  } else {
    Torrent::Creator creator(options);
    TRY_STATUS(creator.add_file(td::PathView(path).file_name(), path));
    creator.root_dir_ = std::move(root_dir);
    return creator.finalize();
  }
}

td::Result<Torrent> Torrent::Creator::create_from_blobs(Options options, td::Span<Blob> blobs) {
  Torrent::Creator creator(options);
  for (auto& blob : blobs) {
    TRY_STATUS(creator.add_blob(blob.name, blob.data));
  }
  return creator.finalize();
}

td::Status Torrent::Creator::add_blob(td::Slice name, td::Slice blob) {
  return add_blob(name, td::BufferSliceBlobView::create(td::BufferSlice(blob)));
}

td::Status Torrent::Creator::add_blob(td::Slice name, td::BlobView blob) {
  File file;
  file.name = name.str();
  file.data = std::move(blob);
  files_.push_back(std::move(file));
  return td::Status::OK();
}

TD_WARN_UNUSED_RESULT td::Status Torrent::Creator::add_file(td::Slice name, td::CSlice path) {
  LOG(DEBUG) << "Add file " << name << " " << path;
  TRY_RESULT(data, td::FileNoCacheBlobView::create(path));
  return add_blob(name, std::move(data));
}

td::Result<Torrent> Torrent::Creator::finalize() {
  if (files_.empty()) {
    return td::Status::Error("No files");
  }
  TorrentHeader header;
  TRY_RESULT(files_count, td::narrow_cast_safe<td::uint32>(files_.size()));
  header.files_count = files_count;
  header.data_index.resize(files_count);
  header.name_index.resize(files_count);
  td::uint64 data_offset = 0;
  for (size_t i = 0; i < files_.size(); i++) {
    header.names += files_[i].name;
    header.name_index[i] = header.names.size();
    data_offset += files_[i].data.size();
    header.data_index[i] = data_offset;
  }
  header.tot_names_size = header.names.size();
  if (options_.dir_name) {
    header.dir_name = options_.dir_name.value();
  }

  // Now we should stream all data to calculate sha256 of all pieces

  std::string buffer;
  td::CyclicBuffer::Options cb_options;
  cb_options.chunk_size = td::max(options_.piece_size * 16, (1 << 20) / options_.piece_size * options_.piece_size);
  cb_options.count = 2;

  auto reader_writer = td::CyclicBuffer::create(cb_options);
  auto reader = std::move(reader_writer.first);
  auto writer = std::move(reader_writer.second);

  auto header_size = header.serialization_size();
  auto file_size = header_size + data_offset;
  auto pieces_count = (file_size + options_.piece_size - 1) / options_.piece_size;
  std::vector<Torrent::ChunkState> chunks;
  std::vector<td::Bits256> pieces;
  auto flush_reader = [&](bool force) {
    while (true) {
      auto slice = reader.prepare_read();
      slice.truncate(options_.piece_size);
      if (slice.empty() || (slice.size() != options_.piece_size && !force)) {
        break;
      }
      td::Bits256 hash;
      sha256(slice, hash.as_slice());
      pieces.push_back(hash);
      reader.confirm_read(slice.size());
    }
  };
  td::uint64 offset = 0;
  auto add_blob = [&](auto data, td::Slice name) {
    td::uint64 data_offset = 0;
    while (data_offset < data.size()) {
      auto dest = writer.prepare_write();
      CHECK(dest.size() != 0);
      dest.truncate(data.size() - data_offset);
      TRY_RESULT(got_size, data.view_copy(dest, data_offset));
      CHECK(got_size != 0);
      data_offset += got_size;
      writer.confirm_write(got_size);
      flush_reader(false);
    }

    Torrent::ChunkState chunk;
    chunk.name = name.str();
    chunk.offset = offset;
    chunk.size = data.size();
    chunk.ready_size = chunk.size;
    chunk.data = std::move(data);

    offset += chunk.size;
    chunks.push_back(std::move(chunk));
    return td::Status::OK();
  };

  Torrent::Info info;
  auto header_str = td::serialize(header);
  CHECK(header_size == header_str.size());
  info.header_size = header_str.size();
  td::sha256(header_str, info.header_hash.as_slice());

  add_blob(td::BufferSliceBlobView::create(td::BufferSlice(header_str)), "").ensure();
  for (auto& file : files_) {
    add_blob(std::move(file.data), file.name).ensure();
  }
  flush_reader(true);
  CHECK(pieces.size() == pieces_count);
  CHECK(offset == file_size);
  MerkleTree tree(std::move(pieces));

  info.header_size = header.serialization_size();
  info.piece_size = options_.piece_size;
  info.description = options_.description;
  info.file_size = file_size;
  info.root_hash = tree.get_root_hash();

  info.init_cell();
  TRY_STATUS_PREFIX(info.validate(), "Invalid torrent info: ");
  TRY_STATUS_PREFIX(header.validate(info.file_size, info.header_size), "Invalid torrent header: ");

  Torrent torrent(info, std::move(header), std::move(tree), std::move(chunks), root_dir_);

  return std::move(torrent);
}
}  // namespace ton
