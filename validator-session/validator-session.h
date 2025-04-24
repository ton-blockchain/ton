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

#include "adnl/adnl.h"
#include "adnl/utils.hpp"
#include "rldp/rldp.h"

#include "ton/ton-types.h"

#include "overlay/overlays.h"
#include "catchain/catchain-types.h"

#include "validator-session-types.h"
#include "auto/tl/lite_api.h"

namespace ton {

namespace validatorsession {

class ValidatorSession : public td::actor::Actor {
 public:
  struct PrintId {
    catchain::CatChainSessionId instance_;
    PublicKeyHash local_id_;
  };
  virtual PrintId print_id() const = 0;

  class CandidateDecision {
   public:
    bool is_ok() const {
      return ok_;
    }
    td::uint32 ok_from() const {
      return ok_from_;
    }
    std::string reason() const {
      return reason_;
    }
    td::BufferSlice proof() const {
      return proof_.clone();
    }
    bool is_cached() const {
      return is_cached_;
    }
    void set_is_cached(bool value = true) {
      is_cached_ = value;
    }
    CandidateDecision(td::uint32 ok_from) {
      ok_ = true;
      ok_from_ = ok_from;
    }
    CandidateDecision(std::string reason, td::BufferSlice proof)
        : ok_(false), reason_(reason), proof_(std::move(proof)) {
    }

   private:
    bool ok_ = false;
    td::uint32 ok_from_ = 0;
    std::string reason_;
    td::BufferSlice proof_;
    bool is_cached_ = false;
  };

  struct GeneratedCandidate {
    BlockCandidate candidate;
    bool is_cached = false;
  };

  class Callback {
   public:
    virtual void on_candidate(BlockSourceInfo source_info, ValidatorSessionRootHash root_hash, td::BufferSlice data,
                              td::BufferSlice collated_data, td::Promise<CandidateDecision> promise) = 0;
    virtual void on_generate_slot(BlockSourceInfo source_info, td::Promise<GeneratedCandidate> promise) = 0;
    virtual void on_block_committed(BlockSourceInfo source_info, ValidatorSessionRootHash root_hash,
                                    ValidatorSessionFileHash file_hash, td::BufferSlice data,
                                    std::vector<std::pair<PublicKeyHash, td::BufferSlice>> signatures,
                                    std::vector<std::pair<PublicKeyHash, td::BufferSlice>> approve_signatures,
                                    ValidatorSessionStats stats) = 0;
    virtual void on_block_skipped(td::uint32 round) = 0;
    virtual void get_approved_candidate(PublicKey source, ValidatorSessionRootHash root_hash,
                                        ValidatorSessionFileHash file_hash,
                                        ValidatorSessionCollatedDataFileHash collated_data_file_hash,
                                        td::Promise<BlockCandidate> promise) = 0;
    virtual ~Callback() = default;
  };

  virtual void start() = 0;
  virtual void destroy() = 0;
  virtual void get_current_stats(td::Promise<ValidatorSessionStats> promise) = 0;
  virtual void get_end_stats(td::Promise<EndValidatorGroupStats> promise) = 0;
  virtual void get_validator_group_info_for_litequery(
      td::uint32 cur_round,
      td::Promise<std::vector<tl_object_ptr<lite_api::liteServer_nonfinal_candidateInfo>>> promise) = 0;
  virtual void set_catchain_max_block_delay(double delay, double delay_slow) = 0;

  static td::actor::ActorOwn<ValidatorSession> create(
      catchain::CatChainSessionId session_id, ValidatorSessionOptions opts, PublicKeyHash local_id,
      std::vector<ValidatorSessionNode> nodes, std::unique_ptr<Callback> callback,
      td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
      td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays, std::string db_root,
      std::string db_suffix, bool allow_unsafe_self_blocks_resync);
  virtual ~ValidatorSession() = default;
};

}  // namespace validatorsession

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb,
                                     const ton::validatorsession::ValidatorSession::PrintId &print_id) {
  sb << "[validatorsession " << print_id.instance_ << "@" << print_id.local_id_ << "]";
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::validatorsession::ValidatorSession *session) {
  sb << session->print_id();
  return sb;
}

}  // namespace td
