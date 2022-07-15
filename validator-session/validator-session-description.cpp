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
#include "validator-session.hpp"
#include "td/utils/Random.h"
#include "td/utils/crypto.h"
#include "validator-session-description.hpp"

namespace ton {

namespace validatorsession {

ValidatorSessionDescriptionImpl::Source::Source(ValidatorSessionNode &node) {
  encryptor = node.pub_key.create_encryptor().move_as_ok();
  weight = node.weight;
  id = node.pub_key.compute_short_id();
  full_id = node.pub_key;
  adnl_id = node.adnl_id;
}

ValidatorSessionDescriptionImpl::ValidatorSessionDescriptionImpl(ValidatorSessionOptions opts,
                                                                 std::vector<ValidatorSessionNode> &nodes,
                                                                 PublicKeyHash local_id)
    : opts_(std::move(opts)) {
  td::uint32 size = static_cast<td::uint32>(nodes.size());
  ValidatorWeight total_weight = 0;
  for (td::uint32 i = 0; i < size; i++) {
    sources_.emplace_back(nodes[i]);
    total_weight += sources_[i].weight;
    CHECK(rev_sources_.find(sources_[i].id) == rev_sources_.end());
    rev_sources_[sources_[i].id] = i;
  }
  total_weight_ = total_weight;
  cutoff_weight_ = (total_weight * 2) / 3 + 1;
  auto it = rev_sources_.find(local_id);
  CHECK(it != rev_sources_.end());
  self_idx_ = it->second;

  pdata_temp_ptr_ = 0;
  pdata_temp_size_ = 1 << 27;
  pdata_temp_ = new td::uint8[pdata_temp_size_];

  pdata_perm_size_ = 1ull << 27;
  pdata_perm_ptr_ = 0;

  for (auto &el : cache_) {
    Cached v{nullptr};
    el.store(v, std::memory_order_relaxed);
  }
}

td::int32 ValidatorSessionDescriptionImpl::get_node_priority(td::uint32 src_idx, td::uint32 round) const {
  round %= get_total_nodes();
  if (src_idx < round) {
    src_idx += get_total_nodes();
  }
  if (src_idx - round < opts_.round_candidates) {
    return src_idx - round;
  }
  return -1;
}

td::uint32 ValidatorSessionDescriptionImpl::get_max_priority() const {
  return opts_.round_candidates - 1;
}

ValidatorSessionCandidateId ValidatorSessionDescriptionImpl::candidate_id(
    td::uint32 src_idx, ValidatorSessionRootHash root_hash, ValidatorSessionFileHash file_hash,
    ValidatorSessionCollatedDataFileHash collated_data_file_hash) const {
  auto obj = create_tl_object<ton_api::validatorSession_candidateId>(get_source_id(src_idx).tl(), root_hash, file_hash,
                                                                     collated_data_file_hash);
  return get_tl_object_sha_bits256(obj);
}

td::Status ValidatorSessionDescriptionImpl::check_signature(ValidatorSessionRootHash root_hash,
                                                            ValidatorSessionFileHash file_hash, td::uint32 src_idx,
                                                            td::Slice signature) const {
  auto obj = create_tl_object<ton_api::ton_blockId>(root_hash, file_hash);
  auto S = serialize_tl_object(obj, true);

  return sources_[src_idx].encryptor->check_signature(S.as_slice(), signature);
}

td::Status ValidatorSessionDescriptionImpl::check_approve_signature(ValidatorSessionRootHash root_hash,
                                                                    ValidatorSessionFileHash file_hash,
                                                                    td::uint32 src_idx, td::Slice signature) const {
  auto obj = create_tl_object<ton_api::ton_blockIdApprove>(root_hash, file_hash);
  auto S = serialize_tl_object(obj, true);

  return sources_[src_idx].encryptor->check_signature(S.as_slice(), signature);
}

std::vector<PublicKeyHash> ValidatorSessionDescriptionImpl::export_nodes() const {
  std::vector<PublicKeyHash> v;
  v.resize(get_total_nodes());
  for (td::uint32 i = 0; i < get_total_nodes(); i++) {
    v[i] = sources_[i].id;
  }
  return v;
}

std::vector<catchain::CatChainNode> ValidatorSessionDescriptionImpl::export_catchain_nodes() const {
  std::vector<catchain::CatChainNode> v;
  v.resize(get_total_nodes());
  for (td::uint32 i = 0; i < get_total_nodes(); i++) {
    v[i].pub_key = sources_[i].full_id;
    v[i].adnl_id = sources_[i].adnl_id;
  }
  return v;
}

std::vector<PublicKey> ValidatorSessionDescriptionImpl::export_full_nodes() const {
  std::vector<PublicKey> v;
  v.resize(get_total_nodes());
  for (td::uint32 i = 0; i < get_total_nodes(); i++) {
    v[i] = sources_[i].full_id;
  }
  return v;
}

double ValidatorSessionDescriptionImpl::get_delay(td::uint32 priority) const {
  return ((sources_.size() >= 5 ? 0 : 1) + priority) * opts_.next_candidate_delay;
}

td::uint32 ValidatorSessionDescriptionImpl::get_vote_for_author(td::uint32 attempt_seqno) const {
  return attempt_seqno % get_total_nodes();
}

const ValidatorSessionDescription::RootObject *ValidatorSessionDescriptionImpl::get_by_hash(HashType hash,
                                                                                            bool allow_temp) const {
  auto x = hash % cache_size;

  return cache_[x].load(std::memory_order_relaxed).ptr;
}

HashType ValidatorSessionDescriptionImpl::compute_hash(td::Slice data) const {
  return td::crc32c(data);
}

void ValidatorSessionDescriptionImpl::update_hash(const RootObject *obj, HashType hash) {
  if (!is_persistent(obj)) {
    return;
  }
  auto x = hash % cache_size;
  Cached p{obj};
  cache_[x].store(p, std::memory_order_relaxed);
}

void *ValidatorSessionDescriptionImpl::alloc(size_t size, size_t align, bool temp) {
  CHECK(align && !(align & (align - 1))); // align should be a power of 2
  auto get_padding = [&](const uint8_t* ptr) {
    return (-(size_t)ptr) & (align - 1);
  };
  if (temp) {
    pdata_temp_ptr_ += get_padding(pdata_temp_ + pdata_temp_ptr_);
    auto s = pdata_temp_ptr_;
    pdata_temp_ptr_ += size;
    CHECK(s + size <= pdata_temp_size_);
    return static_cast<void *>(pdata_temp_ + s);
  } else {
    while (true) {
      size_t idx = pdata_perm_ptr_ / pdata_perm_size_;
      if (idx < pdata_perm_.size()) {
        auto ptr = pdata_perm_[idx] + (pdata_perm_ptr_ % pdata_perm_size_);
        pdata_perm_ptr_ += get_padding(ptr);
        ptr += get_padding(ptr);
        pdata_perm_ptr_ += size;
        if (pdata_perm_ptr_ <= pdata_perm_.size() * pdata_perm_size_) {
          return static_cast<void *>(ptr);
        }
      }
      pdata_perm_.push_back(new td::uint8[pdata_perm_size_]);
    }
  }
}

bool ValidatorSessionDescriptionImpl::is_persistent(const void *ptr) const {
  if (ptr == nullptr) {
    return true;
  }
  for (auto &v : pdata_perm_) {
    if (ptr >= v && ptr <= v + pdata_perm_size_) {
      return true;
    }
  }
  return false;
}

std::unique_ptr<ValidatorSessionDescription> ValidatorSessionDescription::create(
    ValidatorSessionOptions opts, std::vector<ValidatorSessionNode> &nodes, PublicKeyHash local_id) {
  return std::make_unique<ValidatorSessionDescriptionImpl>(std::move(opts), nodes, local_id);
}

}  // namespace validatorsession

}  // namespace ton
