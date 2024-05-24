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

#include "catchain.h"

namespace ton {

namespace catchain {

class CatChainBlockImpl : public CatChainBlock {
 private:
  std::unique_ptr<CatChainBlock::Extra> extra_;
  td::uint32 src_;
  td::uint32 fork_;
  PublicKeyHash src_hash_;
  CatChainBlockHeight height_;
  CatChainBlockHash hash_;
  td::SharedSlice payload_;
  CatChainBlock *prev_;
  std::vector<CatChainBlock *> deps_;
  std::vector<CatChainBlockHeight> vt_;
  bool preprocess_sent_ = false;
  bool is_processed_ = false;

 public:
  td::SharedSlice &payload() override {
    return payload_;
  }
  const td::SharedSlice &payload() const override {
    return payload_;
  }

  CatChainBlock::Extra *extra() const override {
    return extra_.get();
  }
  std::unique_ptr<Extra> move_extra() override {
    return std::move(extra_);
  }
  void set_extra(std::unique_ptr<Extra> extra) override {
    extra_ = std::move(extra);
  }

  td::uint32 source() const override {
    return src_;
  }
  td::uint32 fork() const override {
    return fork_;
  }
  PublicKeyHash source_hash() const override {
    return src_hash_;
  }
  CatChainBlockHash hash() const override {
    return hash_;
  }
  CatChainBlockHeight height() const override {
    return height_;
  }

  CatChainBlock *prev() override {
    return prev_;
  }
  const CatChainBlock *prev() const override {
    return prev_;
  }

  const std::vector<CatChainBlock *> &deps() const override {
    return deps_;
  }
  const std::vector<CatChainBlockHeight> &vt() const override {
    return vt_;
  }

  bool preprocess_is_sent() const override {
    return preprocess_sent_;
  }
  void preprocess_sent() override {
    preprocess_sent_ = true;
  }

  bool is_processed() const override {
    return is_processed_;
  }
  void set_processed() override {
    is_processed_ = true;
  }

  bool is_descendant_of(CatChainBlock *block) override;

  CatChainBlockImpl(td::uint32 src, td::uint32 fork, const PublicKeyHash &src_hash, CatChainBlockHeight height,
                    const CatChainBlockHash &hash, td::SharedSlice payload, CatChainBlock *prev,
                    std::vector<CatChainBlock *> deps, std::vector<CatChainBlockHeight> vt);
};

}  // namespace catchain

}  // namespace ton
