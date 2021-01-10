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
#include "td/utils/buffer.h"
#include "td/utils/Status.h"

namespace td {
class BlobViewImpl;

class BlobView {
 public:
  BlobView();
  explicit BlobView(std::unique_ptr<BlobViewImpl> impl);
  BlobView(BlobView &&);
  BlobView &operator=(BlobView &&);
  ~BlobView();
  td::Result<td::BufferSlice> to_buffer_slice();
  td::Result<td::Slice> view(td::MutableSlice slice, td::uint64 offset);
  td::Result<size_t> view_copy(td::MutableSlice slice, td::uint64 offset);
  td::Result<size_t> write(td::Slice data, td::uint64 offset);
  td::uint64 size();

  explicit operator bool() const {
    return bool(impl_);
  }

 private:
  std::unique_ptr<BlobViewImpl> impl_;
};

class BufferSliceBlobView {
 public:
  static BlobView create(td::BufferSlice slice);
};
class FileBlobView {
 public:
  static td::Result<BlobView> create(td::CSlice file_path, td::uint64 file_size = 0);
};
class FileNoCacheBlobView {
 public:
  static td::Result<BlobView> create(td::CSlice file_path, td::uint64 file_size = 0, bool may_write = false);
};
class FileMemoryMappingBlobView {
 public:
  static td::Result<BlobView> create(td::CSlice file_path, td::uint64 file_size = 0);
};

// For testing purposes
struct CycicBlobView {
  static td::Result<td::BlobView> create(td::BufferSlice data, td::uint64 total_size);
};
}  // namespace td
