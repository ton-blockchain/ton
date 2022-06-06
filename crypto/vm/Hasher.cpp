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

class HasherImplEVP : public HasherImpl {
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

class HasherImplKeccak : public HasherImpl {
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

Hasher::Hasher(unsigned hash_id) : id_(hash_id) {
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

Hasher::Hasher(unsigned id, std::unique_ptr<HasherImpl> impl) : id_(id), impl_(std::move(impl)) {
}

void Hasher::append(td::ConstBitPtr data, unsigned size) {
  if (!impl_) {
    throw VmError{Excno::unknown, "can't use finished hasher"};
  }
  if (size == 0) {
    return;
  }
  if ((data - extra_bits_cnt_).byte_aligned()) {
    if (extra_bits_cnt_) {
      unsigned s = 8 - extra_bits_cnt_;
      CHECK(s <= size);
      td::BitPtr(&extra_bits_, extra_bits_cnt_).copy_from(data, s);
      impl_->append(&extra_bits_, 1);
      data += s;
      size -= s;
    }
    impl_->append(data.get_byte_ptr(), size / 8);
    extra_bits_cnt_ = size % 8;
    if (extra_bits_cnt_) {
      extra_bits_ = data.get_byte_ptr()[size / 8];
    }
    return;
  }

  unsigned char buf[256];
  buf[0] = extra_bits_;
  unsigned buf_cap = 256 * 8, buf_size = extra_bits_cnt_;
  td::BitPtr buf_ptr(buf, buf_size);
  while (true) {
    unsigned s = std::min(size, buf_cap - buf_size);
    buf_ptr.copy_from(data, s);
    buf_ptr += s;
    data += s;
    buf_size += s;
    size -= s;
    if (buf_size >= 8) {
      impl_->append(buf, buf_size / 8);
    }
    if (size == 0) {
      extra_bits_cnt_ = buf_size % 8;
      if (extra_bits_cnt_) {
        extra_bits_ = buf[buf_size / 8];
      }
      break;
    }
    buf_size = 0;
    buf_ptr = td::BitPtr(buf);
  }
}

td::BufferSlice Hasher::finish() {
  if (!impl_) {
    throw VmError{Excno::unknown, "can't use finished hasher"};
  }
  if (extra_bits_cnt_ != 0) {
    throw VmError{Excno::cell_und, "data does not consist of an integer number of bytes"};
  }
  td::BufferSlice hash = impl_->finish();
  impl_ = nullptr;
  return hash;
}

td::CntObject* Hasher::make_copy() const {
  auto copy = new Hasher(id_, impl_ ? impl_->make_copy() : nullptr);
  copy->extra_bits_ = extra_bits_;
  copy->extra_bits_cnt_ = extra_bits_cnt_;
  return copy;
}

}
