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

#include <set>
#include <map>

#include "validator-session.h"
#include "validator-session-state.h"

#include "keys/encryptor.h"

namespace ton {

namespace validatorsession {

class ValidatorSessionDescriptionImpl : public ValidatorSessionDescription {
 private:
  ValidatorSessionOptions opts_;

  struct Source {
    PublicKeyHash id;
    ValidatorWeight weight;
    std::unique_ptr<Encryptor> encryptor;
    PublicKey full_id;
    adnl::AdnlNodeIdShort adnl_id;
    Source(ValidatorSessionNode &node);
  };
  std::map<PublicKeyHash, td::uint32> rev_sources_;

  std::vector<Source> sources_;
  ValidatorWeight cutoff_weight_;
  ValidatorWeight total_weight_;
  td::uint32 self_idx_;

  static constexpr td::uint32 cache_size = (1 << 20);

  struct Cached {
    const RootObject *ptr;
  };
  std::array<std::atomic<Cached>, cache_size> cache_;
  //std::array<std::atomic<Cached>, cache_size> temp_cache_;

  td::uint8 *pdata_temp_;
  size_t pdata_temp_ptr_;
  size_t pdata_temp_size_;

  size_t pdata_perm_size_;
  std::vector<td::uint8 *> pdata_perm_;
  size_t pdata_perm_ptr_;
  std::atomic<td::uint64> reuse_{0};

 public:
  ValidatorSessionDescriptionImpl(ValidatorSessionOptions opts, std::vector<ValidatorSessionNode> &nodes,
                                  PublicKeyHash local_id);
  PublicKeyHash get_source_id(td::uint32 idx) const override {
    CHECK(idx < sources_.size());
    return sources_[idx].id;
  }
  virtual PublicKey get_source_public_key(td::uint32 idx) const override {
    CHECK(idx < sources_.size());
    return sources_[idx].full_id;
  }
  adnl::AdnlNodeIdShort get_source_adnl_id(td::uint32 idx) const override {
    CHECK(idx < sources_.size());
    return sources_[idx].adnl_id;
  }
  td::uint32 get_source_idx(PublicKeyHash id) const override {
    auto it = rev_sources_.find(id);
    CHECK(it != rev_sources_.end());
    return it->second;
  }
  ValidatorWeight get_node_weight(td::uint32 idx) const override {
    CHECK(idx < sources_.size());
    return sources_[idx].weight;
  }
  td::uint32 get_total_nodes() const override {
    return static_cast<td::uint32>(sources_.size());
  }
  ValidatorWeight get_cutoff_weight() const override {
    return cutoff_weight_;
  }
  ValidatorWeight get_total_weight() const override {
    return total_weight_;
  }
  td::int32 get_node_priority(td::uint32 src_idx, td::uint32 round) const override;
  td::uint32 get_max_priority() const override;
  td::uint32 get_unixtime(td::uint64 ts) const override {
    return static_cast<td::uint32>(ts >> 32);
  }
  td::uint32 get_attempt_seqno(td::uint64 ts) const override {
    return get_unixtime(ts) / opts_.round_attempt_duration;
  }
  const RootObject *get_by_hash(HashType hash, bool allow_temp) const override;
  void on_reuse() override {
    if (reuse_++ % (1 << 17) == 0) {
      LOG(INFO) << "reused " << reuse_ << " times";
    }
  }
  void update_hash(const RootObject *obj, HashType hash) override;
  void *alloc(size_t size, size_t align, bool temp) override;
  void clear_temp_memory() override {
    pdata_temp_ptr_ = 0;
  }
  bool is_persistent(const void *ptr) const override;
  HashType compute_hash(td::Slice data) const override;
  td::Timestamp attempt_start_at(td::uint32 att) const override {
    return td::Timestamp::at_unix(att * opts_.round_attempt_duration);
  }
  td::uint32 get_self_idx() const override {
    return self_idx_;
  }
  td::uint64 get_ts() const override {
    auto tm = td::Clocks::system();
    CHECK(tm >= 0);
    auto t = static_cast<td::uint32>(tm);
    auto t2 = static_cast<td::uint64>((1ll << 32) * (tm - t));
    CHECK(t2 < (1ull << 32));
    return ((t * 1ull) << 32) + t2;
  }
  ValidatorSessionCandidateId candidate_id(td::uint32 src_idx, ValidatorSessionRootHash root_hash,
                                           ValidatorSessionFileHash file_hash,
                                           ValidatorSessionCollatedDataFileHash collated_data_file_hash) const override;
  td::Status check_signature(ValidatorSessionRootHash root_hash, ValidatorSessionFileHash file_hash, td::uint32 src_idx,
                             td::Slice signature) const override;
  td::Status check_approve_signature(ValidatorSessionRootHash root_hash, ValidatorSessionFileHash file_hash,
                                     td::uint32 src_idx, td::Slice signature) const override;
  double get_delay(td::uint32 priority) const override;
  double get_empty_block_delay() const override {
    return std::max(get_delay(get_max_priority() + 1), 1.0);
  }
  td::uint32 get_vote_for_author(td::uint32 attempt_seqno) const override;
  std::vector<PublicKeyHash> export_nodes() const;
  std::vector<catchain::CatChainNode> export_catchain_nodes() const override;
  std::vector<PublicKey> export_full_nodes() const;
  const ValidatorSessionOptions &opts() const override {
    return opts_;
  }
  ~ValidatorSessionDescriptionImpl() {
    delete[] pdata_temp_;
    for (auto &x : pdata_perm_) {
      delete[] x;
    }
  }
};

}  // namespace validatorsession

}  // namespace ton
