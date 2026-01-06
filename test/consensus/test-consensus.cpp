/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "adnl/utils.hpp"
#include "block/block.h"
#include "block/validator-set.h"
#include "consensus/runtime.h"
#include "consensus/simplex/bus.h"
#include "td/actor/coro_utils.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Random.h"
#include "td/utils/port/signals.h"

using namespace ton;
using namespace ton::validator;
using namespace ton::validator::consensus;

namespace {
td::Bits256 from_hex(td::Slice s) {
  td::Bits256 x;
  CHECK(x.from_hex(s) == 256);
  return x;
}

constexpr CatchainSeqno CC_SEQNO = 123;
const BlockIdExt MIN_MC_BLOCK_ID{masterchainId, shardIdAll, 0,
                                 from_hex("AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDDAAAAAAAABBBBBBBBCCCCCCCCDDDDDDDD"),
                                 from_hex("0123456012345601234560123456012345601234560123456777777701234567")};
const td::Bits256 SESSION_ID = from_hex("00001234000012340000123400001234aaaaaaaabbbbbbbbcccccccceeeeeeee");

ShardIdFull SHARD{basechainId, shardIdAll};
BlockIdExt FIRST_PARENT{basechainId, shardIdAll, 0,
                        from_hex("0000111122223333444455556666777788889999AAAABBBBCCCCDDDDEEEEFFFF"),
                        from_hex("89abcde89abcde89abcde89abcde89abcde89abcde89abcdefffffff89abcdef")};

double NET_PING_MIN = 0.05;
double NET_PING_MAX = 0.1;
double NET_LOSS = 0.0;

size_t N_NODES = 8;
size_t N_DOUBLE_NODES = 0;

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

  td::actor::Task<> before_receive(size_t src_idx, size_t dst_idx, bool is_candidate) {
    if (!is_candidate && td::Random::fast(0.0, 1.0) < NET_LOSS) {
      co_return td::Status::Error("packet lost");
    }
    co_await td::actor::coro_sleep(td::Timestamp::in(td::Random::fast(NET_PING_MIN, NET_PING_MAX)));
    co_return td::Unit{};
  }
};

td::actor::ActorOwn<TestOverlay> test_overlay;

class TestOverlayNode : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    td::actor::send_closure(test_overlay, &TestOverlay::register_node, owning_bus()->local_id.idx.value(),
                            actor_id(this));
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const OutgoingProtocolMessage> message) {
    if (message->recipient.has_value()) {
      CHECK(message->recipient.value() != bus->local_id.idx);
      td::actor::ask(test_overlay, &TestOverlay::send_message, bus->local_id, message->recipient->value(),
                     message->message.data.clone())
          .detach_silent();
    } else {
      for (size_t i = 0; i < bus->validator_set.size(); ++i) {
        if (bus->local_id.idx.value() != i) {
          td::actor::ask(test_overlay, &TestOverlay::send_message, bus->local_id, i, message->message.data.clone())
              .detach_silent();
        }
      }
    }
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const CandidateGenerated> event) {
    for (size_t i = 0; i < bus->validator_set.size(); ++i) {
      if (bus->local_id.idx.value() != i) {
        td::actor::ask(test_overlay, &TestOverlay::send_candidate, bus->local_id, i, event->candidate).detach_silent();
      }
    }
  }

  void receive_message(PeerValidator src, td::BufferSlice data) {
    if (data.size() > ProtocolMessage::max_length) {
      LOG(WARNING) << "MISBEHAVIOR: Dropping oversized protocol message of size " << data.size() << " from " << src;
      return;
    }
    owning_bus().publish<IncomingProtocolMessage>(src.idx, std::move(data));
  }

  void receive_candidate(RawCandidateRef candidate) {
    owning_bus().publish<CandidateReceived>(candidate);
  }
};

td::actor::Task<> TestOverlay::send_message(PeerValidator src, size_t dst_idx, td::BufferSlice message) {
  co_await before_receive(src.idx.value(), dst_idx, false);
  for (const auto &instance : nodes_[dst_idx]) {
    td::actor::send_closure(instance, &TestOverlayNode::receive_message, src, message.clone());
  }
  co_return td::Unit{};
}

td::actor::Task<> TestOverlay::send_candidate(PeerValidator src, size_t dst_idx, RawCandidateRef candidate) {
  co_await before_receive(src.idx.value(), dst_idx, true);
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
    LOG(WARNING) << "Test finished";
    std::exit(0);
  }

  td::actor::Task<> on_block_accepted(size_t node_idx, size_t instance_idx, BlockIdExt block_id,
                                      td::Ref<block::BlockSignatureSet> signatures) {
    if (signatures->is_final()) {
      signatures->check_signatures(validator_set_, block_id).ensure();
    } else {
      CHECK(!SHARD.is_masterchain());
      signatures->check_approve_signatures(validator_set_, block_id).ensure();
    }
    BlockSeqno seqno = block_id.seqno();
    if (accepted_blocks_.contains(seqno)) {
      LOG_CHECK(accepted_blocks_[seqno] == block_id) << "Accepted different blocks for seqno " << seqno;
    } else {
      accepted_blocks_[seqno] = block_id;

      for (Node &node : nodes_) {
        for (Instance &inst : node.instances) {
          inst.bus.publish<BlockFinalizedInMasterchain>(block_id);
        }
      }
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

    for (size_t i = 0; i < N_NODES; ++i) {
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
    ValidatorWeight total_weight = 0;
    for (size_t idx = 0; idx < nodes_.size(); ++idx) {
      Node &node = nodes_[idx];
      validator_descrs.push_back(ValidatorDescr(Ed25519_PublicKey{node.public_key.ed25519_value().raw()}, node.weight,
                                                node.adnl_id.bits256_value()));
      validators.push_back(PeerValidator{.idx = PeerValidatorId((int)idx),
                                         .key = node.public_key,
                                         .short_id = node.node_id,
                                         .adnl_id = node.adnl_id,
                                         .weight = node.weight});
      total_weight += node.weight;
    }
    validator_set_ = td::Ref<block::ValidatorSet>{true, CC_SEQNO, SHARD, std::move(validator_descrs)};

    test_overlay = td::actor::create_actor<TestOverlay>("test-overlay");

    for (size_t idx = 0; idx < N_NODES; ++idx) {
      Node &node = nodes_[idx];
      size_t n_instances = idx < N_DOUBLE_NODES ? 2 : 1;
      for (size_t i = 0; i < n_instances; ++i) {
        Instance inst;
        auto &runtime = inst.runtime;
        BlockAccepter::register_in(runtime);
        BlockProducer::register_in(runtime);
        BlockValidator::register_in(runtime);
        runtime.register_actor<TestOverlayNode>("PrivateOverlay");
        simplex::CandidateResolver::register_in(runtime);
        simplex::Consensus::register_in(runtime);
        simplex::Pool::register_in(runtime);

        inst.manager_facade = td::actor::create_actor<TestManagerFacade>(
            PSTRING() << "ManagerFacade." << idx << "." << i, idx, i, actor_id(this));
        auto bus = std::make_shared<simplex::Bus>();
        bus->shard = SHARD;
        bus->manager = inst.manager_facade.get();
        bus->keyring = keyring_.get();
        bus->validator_opts = ValidatorManagerOptions::create(BlockIdExt{}, BlockIdExt{});
        bus->validator_set = validators;
        bus->total_weight = total_weight;
        bus->local_id = validators[idx];
        bus->config = NewConsensusConfig{.target_rate_ms = 500,
                                         .max_block_size = 1 << 20,
                                         .max_collated_data_size = 1 << 20,
                                         .consensus = NewConsensusConfig::Simplex{}};
        bus->simplex_config = bus->config.consensus.get<NewConsensusConfig::Simplex>();
        bus->min_masterchain_block_id = MIN_MC_BLOCK_ID;
        bus->session_id = SESSION_ID;
        bus->first_block_parents = {FIRST_PARENT};
        bus->cc_seqno = CC_SEQNO;
        bus->validator_set_hash = validator_set_->get_validator_set_hash();
        inst.bus = runtime.start(std::move(bus), PSTRING() << "consensus." << idx << "." << i);
        node.instances.push_back(std::move(inst));
      }
    }

    co_await td::actor::coro_sleep(td::Timestamp::in(300.0));

    co_return co_await finalize();
  }

  td::actor::Task<> finalize() {
    LOG(WARNING) << "TEST FINISHED";
    for (Node &node : nodes_) {
      for (Instance &inst : node.instances) {
        inst.bus.publish<StopRequested>();
      }
    }
    co_await td::actor::coro_sleep(td::Timestamp::in(1.0));
    LOG(WARNING) << "TEST RESULTS:";
    for (size_t idx = 0; idx < N_NODES; ++idx) {
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
    simplex::BusHandle bus;

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
  td::Ref<block::ValidatorSet> validator_set_;

  td::actor::ActorOwn<keyring::Keyring> keyring_;

  std::map<BlockSeqno, BlockIdExt> accepted_blocks_;
};

td::actor::Task<> TestManagerFacade::accept_block(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                                  td::Ref<block::BlockSignatureSet> signatures, int send_broadcast_mode,
                                                  bool apply) {
  CHECK(id.shard_full() == SHARD);
  LOG(WARNING) << "Accept block #" << id.seqno() << " (" << (signatures->is_final() ? "final" : "notarize")
               << " signatures)";
  td::actor::ask(test_consensus_, &TestConsensus::on_block_accepted, node_idx_, instance_idx_, id, signatures).detach();
  co_return td::Unit{};
}

}  // namespace

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_WARNING);
  td::set_default_failure_signal_handler().ensure();

  td::OptionParser p;
  p.set_description("test consensus");
  p.add_option('h', "help", "prints_help", [&]() {
    std::cout << (PSLICE() << p).c_str();
    std::exit(2);
  });
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_option('m', "masterchain", "masterchain consensus (default is shardchain)", [&]() {
    SHARD = ShardIdFull{masterchainId};
    FIRST_PARENT = MIN_MC_BLOCK_ID;
  });
  p.add_checked_option('n', "n-nodes", "number of nodes (default: 8)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(N_NODES, td::to_integer_safe<td::uint32>(arg));
    if (N_NODES == 0) {
      return td::Status::Error(PSTRING() << "invalid n-nodes value " << arg);
    }
    return td::Status::OK();
  });
  p.add_checked_option('\0', "n-double-nodes", "number of nodes with two instances (default: 0)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(N_DOUBLE_NODES, td::to_integer_safe<td::uint32>(arg));
    return td::Status::OK();
  });
  p.add_checked_option('\0', "net-ping-min", "network ping (minimal)", [&](td::Slice arg) {
    NET_PING_MIN = td::to_double(arg);
    if (NET_PING_MIN < 0.0) {
      return td::Status::Error(PSTRING() << "invalid ping value " << arg);
    }
    return td::Status::OK();
  });
  p.add_checked_option('\0', "net-ping-max", "network ping (minimal)", [&](td::Slice arg) {
    NET_PING_MAX = td::to_double(arg);
    if (NET_PING_MAX < 0.0) {
      return td::Status::Error(PSTRING() << "invalid ping value " << arg);
    }
    return td::Status::OK();
  });
  p.add_checked_option('\0', "net-loss", "packet loss probability", [&](td::Slice arg) {
    NET_LOSS = td::to_double(arg);
    if (NET_LOSS < 0.0 || NET_LOSS > 1.0) {
      return td::Status::Error(PSTRING() << "invalid loss value " << arg);
    }
    return td::Status::OK();
  });
  p.run(argc, argv).ensure();
  CHECK(NET_PING_MIN <= NET_PING_MAX);
  CHECK(N_DOUBLE_NODES <= N_NODES);

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
