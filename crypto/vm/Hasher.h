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

class HasherImpl {
 public:
  virtual ~HasherImpl() = default;
  virtual void append(const unsigned char* data, size_t size) = 0;
  virtual td::BufferSlice finish() = 0;
  virtual std::unique_ptr<HasherImpl> make_copy() const = 0;
};

class Hasher : public td::CntObject {
 public:
  explicit Hasher(unsigned id);
  Hasher(const Hasher&) = delete;
  td::CntObject* make_copy() const override;
  void append(td::ConstBitPtr data, unsigned size);
  td::BufferSlice finish();

  unsigned get_hash_id() const {
    return id_;
  }

  static Ref<Hasher> create(unsigned hash_id) {
    return td::make_ref<Hasher>(hash_id);
  }

  static const unsigned SHA256 = 0;
  static const unsigned SHA512 = 1;
  static const unsigned BLAKE2B = 2;
  static const unsigned KECCAK256 = 3;
  static const unsigned KECCAK512 = 4;

 private:
  unsigned id_ = 0;
  unsigned char extra_bits_ = 0;
  unsigned extra_bits_cnt_ = 0;
  std::unique_ptr<HasherImpl> impl_;

  Hasher(unsigned id, std::unique_ptr<HasherImpl> impl);
};

}