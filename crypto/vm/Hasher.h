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
#include "common/refcnt.hpp"
#include "td/utils/buffer.h"
#include "common/bitstring.h"
#include "vm/cells/Cell.h"
#include <memory>

namespace vm {

using td::Ref;

class Hasher {
 public:
  explicit Hasher(int id);
  Hasher(const Hasher&) = delete;
  void append(td::ConstBitPtr data, unsigned size);
  td::BufferSlice finish();
  size_t bytes_per_gas_unit() const;

  static const int SHA256 = 0;
  static const int SHA512 = 1;
  static const int BLAKE2B = 2;
  static const int KECCAK256 = 3;
  static const int KECCAK512 = 4;

  class HasherImpl {
   public:
    virtual ~HasherImpl() = default;
    virtual void append(const unsigned char* data, size_t size) = 0;
    virtual td::BufferSlice finish() = 0;
    virtual std::unique_ptr<HasherImpl> make_copy() const = 0;
  };

 private:
  int id_ = 0;
  static const unsigned BUF_SIZE = 256;
  unsigned char buf_[BUF_SIZE];
  unsigned buf_ptr_ = 0;
  std::unique_ptr<HasherImpl> impl_;
};

}