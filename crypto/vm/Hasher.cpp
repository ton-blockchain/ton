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
#include "vm/Hasher.h"
#include "vm/excno.hpp"
#include "vm/vm.h"
#include <iostream>
#include <openssl/evp.h>
#include "keccak/keccak.h"

namespace vm {

using td::Ref;

class HasherImplEVP : public Hasher::HasherImpl {
 public:
  explicit HasherImplEVP(EVP_MD_CTX* ctx) : ctx_(ctx) {
  }

  ~HasherImplEVP() override {
    EVP_MD_CTX_free(ctx_);
  }

  void append(const unsigned char *data, size_t size) override {
    CHECK(EVP_DigestUpdate(ctx_, data, size));
  }

  td::BufferSlice finish() override {
    td::BufferSlice hash(EVP_MD_CTX_size(ctx_));
    unsigned size;
    CHECK(EVP_DigestFinal_ex(ctx_, (unsigned char *)hash.data(), &size) || size != hash.size());
    return hash;
  }

  std::unique_ptr<HasherImpl> make_copy() const override {
    EVP_MD_CTX *new_ctx = nullptr;
    new_ctx = EVP_MD_CTX_new();
    CHECK(new_ctx != nullptr);
    CHECK(EVP_MD_CTX_copy_ex(new_ctx, ctx_));
    return std::make_unique<HasherImplEVP>(new_ctx);
  }

 private:
  EVP_MD_CTX *ctx_;
};

class HasherImplKeccak : public Hasher::HasherImpl {
 public:
  explicit HasherImplKeccak(size_t hash_size) : hash_size_(hash_size) {
    CHECK(keccak_init(&state_, hash_size * 2, 24) == 0);
    CHECK(state_ != nullptr);
  }

  ~HasherImplKeccak() override {
    CHECK(keccak_destroy(state_) == 0);
  }

  void append(const unsigned char *data, size_t size) override {
    CHECK(keccak_absorb(state_, data, size) == 0);
  }

  td::BufferSlice finish() override {
    td::BufferSlice hash(hash_size_);
    CHECK(keccak_digest(state_, (unsigned char*)hash.data(), hash_size_, 1) == 0);
    return hash;
  }

  std::unique_ptr<HasherImpl> make_copy() const override {
    auto copy = std::make_unique<HasherImplKeccak>(hash_size_);
    CHECK(keccak_copy(state_, copy->state_) == 0);
    return copy;
  }

 private:
  size_t hash_size_;
  keccak_state *state_ = nullptr;
};

Hasher::Hasher(int hash_id) : id_(hash_id) {
  if (hash_id == KECCAK256 || hash_id == KECCAK512) {
    impl_ = std::make_unique<HasherImplKeccak>(hash_id == KECCAK256 ? 32 : 64);
    return;
  }

  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  CHECK(ctx != nullptr);
  const EVP_MD *evp;
  switch (hash_id) {
    case SHA256: evp = EVP_sha256(); break;
    case SHA512: evp = EVP_sha512(); break;
    case BLAKE2B: evp = EVP_blake2b512(); break;
    default:
      throw VmError{Excno::range_chk, "invalid hash id"};
  }
  CHECK(evp != nullptr && EVP_DigestInit_ex(ctx, evp, nullptr));
  impl_ = std::make_unique<HasherImplEVP>(ctx);
}

void Hasher::append(td::ConstBitPtr data, unsigned size) {
  if (!impl_) {
    throw VmError{Excno::unknown, "can't use finished hasher"};
  }
  while (size > 0) {
    unsigned cur_size = std::min(size, BUF_SIZE * 8 - buf_ptr_);
    td::BitPtr{buf_, (int)buf_ptr_}.copy_from(data, cur_size);
    buf_ptr_ += cur_size;
    if (buf_ptr_ == BUF_SIZE * 8) {
      impl_->append(buf_, BUF_SIZE);
      buf_ptr_ = 0;
    }
    size -= cur_size;
    data += cur_size;
  }
}

td::BufferSlice Hasher::finish() {
  if (!impl_) {
    throw VmError{Excno::unknown, "can't use finished hasher"};
  }
  if (buf_ptr_ % 8 != 0) {
    throw VmError{Excno::cell_und, "data does not consist of an integer number of bytes"};
  }
  impl_->append(buf_, buf_ptr_ / 8);
  td::BufferSlice hash = impl_->finish();
  impl_ = nullptr;
  return hash;
}

static const size_t BYTES_PER_GAS_UNIT[5] = {33, 16, 19, 11, 6};

size_t Hasher::bytes_per_gas_unit() const {
  return BYTES_PER_GAS_UNIT[id_];
}

}
