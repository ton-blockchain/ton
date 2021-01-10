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

#include <vector>
#include "crypto/common/refcnt.hpp"
#include "crypto/common/refint.h"

#include "td/utils/int_types.h"

#include "adnl/utils.hpp"
#include "ton/ton-types.h"

#include "validator-session-types.h"
#include "catchain/catchain.h"

namespace ton {

namespace validatorsession {

class ValidatorSessionDescription {
 public:
  using HashType = td::uint32;
  struct RootObject {
   public:
    void *operator new(size_t size, ValidatorSessionDescription &desc, bool temp) {
      return desc.alloc(size, 8, temp);
    }
    void *operator new[](size_t size, ValidatorSessionDescription &desc, bool temp) {
      return desc.alloc(size, 8, temp);
    }
    void operator delete(void *ptr, ValidatorSessionDescription &desc, bool temp) {
      UNREACHABLE();
    }
    void operator delete[](void *ptr, ValidatorSessionDescription &desc, bool temp) {
      UNREACHABLE();
    }
    RootObject(td::uint32 size) : size_(size) {
    }
    td::uint32 get_size() const {
      return size_;
    }

   private:
    const td::uint32 size_;
  };

  virtual HashType compute_hash(td::Slice data) const = 0;
  HashType zero_hash() const {
    return 0;
  }
  virtual void *alloc(size_t size, size_t align, bool temp) = 0;
  virtual bool is_persistent(const void *ptr) const = 0;
  template <typename T>
  inline bool is_persistent(const T *ptr) const {
    return is_persistent(static_cast<const void *>(ptr));
  }
  virtual void clear_temp_memory() = 0;

  virtual ~ValidatorSessionDescription() = default;

  virtual PublicKeyHash get_source_id(td::uint32 idx) const = 0;
  virtual PublicKey get_source_public_key(td::uint32 idx) const = 0;
  virtual adnl::AdnlNodeIdShort get_source_adnl_id(td::uint32 idx) const = 0;
  virtual td::uint32 get_source_idx(PublicKeyHash id) const = 0;
  virtual ValidatorWeight get_node_weight(td::uint32 idx) const = 0;
  virtual td::uint32 get_total_nodes() const = 0;
  virtual ValidatorWeight get_cutoff_weight() const = 0;
  virtual ValidatorWeight get_total_weight() const = 0;
  virtual td::int32 get_node_priority(td::uint32 src_idx, td::uint32 round) const = 0;
  virtual td::uint32 get_max_priority() const = 0;
  virtual td::uint32 get_unixtime(td::uint64 t) const = 0;
  virtual td::uint32 get_attempt_seqno(td::uint64 t) const = 0;
  virtual td::uint32 get_self_idx() const = 0;
  virtual td::uint64 get_ts() const = 0;
  virtual const RootObject *get_by_hash(HashType hash, bool allow_temp) const = 0;
  virtual void update_hash(const RootObject *obj, HashType hash) = 0;
  virtual void on_reuse() = 0;
  virtual td::Timestamp attempt_start_at(td::uint32 att) const = 0;
  virtual ValidatorSessionCandidateId candidate_id(
      td::uint32 src_idx, ValidatorSessionRootHash root_hash, ValidatorSessionFileHash file_hash,
      ValidatorSessionCollatedDataFileHash collated_data_file_hash) const = 0;
  virtual td::Status check_signature(ValidatorSessionRootHash root_hash, ValidatorSessionFileHash file_hash,
                                     td::uint32 src_idx, td::Slice signature) const = 0;
  virtual td::Status check_approve_signature(ValidatorSessionRootHash root_hash, ValidatorSessionFileHash file_hash,
                                             td::uint32 src_idx, td::Slice signature) const = 0;
  virtual double get_delay(td::uint32 priority) const = 0;
  virtual double get_empty_block_delay() const = 0;
  virtual std::vector<catchain::CatChainNode> export_catchain_nodes() const = 0;

  virtual td::uint32 get_vote_for_author(td::uint32 attempt_seqno) const = 0;

  virtual const ValidatorSessionOptions &opts() const = 0;

  static std::unique_ptr<ValidatorSessionDescription> create(ValidatorSessionOptions opts,
                                                             std::vector<ValidatorSessionNode> &nodes,
                                                             PublicKeyHash local_id);
};

}  // namespace validatorsession

}  // namespace ton
