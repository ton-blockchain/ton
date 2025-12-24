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
*/
#include "adnl/adnl-test-loopback-implementation.h"
#include "adnl/utils.hpp"
#include "block/block.h"
#include "block/validator-set.h"
#include "catchain/catchain.h"
#include "common/errorlog.h"
#include "consensus/consensus-bus.h"
#include "consensus/null/consensus-bus.h"
#include "consensus/runtime.h"
#include "overlay/overlays.h"
#include "td/actor/coro_utils.h"
#include "td/utils/OptionParser.h"
#include "td/utils/port/signals.h"

using namespace ton;
using namespace ton::validator;

namespace {
td::Bits256 from_hex(td::Slice s) {
  td::Bits256 x;
  CHECK(x.from_hex(s) == 256);
  return x;
}

const ShardIdFull SHARD{basechainId, shardIdAll};
constexpr CatchainSeqno CC_SEQNO = 123;
const BlockIdExt MIN_MC_BLOCK_ID{masterchainId, shardIdAll, 0,
                                 from_hex("AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDDAAAAAAAABBBBBBBBCCCCCCCCDDDDDDDD"),
                                 from_hex("0123456012345601234560123456012345601234560123456777777701234567")};
const BlockIdExt FIRST_PARENT{basechainId, shardIdAll, 0,
                              from_hex("0000111122223333444455556666777788889999AAAABBBBCCCCDDDDEEEEFFFF"),
                              from_hex("89abcde89abcde89abcde89abcde89abcde89abcde89abcdefffffff89abcdef")};
const td::Bits256 SESSION_ID = from_hex("00001234000012340000123400001234aaaaaaaabbbbbbbbcccccccceeeeeeee");

class TestOverlayNode;

class TestOverlay : public td::actor::Actor {
 public:
  void register_node(size_t idx, td::actor::ActorId<TestOverlayNode> node) {
    if (nodes_.size() <= idx) {
      nodes_.resize(idx + 1);
    }
    nodes_[idx].push_back(std::move(node));
  }

  td::actor::Task<> send_message(PeerValidator src, size_t dst_idx, td::BufferSlice message);
  td::actor::Task<> send_candidate(PeerValidator src, size_t dst_idx, RawCandidateRef candidate);

 private:
  std::vector<std::vector<td::actor::ActorId<TestOverlayNode>>> nodes_;

  td::actor::Task<> before_receive(size_t src_idx, size_t dst_idx) {
    co_await td::actor::coro_sleep(td::Timestamp::in(td::Random::fast(PING_MIN, PING_MAX)));
    co_return td::Unit{};
  }

  static constexpr double PING_MIN = 0.1;
  static constexpr double PING_MAX = 0.2;
};

td::actor::ActorOwn<TestOverlay> test_overlay;

class TestOverlayNode : public runtime::SpawnsWith<ConsensusBus>, public runtime::ConnectsTo<ConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    td::actor::send_closure(test_overlay, &TestOverlay::register_node, owning_bus()->local_id.idx.value(),
                            actor_id(this));
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::StopRequested>) {
    stop();
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus> bus,
              std::shared_ptr<const ConsensusBus::OutgoingProtocolMessage> message) {
    if (message->recipient.has_value()) {
      CHECK(message->recipient.value() != bus->local_id.idx);
      td::actor::ask(test_overlay, &TestOverlay::send_message, bus->local_id, message->recipient->value(),
                     message->message.data.clone())
          .detach();
    } else {
      for (size_t i = 0; i < bus->validator_set.size(); ++i) {
        if (bus->local_id.idx.value() != i) {
          td::actor::ask(test_overlay, &TestOverlay::send_message, bus->local_id, i, message->message.data.clone())
              .detach();
        }
      }
    }
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus> bus, std::shared_ptr<const ConsensusBus::CandidateGenerated> event) {
    for (size_t i = 0; i < bus->validator_set.size(); ++i) {
      if (bus->local_id.idx.value() != i) {
        td::actor::ask(test_overlay, &TestOverlay::send_candidate, bus->local_id, i, event->candidate).detach();
      }
    }
  }

  void receive_message(PeerValidator src, td::BufferSlice data) {
    if (data.size() > ProtocolMessage::max_length) {
      LOG(WARNING) << "MISBEHAVIOR: Dropping oversized protocol message of size " << data.size() << " from " << src;
      return;
    }
    owning_bus().publish<ConsensusBus::IncomingProtocolMessage>(src.idx, std::move(data));
  }

  void receive_candidate(RawCandidateRef candidate) {
    owning_bus().publish<ConsensusBus::CandidateReceived>(candidate);
  }
};

td::actor::Task<> TestOverlay::send_message(PeerValidator src, size_t dst_idx, td::BufferSlice message) {
  co_await before_receive(src.idx.value(), dst_idx);
  for (const auto &instance : nodes_[dst_idx]) {
    td::actor::send_closure(instance, &TestOverlayNode::receive_message, src, message.clone());
  }
  co_return td::Unit{};
}

td::actor::Task<> TestOverlay::send_candidate(PeerValidator src, size_t dst_idx, RawCandidateRef candidate) {
  co_await before_receive(src.idx.value(), dst_idx);
  for (const auto &instance : nodes_[dst_idx]) {
    td::actor::send_closure(instance, &TestOverlayNode::receive_candidate, candidate);
  }
  co_return td::Unit{};
}

class TestConsensus;

class TestManagerFacade : public ManagerFacade {
 public:
  explicit TestManagerFacade(size_t node_idx, size_t instance_idx, td::actor::ActorId<TestConsensus> test_consensus)
      : node_idx_(node_idx), instance_idx_(instance_idx), test_consensus_(test_consensus) {
  }

  td::actor::Task<GeneratedCandidate> collate_block(ShardIdFull shard, BlockIdExt min_masterchain_block_id,
                                                    std::vector<BlockIdExt> prev, Ed25519_PublicKey creator,
                                                    BlockCandidatePriority priority, td::uint64 max_answer_size,
                                                    td::CancellationToken cancellation_token) override {
    LOG(WARNING) << "Collate block #" << prev[0].seqno() + 1;
    CHECK(shard == SHARD);
    CHECK(min_masterchain_block_id == MIN_MC_BLOCK_ID);
    CHECK(prev.size() == 1);

    td::Bits256 rand_data;
    td::Random::secure_bytes(rand_data.as_slice());
    td::Ref<vm::Cell> root = vm::CellBuilder{}.store_bytes(rand_data.as_slice()).finalize_novm();
    td::BufferSlice data = vm::std_boc_serialize(root, 31).move_as_ok();
    td::BufferSlice collated_data = {};

    BlockCandidate candidate(
        creator, BlockIdExt(BlockId(shard, prev[0].seqno() + 1), root->get_hash().bits(), td::sha256_bits256(data)),
        td::sha256_bits256(collated_data), data.clone(), collated_data.clone());
    co_return GeneratedCandidate{.candidate = std::move(candidate), .is_cached = false, .self_collated = true};
  }

  td::actor::Task<ValidateCandidateResult> validate_block_candidate(BlockCandidate candidate, ValidateParams params,
                                                                    td::Timestamp timeout) override {
    LOG(WARNING) << "Validate block #" << candidate.id.seqno();
    CHECK(params.prev.size() == 1);
    CHECK(params.prev[0].shard_full() == SHARD);
    CHECK(candidate.id.shard_full() == SHARD);
    CHECK(candidate.id.seqno() == params.prev[0].seqno() + 1);
    co_return ValidateCandidateResult{(UnixTime)td::Clocks::system()};
  }

  td::actor::Task<> accept_block(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                 td::Ref<block::BlockSignatureSet> signatures, int send_broadcast_mode,
                                 bool apply) override;

 private:
  size_t node_idx_;
  size_t instance_idx_;
  td::actor::ActorId<TestConsensus> test_consensus_;
};

class TestConsensus : public td::actor::Actor {
 public:
  td::actor::Task<> run() {
    auto result = co_await run_inner().wrap();
    if (result.is_error()) {
      LOG(FATAL) << "Test consensus error: " << result.move_as_error();
    }
    LOG(INFO) << "Test finished";
    std::exit(0);
  }

  td::actor::Task<> on_block_accepted(size_t node_idx, size_t instance_idx, BlockIdExt block_id,
                                      td::Ref<block::BlockSignatureSet> signatures) {
    CHECK(signatures.not_null());
    signatures->check_signatures(validator_set_, block_id).ensure();
    BlockSeqno seqno = block_id.seqno();
    if (accepted_blocks_.contains(seqno)) {
      LOG_CHECK(accepted_blocks_[seqno] == block_id) << "Accepted different blocks for seqno " << seqno;
    } else {
      accepted_blocks_[seqno] = block_id;
    }
    Instance &inst = nodes_[node_idx].instances[instance_idx];
    if (!inst.accepted_blocks.insert(seqno).second) {
      LOG(FATAL) << "Node " << node_idx << "." << instance_idx << " accepted block #" << seqno << " twice";
    }
    co_return td::Unit{};
  }

 private:
  td::actor::Task<> run_inner() {
    keyring_ = keyring::Keyring::create("");

    for (size_t i = 0; i < n_nodes_; ++i) {
      Node node;

      PrivateKey node_pk{privkeys::Ed25519::random()};
      node.public_key = node_pk.compute_public_key();
      node.node_id = node.public_key.compute_short_id();
      td::actor::send_closure(keyring_, &keyring::Keyring::add_key, std::move(node_pk), true, [](td::Unit) {});

      PrivateKey adnl_pk{privkeys::Ed25519::random()};
      node.adnl_id_full = adnl::AdnlNodeIdFull{adnl_pk.compute_public_key()};
      node.adnl_id = node.adnl_id_full.compute_short_id();
      td::actor::send_closure(keyring_, &keyring::Keyring::add_key, std::move(adnl_pk), true, [](td::Unit) {});

      node.weight = 11;

      nodes_.push_back(std::move(node));
    }

    std::vector<ValidatorDescr> validator_descrs;
    std::vector<PeerValidator> validators;
    for (size_t idx = 0; idx < nodes_.size(); ++idx) {
      Node &node = nodes_[idx];
      validator_descrs.push_back(ValidatorDescr(Ed25519_PublicKey{node.public_key.ed25519_value().raw()}, node.weight,
                                                node.adnl_id.bits256_value()));
      validators.push_back(PeerValidator{.idx = PeerValidatorId((int)idx),
                                         .key = node.public_key,
                                         .short_id = node.node_id,
                                         .adnl_id = node.adnl_id,
                                         .weight = node.weight});
    }
    validator_set_ = td::Ref<block::ValidatorSet>{true, CC_SEQNO, SHARD, std::move(validator_descrs)};

    test_overlay = td::actor::create_actor<TestOverlay>("test-overlay");

    for (size_t idx = 0; idx < n_nodes_; ++idx) {
      Node &node = nodes_[idx];
      size_t n_instances = idx < 4 ? 1 : 2;
      for (size_t i = 0; i < n_instances; ++i) {
        Instance inst;
        auto &runtime = inst.runtime;
        BlockAccepter::register_in(runtime);
        BlockProducer::register_in(runtime);
        BlockValidator::register_in(runtime);
        runtime.register_actor<TestOverlayNode>("PrivateOverlay");
        NullConsensus::register_in(runtime);

        inst.manager_facade = td::actor::create_actor<TestManagerFacade>(
            PSTRING() << "ManagerFacade." << idx << "." << i, idx, i, actor_id(this));
        auto bus = std::make_shared<NullConsensusBus>();
        bus->shard = SHARD;
        bus->manager = inst.manager_facade.get();
        bus->keyring = keyring_.get();
        bus->validator_opts = ValidatorManagerOptions::create(BlockIdExt{}, BlockIdExt{});
        bus->validator_set = validators;
        bus->local_id = validators[idx];
        bus->config = NewConsensusConfig{.target_rate_ms = 500,
                                         .max_block_size = 1 << 20,
                                         .max_collated_data_size = 1 << 20,
                                         .consensus = NewConsensusConfig::NullConsensus{}};
        bus->min_masterchain_block_id = MIN_MC_BLOCK_ID;
        bus->session_id = SESSION_ID;
        bus->first_block_parents = {FIRST_PARENT};
        bus->cc_seqno = CC_SEQNO;
        bus->validator_set_hash = validator_set_->get_validator_set_hash();
        inst.bus = runtime.start(std::move(bus), PSTRING() << "consensus." << idx << "." << i);
        node.instances.push_back(std::move(inst));
      }
    }

    co_await td::actor::coro_sleep(td::Timestamp::in(30.0));

    co_return co_await finalize();
  }

  td::actor::Task<> finalize() {
    LOG(WARNING) << "TEST FINISHED";
    for (Node &node : nodes_) {
      for (Instance &inst : node.instances) {
        inst.bus.publish<ConsensusBus::StopRequested>();
      }
    }
    co_await td::actor::coro_sleep(td::Timestamp::in(1.0));
    LOG(WARNING) << "TEST RESULTS:";
    for (size_t idx = 0; idx < n_nodes_; ++idx) {
      for (size_t inst_idx = 0; inst_idx < nodes_[idx].instances.size(); ++inst_idx) {
        Instance &inst = nodes_[idx].instances[inst_idx];
        BlockSeqno seqno = FIRST_PARENT.seqno();
        while (inst.accepted_blocks.contains(seqno + 1)) {
          ++seqno;
        }
        LOG(WARNING) << "Node #" << idx << " instance #" << inst_idx << " : synced up to block " << seqno;
      }
    }
    co_return td::Unit{};
  }

  struct Instance {
    runtime::Runtime runtime;
    td::actor::ActorOwn<TestManagerFacade> manager_facade;
    runtime::BusHandle<NullConsensusBus> bus;

    std::set<BlockSeqno> accepted_blocks;
  };
  struct Node {
    PublicKey public_key;
    PublicKeyHash node_id;
    adnl::AdnlNodeIdFull adnl_id_full;
    adnl::AdnlNodeIdShort adnl_id;
    ValidatorWeight weight = 0;
    std::vector<Instance> instances;
  };
  std::vector<Node> nodes_;
  size_t n_nodes_ = 8;
  td::Ref<block::ValidatorSet> validator_set_;

  td::actor::ActorOwn<keyring::Keyring> keyring_;

  std::map<BlockSeqno, BlockIdExt> accepted_blocks_;
};

td::actor::Task<> TestManagerFacade::accept_block(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                                  td::Ref<block::BlockSignatureSet> signatures, int send_broadcast_mode,
                                                  bool apply) {
  CHECK(id.shard_full() == SHARD);
  LOG(WARNING) << "Accept block #" << id.seqno() << " (" << (signatures.is_null() ? "no" : "with") << " signatures)";
  td::actor::ask(test_consensus_, &TestConsensus::on_block_accepted, node_idx_, instance_idx_, id, signatures).detach();
  co_return td::Unit{};
}

}  // namespace

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_WARNING);
  td::set_default_failure_signal_handler().ensure();

  td::actor::Scheduler scheduler({7});
  td::actor::ActorOwn<TestConsensus> test;

  scheduler.run_in_context([&] {
    test = td::actor::create_actor<TestConsensus>("test-consensus");
    td::actor::ask(test, &TestConsensus::run).detach();
  });
  while (scheduler.run(1)) {
  }

  return 0;
}
