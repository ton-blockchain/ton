/* 
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission 
    to link the code of portions of this program with the OpenSSL library. 
    You must obey the GNU General Public License in all respects for all 
    of the code used other than OpenSSL. If you modify file(s) with this 
    exception, you may extend this exception to your version of the file(s), 
    but you are not obligated to do so. If you do not wish to do so, delete this 
    exception statement from your version. If you delete this exception statement 
    from all source files in the program, then also delete it here.

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "adnl/adnl.h"

#include "td/utils/misc.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/path.h"
#include "td/utils/Random.h"

#include "validator-session/validator-session-description.h"
#include "validator-session/validator-session-state.h"

#include <limits>
#include <memory>
#include <set>

class Description : public ton::validatorsession::ValidatorSessionDescription {
 public:
  HashType compute_hash(td::Slice data) const override {
    return td::crc32c(data);
  }
  HashType zero_hash() const {
    return 0;
  }
  void *alloc(size_t size, size_t align, bool temp) override {
    td::uint32 idx = temp ? 1 : 0;
    auto s = pdata_cur_[idx].fetch_add(size);
    CHECK(s + size <= pdata_size_[idx]);
    return static_cast<void *>(pdata_[idx] + s);
  }
  bool is_persistent(const void *ptr) const override {
    return ptr == nullptr || (ptr >= pdata_[0] && ptr < pdata_[0] + pdata_size_[0]);
  }
  void clear_temp_memory() override {
    pdata_cur_[1] = 0;
  }

  ton::PublicKeyHash get_source_id(td::uint32 idx) const override {
    CHECK(idx < total_nodes_);
    td::Bits256 x = td::Bits256::zero();
    auto &d = x.as_array();
    d[0] = static_cast<td::uint8>(idx);
    return ton::PublicKeyHash{x};
  }
  ton::PublicKey get_source_public_key(td::uint32 idx) const override {
    UNREACHABLE();
  }
  ton::adnl::AdnlNodeIdShort get_source_adnl_id(td::uint32 idx) const override {
    UNREACHABLE();
  }
  td::uint32 get_source_idx(ton::PublicKeyHash id) const override {
    auto x = id.bits256_value();
    auto y = x.as_array();
    return y[0];
  }
  ton::ValidatorWeight get_node_weight(td::uint32 idx) const override {
    return 1;
  }
  td::uint32 get_total_nodes() const override {
    return total_nodes_;
  }
  ton::ValidatorWeight get_cutoff_weight() const override {
    return 2 * total_nodes_ / 3 + 1;
  }
  ton::ValidatorWeight get_total_weight() const override {
    return total_nodes_;
  }
  td::int32 get_node_priority(td::uint32 src_idx, td::uint32 round) const override {
    round %= get_total_nodes();
    if (src_idx < round) {
      src_idx += get_total_nodes();
    }
    if (src_idx - round < opts_.round_candidates) {
      return src_idx - round;
    }
    return -1;
  }
  td::uint32 get_max_priority() const override {
    return opts_.round_candidates - 1;
  }
  td::uint32 get_unixtime(td::uint64 ts) const override {
    return static_cast<td::uint32>(ts >> 32);
  }
  td::uint32 get_attempt_seqno(td::uint64 ts) const override {
    return get_unixtime(ts) / opts_.round_attempt_duration;
  }
  td::uint32 get_self_idx() const override {
    UNREACHABLE();
  }
  td::uint64 get_ts() const override {
    auto tm = td::Clocks::system();
    CHECK(tm >= 0);
    auto t = static_cast<td::uint32>(tm);
    auto t2 = static_cast<td::uint64>((1ll << 32) * (tm - t));
    CHECK(t2 < (1ull << 32));
    return ((t * 1ull) << 32) + t2;
  }
  const RootObject *get_by_hash(HashType hash, bool allow_temp) const override {
    auto x = hash % cache_size;

    return cache_[x].load(std::memory_order_relaxed).ptr;
  }
  void update_hash(const RootObject *obj, HashType hash) override {
    if (!is_persistent(obj)) {
      return;
    }
    auto x = hash % cache_size;
    Cached p{obj};
    cache_[x].store(p, std::memory_order_relaxed);
  }
  void on_reuse() override {
  }
  td::Timestamp attempt_start_at(td::uint32 att) const override {
    return td::Timestamp::at_unix(att * opts_.round_attempt_duration);
  }
  ton::validatorsession::ValidatorSessionCandidateId candidate_id(
      td::uint32 src_idx, ton::validatorsession::ValidatorSessionRootHash root_hash,
      ton::validatorsession::ValidatorSessionFileHash file_hash,
      ton::validatorsession::ValidatorSessionCollatedDataFileHash collated_data_file_hash) const override {
    auto obj = ton::create_tl_object<ton::ton_api::validatorSession_candidateId>(get_source_id(src_idx).tl(), root_hash,
                                                                                 file_hash, collated_data_file_hash);
    return get_tl_object_sha_bits256(obj);
  }
  td::Status check_signature(ton::validatorsession::ValidatorSessionRootHash root_hash,
                             ton::validatorsession::ValidatorSessionFileHash file_hash, td::uint32 src_idx,
                             td::Slice signature) const override {
    if (signature.size() == 0) {
      return td::Status::Error("wrong size");
    }
    if (signature[0] == 126) {
      return td::Status::OK();
    } else {
      return td::Status::Error("invalid");
    }
  }
  td::Status check_approve_signature(ton::validatorsession::ValidatorSessionRootHash root_hash,
                                     ton::validatorsession::ValidatorSessionFileHash file_hash, td::uint32 src_idx,
                                     td::Slice signature) const override {
    if (signature.size() == 0) {
      return td::Status::Error("wrong size");
    }
    if (signature[0] == 127) {
      return td::Status::OK();
    } else {
      return td::Status::Error("invalid");
    }
  }
  double get_delay(td::uint32 priority) const override {
    return 0;
  }
  double get_empty_block_delay() const override {
    return 0;
  }
  std::vector<ton::catchain::CatChainNode> export_catchain_nodes() const override {
    UNREACHABLE();
  }

  td::uint32 get_vote_for_author(td::uint32 attempt_seqno) const override {
    return attempt_seqno % total_nodes_;
  }

  const ton::validatorsession::ValidatorSessionOptions &opts() const override {
    return opts_;
  }

  ~Description() {
    delete[] pdata_[0];
    delete[] pdata_[1];
  }

  Description(ton::validatorsession::ValidatorSessionOptions opts, td::uint32 total_nodes)
      : opts_(opts), total_nodes_(total_nodes) {
    pdata_size_[0] =
        static_cast<std::size_t>(std::numeric_limits<std::size_t>::max() < (1ull << 32) ? 1ull << 30 : 1ull << 33);
    pdata_size_[1] = 1 << 22;
    pdata_[0] = new td::uint8[pdata_size_[0]];
    pdata_[1] = new td::uint8[pdata_size_[1]];
    pdata_cur_[0] = 0;
    pdata_cur_[1] = 0;

    for (auto &el : cache_) {
      Cached v{nullptr};
      el.store(v, std::memory_order_relaxed);
    }

    CHECK(total_nodes_ > 0);
  }

 private:
  ton::validatorsession::ValidatorSessionOptions opts_;

  td::uint32 total_nodes_;

  static constexpr td::uint32 cache_size = (1 << 20);

  struct Cached {
    const RootObject *ptr;
  };
  std::array<std::atomic<Cached>, cache_size> cache_;

  td::uint8 *pdata_[2];
  std::atomic<size_t> pdata_cur_[2];
  size_t pdata_size_[2];
};

double myrand() {
  return td::Random::fast(0, 100) * 0.01;
}

int main() {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  td::set_default_failure_signal_handler().ensure();
  td::uint32 total_nodes = 100;

  ton::validatorsession::ValidatorSessionOptions opts;

  {
    auto descptr = new Description(opts, total_nodes);
    auto &desc = *descptr;

    auto c1 = desc.candidate_id(0, td::Bits256::zero(), td::Bits256::zero(), td::Bits256::zero());
    auto c2 = desc.candidate_id(1, td::Bits256::zero(), td::Bits256::zero(), td::Bits256::zero());
    CHECK(c1 != c2);

    auto s = ton::validatorsession::ValidatorSessionState::create(desc);
    CHECK(s);
    s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
    CHECK(s);
    auto att = 1000000000;

    for (td::uint32 i = 0; i < total_nodes; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);
      CHECK(act->get_id() == ton::ton_api::validatorSession_message_empty::ID);
    }

    {
      auto act = ton::create_tl_object<ton::ton_api::validatorSession_message_submittedBlock>(
          0, ton::Bits256::zero(), ton::Bits256::zero(), ton::Bits256::zero());
      s = ton::validatorsession::ValidatorSessionState::action(desc, s, 1, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    for (td::uint32 i = 0; i < total_nodes; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);
      LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_empty::ID) << act.get();
    }

    for (td::uint32 i = 0; i < total_nodes; i++) {
      bool found;
      s->choose_block_to_sign(desc, i, found);
      CHECK(!found);
      auto vec = s->choose_blocks_to_approve(desc, i);
      LOG_CHECK(vec.size() == 2) << vec.size();
      CHECK(vec[0]);
      CHECK(ton::validatorsession::SentBlock::get_block_id(vec[0]) == c2);
      CHECK(vec[1] == nullptr);
      CHECK(ton::validatorsession::SentBlock::get_block_id(vec[1]) == ton::validatorsession::skip_round_candidate_id());
    }
    for (td::uint32 i = 0; i < 2 * total_nodes / 3; i++) {
      td::BufferSlice sig{1};
      sig.as_slice()[0] = 127;
      auto act = ton::create_tl_object<ton::ton_api::validatorSession_message_approvedBlock>(0, c2, std::move(sig));
      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    for (td::uint32 i = 0; i < total_nodes; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);
      LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_empty::ID) << act.get();
    }

    for (td::uint32 i = 2 * total_nodes / 3; i < total_nodes; i++) {
      td::BufferSlice sig{1};
      sig.as_slice()[0] = 127;
      auto act = ton::create_tl_object<ton::ton_api::validatorSession_message_approvedBlock>(0, c2, std::move(sig));
      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    for (td::uint32 i = 0; i < total_nodes; i++) {
      bool found;
      s->choose_block_to_sign(desc, i, found);
      CHECK(!found);
      auto vec = s->choose_blocks_to_approve(desc, i);
      CHECK(vec.size() == 1);
      CHECK(vec[0] == nullptr);
      CHECK(ton::validatorsession::SentBlock::get_block_id(vec[0]) == ton::validatorsession::skip_round_candidate_id());
    }

    for (td::uint32 i = 0; i < total_nodes; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);
      LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_vote::ID) << act.get();

      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);

      auto act2 = s->create_action(desc, i, att);
      if (i < 2 * total_nodes / 3) {
        LOG_CHECK(act2->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "i=" << i << " " << act2.get();
      } else {
        LOG_CHECK(act2->get_id() == ton::ton_api::validatorSession_message_precommit::ID)
            << "i=" << i << " " << act2.get();
      }
    }
    for (td::uint32 j = 1; j < opts.max_round_attempts; j++) {
      auto act = s->create_action(desc, 0, att + j);
      CHECK(act);
      LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_vote::ID) << "j=" << j << " " << act.get();
    }
    for (td::uint32 j = opts.max_round_attempts; j < opts.max_round_attempts + 10; j++) {
      auto act = s->create_action(desc, 0, att + j);
      CHECK(act);
      LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "j=" << j << " " << act.get();
    }
    auto s_copy = s;
    auto att_copy = att;
    for (td::uint32 i = 0; i < total_nodes; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);

      if (i <= 2 * total_nodes / 3) {
        LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_precommit::ID)
            << "i=" << i << " " << act.get();
      } else {
        LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "i=" << i << " " << act.get();
      }

      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);

      auto act2 = s->create_action(desc, i, att);
      LOG_CHECK(act2->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "i=" << i << " " << act2.get();
    }

    att += 10;
    for (td::uint32 i = 0; i < total_nodes; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);
      LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "i=" << i << " " << act.get();
    }

    for (td::uint32 i = 0; i < total_nodes; i++) {
      bool found;
      auto block = s->choose_block_to_sign(desc, i, found);
      CHECK(found);
      CHECK(ton::validatorsession::SentBlock::get_block_id(block) == c2);
    }

    for (td::uint32 i = 0; i < 2 * total_nodes / 3; i++) {
      td::BufferSlice sig{1};
      sig.as_slice()[0] = 126;
      auto act = ton::create_tl_object<ton::ton_api::validatorSession_message_commit>(0, c2, std::move(sig));
      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    CHECK(s->cur_round_seqno() == 0);

    for (td::uint32 i = 2 * total_nodes / 3; i < total_nodes; i++) {
      td::BufferSlice sig{1};
      sig.as_slice()[0] = 126;
      auto act = ton::create_tl_object<ton::ton_api::validatorSession_message_commit>(0, c2, std::move(sig));
      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    CHECK(s->cur_round_seqno() == 1);

    auto sigs = s->get_committed_block_signatures(desc, 0);
    for (td::uint32 i = 0; i < sigs->size(); i++) {
      auto S = sigs->at(i);
      CHECK(S);
    }

    s = s_copy;
    att = att_copy;

    for (td::uint32 i = 0; i < total_nodes / 3; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);

      LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_precommit::ID) << "i=" << i << " " << act.get();

      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);

      auto act2 = s->create_action(desc, i, att);
      LOG_CHECK(act2->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "i=" << i << " " << act2.get();
    }

    att += opts.max_round_attempts - 1;

    do {
      att++;
      for (td::uint32 i = 0; i < total_nodes; i++) {
        auto act = s->create_action(desc, i, att);
        CHECK(act);

        if (i < total_nodes / 3) {
          LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_vote::ID) << "i=" << i << " " << act.get();
        } else {
          LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "i=" << i << " " << act.get();
        }

        s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
        CHECK(s);
        s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
        CHECK(s);

        auto act2 = s->create_action(desc, i, att);
        LOG_CHECK(act2->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "i=" << i << " " << act2.get();
      }
      desc.clear_temp_memory();
    } while (desc.get_vote_for_author(att) < total_nodes / 3);

    {
      auto act = ton::create_tl_object<ton::ton_api::validatorSession_message_submittedBlock>(
          0, ton::Bits256::zero(), ton::Bits256::zero(), ton::Bits256::zero());
      s = ton::validatorsession::ValidatorSessionState::action(desc, s, 0, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    auto idx = desc.get_vote_for_author(att);
    for (td::uint32 i = 0; i < total_nodes; i++) {
      LOG_CHECK(s->check_need_generate_vote_for(desc, i, att) == (i == idx))
          << i << " " << idx << " " << s->check_need_generate_vote_for(desc, i, att);
    }

    for (td::uint32 i = 0; i < total_nodes; i++) {
      td::BufferSlice sig{1};
      sig.as_slice()[0] = 127;
      auto act = ton::create_tl_object<ton::ton_api::validatorSession_message_approvedBlock>(0, c1, std::move(sig));
      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    {
      auto act = s->generate_vote_for(desc, idx, att);
      CHECK(act);
      act->candidate_ = c1;
      s = ton::validatorsession::ValidatorSessionState::action(desc, s, idx, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    td::BufferSlice buf{10240};
    td::StringBuilder sb{buf.as_slice()};

    s->dump(desc, sb, att);

    LOG(ERROR) << sb.as_cslice();

    for (td::uint32 i = 0; i < total_nodes; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);

      if (i < total_nodes / 3) {
        LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "i=" << i << " " << act.get();
      } else {
        LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_vote::ID) << "i=" << i << " " << act.get();
      }

      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    att++;
    idx = desc.get_vote_for_author(att);
    for (td::uint32 i = 0; i < total_nodes; i++) {
      LOG_CHECK(s->check_need_generate_vote_for(desc, i, att) == (i == idx))
          << i << " " << idx << " " << s->check_need_generate_vote_for(desc, i, att);
    }
    for (td::uint32 i = 0; i < total_nodes; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);

      LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "i=" << i << " " << act.get();

      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    {
      auto act = s->generate_vote_for(desc, idx, att);
      CHECK(act);
      act->candidate_ = c1;
      s = ton::validatorsession::ValidatorSessionState::action(desc, s, idx, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    for (td::uint32 i = 0; i < total_nodes; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);

      LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_vote::ID) << "i=" << i << " " << act.get();

      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    for (td::uint32 i = 0; i < total_nodes / 3; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);

      LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_precommit::ID) << "i=" << i << " " << act.get();

      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);

      auto act2 = s->create_action(desc, i, att);
      LOG_CHECK(act2->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "i=" << i << " " << act2.get();
    }

    att++;
    idx = desc.get_vote_for_author(att);
    {
      auto act = s->generate_vote_for(desc, idx, att);
      CHECK(act);
      act->candidate_ = c1;
      s = ton::validatorsession::ValidatorSessionState::action(desc, s, idx, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    for (td::uint32 i = 0; i < total_nodes; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);

      LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_vote::ID) << "i=" << i << " " << act.get();

      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);
    }

    for (td::uint32 i = 0; i < total_nodes; i++) {
      auto act = s->create_action(desc, i, att);
      CHECK(act);

      if (i <= 2 * total_nodes / 3) {
        LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_precommit::ID)
            << "i=" << i << " " << act.get();
      } else {
        LOG_CHECK(act->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "i=" << i << " " << act.get();
      }

      s = ton::validatorsession::ValidatorSessionState::action(desc, s, i, att, act.get());
      CHECK(s);
      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);

      auto act2 = s->create_action(desc, i, att);
      LOG_CHECK(act2->get_id() == ton::ton_api::validatorSession_message_empty::ID) << "i=" << i << " " << act2.get();
    }
    delete descptr;
  }

  {
    auto descptr = new Description(opts, total_nodes);
    auto &desc = *descptr;

    auto sig1 = ton::validatorsession::SessionBlockCandidateSignature::create(desc, td::BufferSlice{"a"});
    auto sig2 = ton::validatorsession::SessionBlockCandidateSignature::create(desc, td::BufferSlice{"b"});
    auto sig3 = ton::validatorsession::SessionBlockCandidateSignature::create(desc, td::BufferSlice{"c"});
    auto sig4 = ton::validatorsession::SessionBlockCandidateSignature::create(desc, td::BufferSlice{"d"});

    {
      auto m1 = ton::validatorsession::SessionBlockCandidateSignature::merge(desc, sig1, sig2);
      CHECK(m1->as_slice() == "a");
      auto m2 = ton::validatorsession::SessionBlockCandidateSignature::merge(desc, sig2, sig1);
      CHECK(m2->as_slice() == "a");
    }

    std::vector<const ton::validatorsession::SessionBlockCandidateSignature *> sig_vec_null;
    sig_vec_null.resize(desc.get_total_nodes(), nullptr);
    auto sig_vec1 = ton::validatorsession::SessionBlockCandidateSignatureVector::create(desc, sig_vec_null);
    auto sig_vec2 = ton::validatorsession::SessionBlockCandidateSignatureVector::create(desc, sig_vec_null);

    sig_vec1 = ton::validatorsession::SessionBlockCandidateSignatureVector::change(desc, sig_vec1, 0, sig1);
    sig_vec1 = ton::validatorsession::SessionBlockCandidateSignatureVector::change(desc, sig_vec1, 1, sig3);
    sig_vec2 = ton::validatorsession::SessionBlockCandidateSignatureVector::change(desc, sig_vec2, 0, sig4);
    sig_vec2 = ton::validatorsession::SessionBlockCandidateSignatureVector::change(desc, sig_vec2, 1, sig2);
    sig_vec2 = ton::validatorsession::SessionBlockCandidateSignatureVector::change(desc, sig_vec2, 2, sig4);

    {
      auto m1 = ton::validatorsession::SessionBlockCandidateSignatureVector::merge(
          desc, sig_vec1, sig_vec2, [&](const auto l, const auto r) {
            return ton::validatorsession::SessionBlockCandidateSignature::merge(desc, l, r);
          });
      CHECK(m1->at(0)->as_slice() == "a");
      CHECK(m1->at(1)->as_slice() == "b");
      CHECK(m1->at(2)->as_slice() == "d");
      CHECK(!m1->at(3));
      auto m2 = ton::validatorsession::SessionBlockCandidateSignatureVector::merge(
          desc, sig_vec2, sig_vec1, [&](const auto l, const auto r) {
            return ton::validatorsession::SessionBlockCandidateSignature::merge(desc, l, r);
          });
      CHECK(m2->at(0)->as_slice() == "a");
      CHECK(m2->at(1)->as_slice() == "b");
      CHECK(m2->at(2)->as_slice() == "d");
      CHECK(!m2->at(3));
    }

    auto sentb = ton::validatorsession::SentBlock::create(desc, 0, ton::Bits256::zero(), ton::Bits256::zero(),
                                                          ton::Bits256::zero());
    //auto sentb2 = ton::validatorsession::SentBlock::create(desc, 1, ton::Bits256::zero(), ton::Bits256::zero(),
    //                                                       ton::Bits256::zero());

    auto cand1 = ton::validatorsession::SessionBlockCandidate::create(desc, sentb, sig_vec1);
    auto cand2 = ton::validatorsession::SessionBlockCandidate::create(desc, sentb, sig_vec2);

    {
      auto m1 = ton::validatorsession::SessionBlockCandidate::merge(desc, cand1, cand2);
      CHECK(m1->get_block() == sentb);
      CHECK(m1->get_approvers_list()->at(0)->as_slice() == "a");
      CHECK(m1->get_approvers_list()->at(1)->as_slice() == "b");
      CHECK(m1->get_approvers_list()->at(2)->as_slice() == "d");
      CHECK(!m1->get_approvers_list()->at(3));
      auto m2 = ton::validatorsession::SessionBlockCandidate::merge(desc, cand2, cand1);
      CHECK(m2->get_block() == sentb);
      CHECK(m2->get_approvers_list()->at(0)->as_slice() == "a");
      CHECK(m2->get_approvers_list()->at(1)->as_slice() == "b");
      CHECK(m2->get_approvers_list()->at(2)->as_slice() == "d");
      CHECK(!m2->get_approvers_list()->at(3));
    }

    std::vector<bool> vote_vec_1;
    vote_vec_1.resize(desc.get_total_nodes());
    for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
      vote_vec_1[i] = td::Random::fast(0, 1) == 0;
    }
    std::vector<bool> vote_vec_2;
    vote_vec_2.resize(desc.get_total_nodes());
    for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
      vote_vec_2[i] = td::Random::fast(0, 1) == 0;
    }

    auto vote_t1 = ton::validatorsession::SessionVoteCandidate::create(
        desc, sentb, ton::validatorsession::CntVector<bool>::create(desc, vote_vec_1));
    auto vote_t2 = ton::validatorsession::SessionVoteCandidate::create(
        desc, sentb, ton::validatorsession::CntVector<bool>::create(desc, vote_vec_2));

    {
      auto m = ton::validatorsession::SessionVoteCandidate::merge(desc, vote_t1, vote_t2);
      for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
        CHECK(m->check_block_is_voted_by(i) == vote_t1->check_block_is_voted_by(i) ||
              vote_t2->check_block_is_voted_by(i));
      }
    }

    std::vector<bool> vote_vec;
    vote_vec.resize(desc.get_total_nodes(), false);
    auto vote1 = ton::validatorsession::SessionVoteCandidate::create(
        desc, nullptr, ton::validatorsession::CntVector<bool>::create(desc, vote_vec));
    auto vote1d = ton::validatorsession::SessionVoteCandidate::create(
        desc, sentb, ton::validatorsession::CntVector<bool>::create(desc, vote_vec));
    auto vote2 = ton::validatorsession::SessionVoteCandidate::create(
        desc, sentb, ton::validatorsession::CntVector<bool>::create(desc, vote_vec));
    auto vote2d = ton::validatorsession::SessionVoteCandidate::create(
        desc, sentb, ton::validatorsession::CntVector<bool>::create(desc, vote_vec));
    CHECK(ton::validatorsession::SessionVoteCandidate::Compare()(vote1, vote2));
    CHECK(!ton::validatorsession::SessionVoteCandidate::Compare()(vote2, vote1));

    for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
      if (i < desc.get_cutoff_weight()) {
        vote1 = ton::validatorsession::SessionVoteCandidate::push(desc, vote1, i);
      } else {
        vote2 = ton::validatorsession::SessionVoteCandidate::push(desc, vote2, i);
      }
      if (i < desc.get_cutoff_weight() - 1) {
        vote1d = ton::validatorsession::SessionVoteCandidate::push(desc, vote1d, i);
      } else {
        vote2d = ton::validatorsession::SessionVoteCandidate::push(desc, vote2d, i);
      }
    }

    auto v = ton::validatorsession::VoteVector::create(desc, {vote1, vote2});

    auto prec0_vec = ton::validatorsession::CntVector<bool>::create(desc, vote_vec);
    auto prec1_vec = ton::validatorsession::CntVector<bool>::change(desc, prec0_vec, 0, true);
    auto prec2_vec = ton::validatorsession::CntVector<bool>::change(desc, prec0_vec, 1, true);

    auto att0_0 =
        ton::validatorsession::ValidatorSessionRoundAttemptState::create(desc, 1, v, prec1_vec, nullptr, false);
    bool f;
    CHECK(att0_0->get_voted_block(desc, f) == nullptr);
    CHECK(f);

    auto att1_0 = ton::validatorsession::ValidatorSessionRoundAttemptState::create(
        desc, 2, ton::validatorsession::VoteVector::create(desc, {vote1d}), prec0_vec, nullptr, false);
    CHECK(att1_0->get_voted_block(desc, f) == nullptr);
    CHECK(!f);

    auto att1_1 = ton::validatorsession::ValidatorSessionRoundAttemptState::create(
        desc, 2, ton::validatorsession::VoteVector::create(desc, {vote2d}), prec0_vec, nullptr, false);
    CHECK(att1_1->get_voted_block(desc, f) == nullptr);
    CHECK(!f);

    auto att2_0 =
        ton::validatorsession::ValidatorSessionRoundAttemptState::create(desc, 3, v, prec2_vec, nullptr, false);
    CHECK(att2_0->get_voted_block(desc, f) == nullptr);
    CHECK(f);

    {
      auto m = ton::validatorsession::ValidatorSessionRoundAttemptState::merge(desc, att1_0, att1_1);
      CHECK(m->get_voted_block(desc, f) == sentb);
      CHECK(f);
    }

    std::vector<td::uint32> first_att_1;
    first_att_1.resize(desc.get_total_nodes());
    std::vector<td::uint32> first_att_2;
    first_att_2.resize(desc.get_total_nodes());
    for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
      first_att_1[i] = td::Random::fast(0, 1000000000);
      first_att_2[i] = td::Random::fast(0, 1000000000);
    }

    std::vector<td::uint32> last_precommit0;
    last_precommit0.resize(desc.get_total_nodes());
    last_precommit0[0] = 1;
    last_precommit0[1] = 3;

    std::vector<td::uint32> last_precommit1;
    last_precommit1.resize(desc.get_total_nodes());

    auto r1 = ton::validatorsession::ValidatorSessionRoundState::create(
        desc, nullptr, 0, false, ton::validatorsession::CntVector<td::uint32>::create(desc, first_att_1),
        ton::validatorsession::CntVector<td::uint32>::create(desc, last_precommit0), nullptr, sig_vec1,
        ton::validatorsession::AttemptVector::create(desc, {att0_0, att1_0, att2_0}));
    CHECK(r1->get_last_precommit(0) == 1);
    CHECK(r1->get_last_precommit(1) == 3);
    auto r2 = ton::validatorsession::ValidatorSessionRoundState::create(
        desc, nullptr, 0, false, ton::validatorsession::CntVector<td::uint32>::create(desc, first_att_2),
        ton::validatorsession::CntVector<td::uint32>::create(desc, last_precommit1), nullptr, sig_vec2,
        ton::validatorsession::AttemptVector::create(desc, {att1_1}));

    {
      auto m = ton::validatorsession::ValidatorSessionRoundState::merge(desc, r1, r2);
      CHECK(m);

      for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
        CHECK(m->get_first_attempt(i) == first_att_1[i]
                  ? first_att_2[i] ? std::min(first_att_1[i], first_att_2[i]) : first_att_1[i]
                  : first_att_2[i]);
      }
      for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
        if (i == 1) {
          CHECK(m->get_last_precommit(i) == 3);
        } else {
          CHECK(m->get_last_precommit(i) == 0);
        }
      }
    }

    delete descptr;
  }

  for (td::uint32 ver = 0; ver < 2; ver++) {
    double sign_prob = 1.0;
    double submit_prob = 0.8;
    double approve_prob = 0.5;
    double blocks_per_sec_per_node = 0.5;

    auto descptr = new Description(opts, total_nodes);
    auto &desc = *descptr;

    auto adj_total_nodes = total_nodes + (ver ? total_nodes / 3 : 0);

    std::vector<std::vector<const ton::validatorsession::ValidatorSessionState *>> states;
    states.resize(adj_total_nodes);

    td::uint64 ts = desc.get_ts();

    auto virt_state = ton::validatorsession::ValidatorSessionState::create(desc);
    virt_state = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, virt_state);

    for (td::uint32 ri = 0; ri < 100000; ri++) {
      auto ts_adj = ts;
      auto att = desc.get_attempt_seqno(ts_adj);

      //auto virt_round = virt_state->cur_round_seqno();
      auto virt_x = desc.get_vote_for_author(att);
      td::int32 x = virt_x;

      if (!virt_state->check_need_generate_vote_for(desc, virt_x, att) || myrand() < 0.5) {
        x = td::Random::fast(0, total_nodes - 1);
      }

      auto adj_x = x;
      if (x + total_nodes < adj_total_nodes && td::Random::fast(0, 1) == 0) {
        adj_x += total_nodes;
      }

      const ton::validatorsession::ValidatorSessionState *s;
      if (states[adj_x].size() == 0) {
        s = ton::validatorsession::ValidatorSessionState::create(desc);
      } else {
        s = *states[adj_x].rbegin();
      }

      for (td::uint32 z = 0; z < 3; z++) {
        auto y = td::Random::fast(0, adj_total_nodes - 2);
        if (adj_x <= y) {
          y++;
        }
        if (states[y].size() > 0) {
          auto k = td::Random::fast(static_cast<td::int32>(states[y].size() - 2),
                                    static_cast<td::int32>(states[y].size() - 1));
          if (k < 0) {
            k = 0;
          }
          s = ton::validatorsession::ValidatorSessionState::merge(desc, s, states[y][k]);
          CHECK(s);
          s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
          CHECK(s);
        }
      }
      auto round = s->cur_round_seqno();

      if (desc.get_node_priority(x, round) >= 0 && myrand() <= submit_prob && !s->check_block_is_sent_by(desc, x)) {
        auto act = ton::create_tl_object<ton::ton_api::validatorSession_message_submittedBlock>(
            round, ton::Bits256::zero(), ton::Bits256::zero(), ton::Bits256::zero());
        s = ton::validatorsession::ValidatorSessionState::action(desc, s, x, att, act.get());
        CHECK(s);
        s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
        CHECK(s);
      }

      auto vec = s->choose_blocks_to_approve(desc, x);
      if (vec.size() > 0 && myrand() <= approve_prob) {
        auto B = vec[td::Random::fast(0, static_cast<td::uint32>(vec.size() - 1))];
        auto id = ton::validatorsession::SentBlock::get_block_id(B);
        td::BufferSlice sig{B ? 1u : 0u};
        if (B) {
          sig.as_slice()[0] = 127;
        }
        auto act =
            ton::create_tl_object<ton::ton_api::validatorSession_message_approvedBlock>(round, id, std::move(sig));
        s = ton::validatorsession::ValidatorSessionState::action(desc, s, x, att, act.get());
        CHECK(s);
      }

      bool found;
      auto to_sign = s->choose_block_to_sign(desc, x, found);
      if (found && myrand() <= sign_prob) {
        auto id = ton::validatorsession::SentBlock::get_block_id(to_sign);
        td::BufferSlice sig{to_sign ? 1u : 0u};
        if (to_sign) {
          sig.as_slice()[0] = 126;
        }
        auto act = ton::create_tl_object<ton::ton_api::validatorSession_message_commit>(round, id, std::move(sig));
        s = ton::validatorsession::ValidatorSessionState::action(desc, s, x, att, act.get());
        CHECK(s);
      }

      if (s->check_need_generate_vote_for(desc, x, att)) {
        auto act = s->generate_vote_for(desc, x, att);
        s = ton::validatorsession::ValidatorSessionState::action(desc, s, x, att, act.get());
        CHECK(s);
      }

      while (true) {
        auto act = s->create_action(desc, x, att);
        bool stop = false;
        if (act->get_id() == ton::ton_api::validatorSession_message_empty::ID) {
          stop = true;
        }
        s = ton::validatorsession::ValidatorSessionState::action(desc, s, x, att, act.get());
        CHECK(s);
        if (stop) {
          break;
        }
      }

      bool made = false;
      s = ton::validatorsession::ValidatorSessionState::make_one(desc, s, x, att, made);
      CHECK(!made);

      s = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, s);
      CHECK(s);

      states[adj_x].push_back(s);

      if (myrand() <= 1.0 / blocks_per_sec_per_node / total_nodes) {
        ts += 1ull << 32;
      }
      desc.clear_temp_memory();

      virt_state = ton::validatorsession::ValidatorSessionState::merge(desc, virt_state, s);
      virt_state = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, virt_state);
    }

    td::BufferSlice buf{10240};
    td::StringBuilder sb{buf.as_slice()};

    virt_state->dump(desc, sb, desc.get_attempt_seqno(ts));

    LOG(ERROR) << sb.as_cslice();

    for (auto &x : states) {
      if (x.size() == 0) {
        std::cout << "<EMPTY>"
                  << "\n";
      } else {
        auto s = *x.rbegin();
        std::cout << "round=" << s->cur_round_seqno() << "\n";
      }
    }

    for (td::uint32 i = 0; i < total_nodes; i++) {
      for (td::uint32 j = 0; j < total_nodes; j++) {
        td::uint32 x = td::Random::fast(0, static_cast<td::uint32>(states[i].size() - 1));
        td::uint32 y = td::Random::fast(0, static_cast<td::uint32>(states[j].size() - 1));
        auto s1 = states[i][x];
        auto s2 = states[j][y];
        auto m1 = ton::validatorsession::ValidatorSessionState::merge(desc, s1, s2);
        auto m2 = ton::validatorsession::ValidatorSessionState::merge(desc, s2, s1);
        CHECK(m1->get_hash(desc) == m2->get_hash(desc));
        desc.clear_temp_memory();
      }
    }

    auto x_state = ton::validatorsession::ValidatorSessionState::create(desc);
    x_state = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, x_state);
    for (td::uint32 i = 0; i < adj_total_nodes; i++) {
      x_state = ton::validatorsession::ValidatorSessionState::merge(desc, x_state, *states[i].rbegin());
      x_state = ton::validatorsession::ValidatorSessionState::move_to_persistent(desc, x_state);
    }
    CHECK(x_state->get_hash(desc) == virt_state->get_hash(desc));
    delete descptr;
  }

  std::_Exit(0);
  return 0;
}
