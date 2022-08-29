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

#include <map>

#include "catchain-receiver-source.h"
#include "catchain-receiver.h"
#include "catchain-received-block.h"
#include <queue>

namespace ton {

namespace catchain {

class CatChainReceiverSourceImpl : public CatChainReceiverSource {
 public:
  td::uint32 get_id() const override {
    return id_;
  }
  PublicKeyHash get_hash() const override {
    return src_;
  }
  PublicKey get_full_id() const override {
    return full_id_;
  }
  adnl::AdnlNodeIdShort get_adnl_id() const override {
    return adnl_id_;
  }

  td::uint32 add_fork() override;

  bool blamed() const override {
    return blamed_;
  }
  void blame(td::uint32 fork, CatChainBlockHeight height) override;
  void blame() override;
  void block_received(CatChainBlockHeight height) override;
  void block_delivered(CatChainBlockHeight height) override;

  const std::vector<td::uint32> &get_forks() const override {
    return fork_ids_;
  }

  const std::vector<CatChainBlockHeight> &get_blamed_heights() const override {
    return blamed_heights_;
  }

  td::actor::ActorId<EncryptorAsync> get_encryptor() const {
    return encryptor_.get();
  }
  Encryptor *get_encryptor_sync() const override {
    return encryptor_sync_.get();
  }

  td::uint32 get_forks_cnt() const override {
    return static_cast<td::uint32>(fork_ids_.size());
  }

  CatChainBlockHeight delivered_height() const override {
    return delivered_height_;
  }
  CatChainBlockHeight received_height() const override {
    return received_height_;
  }
  bool has_unreceived() const override {
    if (blamed()) {
      return true;
    }
    if (blocks_.empty()) {
      return false;
    }
    CHECK(blocks_.rbegin()->second->get_height() >= received_height_);
    return blocks_.rbegin()->second->get_height() > received_height_;
  }
  bool has_undelivered() const override {
    return delivered_height_ < received_height_;
  }
  CatChainReceivedBlock *get_block(CatChainBlockHeight height) const override;

  void on_new_block(CatChainReceivedBlock *block) override;
  void on_found_fork_proof(const td::Slice &proof) override;
  bool fork_is_found() const override {
    return !fork_proof_.empty();
  }
  td::BufferSlice fork_proof() const override {
    if (!fork_proof_.empty()) {
      return fork_proof_.clone_as_buffer_slice();
    } else {
      return {};
    }
  }

  CatChainReceiver *get_chain() const override {
    return chain_;
  }

  CatChainReceiverSourceImpl(CatChainReceiver *chain, PublicKey source, adnl::AdnlNodeIdShort adnl_id, td::uint32 id);

 private:
  CatChainReceiver *chain_;
  td::uint32 id_;
  PublicKeyHash src_;
  bool blamed_ = false;
  PublicKey full_id_;
  adnl::AdnlNodeIdShort adnl_id_;

  std::vector<td::uint32> fork_ids_;
  td::actor::ActorOwn<EncryptorAsync> encryptor_;
  std::unique_ptr<Encryptor> encryptor_sync_;
  std::vector<CatChainBlockHeight> blamed_heights_;
  std::map<CatChainBlockHeight, CatChainReceivedBlock *> blocks_;
  td::SharedSlice fork_proof_;

  CatChainBlockHeight delivered_height_ = 0;
  CatChainBlockHeight received_height_ = 0;
};

}  // namespace catchain

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChainReceiverSourceImpl &source) {
  sb << "[source " << source.get_chain()->get_incarnation() << " " << source.get_id() << "]";
  return sb;
}
inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChainReceiverSourceImpl *source) {
  sb << *source;
  return sb;
}

}  // namespace td
