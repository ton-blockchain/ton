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

#include "catchain/catchain-received-block.h"

namespace ton {

namespace catchain {

class CatChainReceiver;
class CatChainReceiverSource;

class CatChainReceivedBlockImpl final : public CatChainReceivedBlock {
 public:
  const td::SharedSlice &get_payload() const override {
    return payload_;
  }
  CatChainBlockHash get_hash() const override {
    return block_id_hash_;
  }
  const td::SharedSlice &get_signature() const override {
    return signature_;
  }

  CatChainBlockHeight get_height() const override {
    return height_;
  }
  CatChainReceivedBlock *get_prev() const override;
  CatChainBlockHash get_prev_hash() const override;

  const std::vector<CatChainBlockHeight> &get_vt() const override {
    return vt_;
  }

  std::vector<CatChainBlockHash> get_dep_hashes() const override;

  CatChainReceiver *get_chain() const override {
    return chain_;
  }

  td::uint32 get_fork_id() const override {
    return fork_id_;
  }

  td::uint32 get_source_id() const override {
    return source_id_;
  }

  tl_object_ptr<ton_api::catchain_block> export_tl() const override;
  tl_object_ptr<ton_api::catchain_block_dep> export_tl_dep() const override;

  void find_pending_deps(std::vector<CatChainBlockHash> &vec, td::uint32 max_size) const override;

  bool has_rev_deps() const override {
    return !rev_deps_.empty();
  }

 public:
  bool initialized() const override {
    return state_ >= bs_initialized;
  }
  bool delivered() const override {
    return state_ >= bs_delivered;
  }
  bool is_ill() const override {
    return state_ == bs_ill;
  }
  bool is_custom() const override {
    return is_custom_;
  }
  bool in_db() const override {
    return in_db_;
  }

 public:
  void initialize(tl_object_ptr<ton_api::catchain_block> block, td::SharedSlice payload) override;

  void run() override;
  void pre_deliver(ton_api::catchain_block_data_fork &b);
  void pre_deliver(ton_api::catchain_block_data_badBlock &b);
  void pre_deliver(ton_api::catchain_block_data_nop &b);
  template <class T>
  void pre_deliver(T &b) {
    // do nothing, it is custom block
    is_custom_ = true;
  }
  void pre_deliver();
  void deliver();

  void dep_delivered(CatChainReceivedBlockImpl *block);
  void dep_ill(CatChainReceivedBlockImpl *block);

  void set_ill() override;
  void schedule();

  void written() override;

 public:
  CatChainReceivedBlockImpl(tl_object_ptr<ton_api::catchain_block> block, td::SharedSlice payload,
                            CatChainReceiver *chain);
  CatChainReceivedBlockImpl(tl_object_ptr<ton_api::catchain_block_dep> block, CatChainReceiver *chain);

  CatChainReceivedBlockImpl(td::uint32 source_id, const CatChainSessionId &hash, CatChainReceiver *chain);

 private:
  enum State {
    bs_none,
    bs_ill,
    bs_initialized,
    bs_delivered,
  } state_ = bs_none;

  void update_vt(CatChainReceivedBlockImpl *block);

  void add_rev_dep(CatChainReceivedBlockImpl *block);

  void initialize_fork();

  td::uint32 fork_id_{0};
  td::uint32 source_id_;
  CatChainReceiver *chain_;

  td::SharedSlice payload_;

  CatChainBlockHash block_id_hash_{};
  CatChainBlockPayloadHash data_payload_hash_{};

  CatChainReceivedBlockImpl *prev_ = nullptr;
  CatChainBlockHeight height_;

  CatChainReceivedBlockImpl *next_ = nullptr;

  std::vector<CatChainReceivedBlockImpl *> block_deps_;
  std::vector<CatChainBlockHeight> vt_;

  td::SharedSlice signature_;

  std::vector<CatChainReceivedBlockImpl *> rev_deps_;

  td::uint32 pending_deps_ = 0;

  bool is_custom_ = false;
  bool in_db_ = false;
};

}  // namespace catchain

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChainReceivedBlockImpl &block) {
  sb << "[block " << block.get_chain()->get_incarnation() << " " << block.get_source_id() << " " << block.get_fork_id()
     << " " << block.get_hash() << "]";
  return sb;
}
inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChainReceivedBlockImpl *block) {
  sb << *block;
  return sb;
}

}  // namespace td
