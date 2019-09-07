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

    Copyright 2017-2019 Telegram Systems LLP
*/
#pragma once

#include "validator/interfaces/proof.h"
#include "adnl/utils.hpp"

namespace ton {

namespace validator {

namespace dummy0 {

class ProofImpl : public ton::validator::Proof {
 private:
  BlockIdExt masterchain_block_id_;
  td::BufferSlice data_;
  FileHash file_hash_;

 public:
  td::BufferSlice data() const override {
    return data_.clone();
  }
  FileHash file_hash() const override {
    return file_hash_;
  }
  BlockIdExt masterchain_block_id() const override {
    return masterchain_block_id_;
  }

  ProofImpl *make_copy() const override {
    return new ProofImpl(masterchain_block_id_, data_.clone(), file_hash_);
  }
  ProofImpl(BlockIdExt masterchain_block_id, td::BufferSlice data, FileHash file_hash)
      : masterchain_block_id_(masterchain_block_id), data_(std::move(data)), file_hash_(file_hash) {
  }
  ProofImpl(BlockIdExt masterchain_block_id, td::BufferSlice data)
      : masterchain_block_id_(masterchain_block_id), data_(std::move(data)) {
    file_hash_ = UInt256_2_Bits256(sha256_uint256(data_.as_slice()));
  }
};

class ProofLinkImpl : public ton::validator::ProofLink {
 private:
  td::BufferSlice data_;
  FileHash file_hash_;

 public:
  td::BufferSlice data() const override {
    return data_.clone();
  }
  FileHash file_hash() const override {
    return file_hash_;
  }
  ProofLinkImpl *make_copy() const override {
    return new ProofLinkImpl(data_.clone(), file_hash_);
  }
  ProofLinkImpl(td::BufferSlice data, FileHash file_hash) : data_(std::move(data)), file_hash_(file_hash) {
  }
  ProofLinkImpl(td::BufferSlice data) : data_(std::move(data)) {
    file_hash_ = UInt256_2_Bits256(sha256_uint256(data_.as_slice()));
  }
};

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
