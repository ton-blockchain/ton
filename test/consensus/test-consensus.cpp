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
#include "consensus/utils.h"
#include "td/actor/coro_utils.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Random.h"
#include "td/utils/port/signals.h"

#include "block-auto.h"

using namespace ton;
using namespace ton::validator;
using namespace ton::validator::consensus;

namespace {
td::Bits256 from_hex(td::Slice s) {
  td::Bits256 x;
  CHECK(x.from_hex(s) == 256);
  return x;
}

td::Ref<vm::Cell> gen_shard_state(BlockSeqno seqno) {
  return vm::CellBuilder().store_long(0xabcdabcdU, 32).store_long(seqno, 32).finalize_novm();
}

CatchainSeqno CC_SEQNO = 123;
BlockIdExt MIN_MC_BLOCK_ID{masterchainId, shardIdAll, 0,
                           from_hex("AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDDAAAAAAAABBBBBBBBCCCCCCCCDDDDDDDD"),
                           from_hex("0123456012345601234560123456012345601234560123456777777701234567")};
td::Bits256 SESSION_ID = from_hex("00001234000012340000123400001234aaaaaaaabbbbbbbbcccccccceeeeeeee");

ShardIdFull SHARD{basechainId, shardIdAll};
BlockIdExt FIRST_PARENT{basechainId, shardIdAll, 0, td::Bits256(gen_shard_state(0)->get_hash().bits()),
                        from_hex("89abcde89abcde89abcde89abcde89abcde89abcde89abcdefffffff89abcdef")};

double NET_PING_MIN = 0.05;
double NET_PING_MAX = 0.1;
double NET_LOSS = 0.0;

size_t N_NODES = 8;
size_t N_DOUBLE_NODES = 0;

double DURATION = 60.0;
td::uint32 TARGET_RATE_MS = 1000;

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
  td::actor::Task<td::BufferSlice> send_query(PeerValidator src, size_t dst_idx, td::BufferSlice message);

 private:
  std::vector<std::vector<td::actor::ActorId<TestOverlayNode>>> nodes_;

  td::actor::Task<> before_receive(size_t src_idx, size_t dst_idx, bool no_loss) {
    if (!no_loss && td::Random::fast(0.0, 1.0) < NET_LOSS) {
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

  template <>
  td::actor::Task<ProtocolMessage> process(BusHandle bus, std::shared_ptr<OutgoingOverlayRequest> message) {
    auto [task, promise] = td::actor::StartedTask<ProtocolMessage>::make_bridge();
    auto promise_ptr = std::make_shared<td::Promise<ProtocolMessage>>(std::move(promise));
    process_query_inner1(bus, message, promise_ptr).start().detach();
    process_query_inner2(bus, message, promise_ptr).start().detach();
    co_return co_await std::move(task);
  }

  td::actor::Task<> process_query_inner1(BusHandle bus, std::shared_ptr<OutgoingOverlayRequest> message,
                                         std::shared_ptr<td::Promise<ProtocolMessage>> promise_ptr) {
    if (message->timeout) {
      co_await td::actor::coro_sleep(message->timeout);
      if (*promise_ptr) {
        promise_ptr->set_error(td::Status::Error(ErrorCode::timeout, "timeout"));
      }
    }
    co_return {};
  }

  td::actor::Task<> process_query_inner2(BusHandle bus, std::shared_ptr<OutgoingOverlayRequest> message,
                                         std::shared_ptr<td::Promise<ProtocolMessage>> promise_ptr) {
    auto r_response = co_await td::actor::ask(test_overlay, &TestOverlay::send_query, bus->local_id,
                                              message->destination.value(), message->request.data.clone())
                          .wrap();
    if (r_response.is_ok() && *promise_ptr) {
      td::BufferSlice response = r_response.move_as_ok();
      if (fetch_tl_object<ton_api::consensus_requestError>(response, true).is_ok()) {
        promise_ptr->set_error(td::Status::Error("Peer returned an error"));
      } else {
        promise_ptr->set_value(ProtocolMessage{std::move(response)});
      }
    }
    co_return {};
  }

  void receive_message(PeerValidator src, td::BufferSlice data) {
    owning_bus().publish<IncomingProtocolMessage>(src.idx, std::move(data));
  }

  void receive_candidate(RawCandidateRef candidate) {
    owning_bus().publish<CandidateReceived>(candidate);
  }

  td::actor::Task<td::BufferSlice> receive_query(PeerValidator src, td::BufferSlice query) {
    auto request = std::make_shared<IncomingOverlayRequest>(src.idx, std::move(query));
    auto response = co_await owning_bus().publish(std::move(request)).wrap();
    if (response.is_ok()) {
      co_return std::move(response.move_as_ok().data);
    }
    co_return create_serialize_tl_object<ton_api::consensus_requestError>();
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

td::actor::Task<td::BufferSlice> TestOverlay::send_query(PeerValidator src, size_t dst_idx, td::BufferSlice message) {
  if (nodes_[dst_idx].empty()) {
    co_return td::Status::Error("no instances");
  }
  const auto &instance = nodes_[dst_idx][td::Random::fast(0, (int)nodes_[dst_idx].size() - 1)];
  co_await before_receive(src.idx.value(), dst_idx, true);
  auto response = co_await td::actor::ask(instance, &TestOverlayNode::receive_query, src, std::move(message));
  co_await before_receive(src.idx.value(), dst_idx, true);
  co_return response;
}

class TestConsensus;

class TestManagerFacade : public ManagerFacade {
 public:
  explicit TestManagerFacade(size_t node_idx, size_t instance_idx, td::actor::ActorId<TestConsensus> test_consensus)
      : node_idx_(node_idx), instance_idx_(instance_idx), test_consensus_(test_consensus) {
  }

  td::actor::Task<GeneratedCandidate> collate_block(CollateParams params,
                                                    td::CancellationToken cancellation_token) override {
    CHECK(params.prev.size() == 1);
    uint32_t prev_seqno = params.prev[0].seqno();
    LOG(WARNING) << "Collate block #" << prev_seqno + 1;
    CHECK(params.shard == SHARD);
    CHECK(params.min_masterchain_block_id == MIN_MC_BLOCK_ID);

    CHECK(params.prev_block_state_roots.size() == 1 &&
          params.prev_block_state_roots[0]->get_hash() == gen_shard_state(prev_seqno)->get_hash());
    if (prev_seqno != 0) {
      CHECK(params.prev_block_data.size() == 1 && params.prev_block_data[0]->block_id() == params.prev[0]);
    }

    td::Bits256 rand_data;
    td::Random::secure_bytes(rand_data.as_slice());
    td::Ref<vm::Cell> block_info = vm::CellBuilder{}.store_bytes(rand_data.as_slice()).finalize_novm();
    td::Ref<vm::Cell> value_flow = vm::CellBuilder{}.finalize_novm();
    td::Ref<vm::Cell> merkle_update =
        vm::CellBuilder::create_merkle_update(gen_shard_state(prev_seqno), gen_shard_state(prev_seqno + 1));
    td::Ref<vm::Cell> block_extra = vm::CellBuilder{}.finalize_novm();
    td::Ref<vm::Cell> block_root = vm::CellBuilder{}
                                       .store_long(0x11ef55aa, 32)
                                       .store_long(-111, 32)
                                       .store_ref(block_info)
                                       .store_ref(value_flow)
                                       .store_ref(merkle_update)
                                       .store_ref(block_extra)
                                       .finalize_novm();
    td::BufferSlice data = vm::std_boc_serialize(block_root, 31).move_as_ok();

    std::vector<td::Ref<vm::Cell>> collated_roots;
    // consensus_extra_data#638eb292 flags:# gen_utime_ms:uint64 = ConsensusExtraData;
    auto cell = vm::CellBuilder{}
                    .store_long(0x638eb292, 32)
                    .store_long(0, 32)
                    .store_long((td::uint64)(td::Clocks::system() * 1000.0), 64)
                    .finalize_novm();
    collated_roots.push_back(std::move(cell));
    td::BufferSlice collated_data = co_await vm::std_boc_serialize_multi(collated_roots, 2);

    BlockCandidate candidate(
        params.creator,
        BlockIdExt(BlockId(params.shard, prev_seqno + 1), block_root->get_hash().bits(), td::sha256_bits256(data)),
        td::sha256_bits256(collated_data), data.clone(), collated_data.clone());
    co_return GeneratedCandidate{.candidate = std::move(candidate), .is_cached = false, .self_collated = true};
  }

  td::actor::Task<ValidateCandidateResult> validate_block_candidate(BlockCandidate candidate, ValidateParams params,
                                                                    td::Timestamp timeout) override {
    CHECK(params.prev.size() == 1);
    uint32_t prev_seqno = params.prev[0].seqno();
    LOG(WARNING) << "Validate block #" << candidate.id.seqno();
    CHECK(params.prev[0].shard_full() == SHARD);
    CHECK(candidate.id.shard_full() == SHARD);
    CHECK(candidate.id.seqno() == prev_seqno + 1);
    CHECK(params.prev_block_state_roots.size() == 1 &&
          params.prev_block_state_roots[0]->get_hash() == gen_shard_state(prev_seqno)->get_hash());
    co_return CandidateAccept{.ok_from_utime = co_await get_candidate_gen_utime_exact(candidate)};
  }

  td::actor::Task<> accept_block(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                 td::Ref<block::BlockSignatureSet> signatures, int send_broadcast_mode,
                                 bool apply) override;

  td::actor::Task<td::Ref<vm::Cell>> wait_block_state_root(BlockIdExt block_id, td::Timestamp timeout) override;
  td::actor::Task<td::Ref<BlockData>> wait_block_data(BlockIdExt block_id, td::Timestamp timeout) override;

 private:
  size_t node_idx_;
  size_t instance_idx_;
  td::actor::ActorId<TestConsensus> test_consensus_;
};

class TestSimplexBus : public simplex::Bus {
 public:
  using Parent = simplex::Bus;

  explicit TestSimplexBus(td::Promise<td::Unit> stop_promise) : stop_promise_(std::move(stop_promise)) {
  }
  ~TestSimplexBus() override {
    stop_promise_.set_value(td::Unit{});
  }

 private:
  td::Promise<td::Unit> stop_promise_;
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

  td::actor::Task<> on_block_accepted(size_t node_idx, size_t instance_idx, td::Ref<BlockData> block,
                                      td::Ref<block::BlockSignatureSet> signatures) {
    BlockIdExt block_id = block->block_id();
    if (signatures->is_final()) {
      signatures->check_signatures(validator_set_, block_id).ensure();
    } else {
      CHECK(!SHARD.is_masterchain());
      signatures->check_approve_signatures(validator_set_, block_id).ensure();
    }
    BlockSeqno seqno = block_id.seqno();
    if (accepted_blocks_.contains(seqno)) {
      LOG_CHECK(accepted_blocks_[seqno]->block_id() == block_id) << "Accepted different blocks for seqno " << seqno;
    } else {
      accepted_blocks_[seqno] = block;

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

  td::actor::Task<td::Ref<vm::Cell>> wait_block_state_root(BlockIdExt block_id) {
    if (block_id == FIRST_PARENT) {
      co_return gen_shard_state(block_id.seqno());
    }
    auto it = accepted_blocks_.find(block_id.seqno());
    CHECK(it != accepted_blocks_.end());
    CHECK(it->second->block_id() == block_id);
    co_return gen_shard_state(block_id.seqno());
  }

  td::actor::Task<td::Ref<BlockData>> wait_block_data(BlockIdExt block_id) {
    CHECK(block_id != FIRST_PARENT);
    auto it = accepted_blocks_.find(block_id.seqno());
    CHECK(it != accepted_blocks_.end());
    CHECK(it->second->block_id() == block_id);
    co_return it->second;
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
        auto [stop_task, stop_promise] = td::actor::StartedTask<>::make_bridge();
        auto bus = std::make_shared<TestSimplexBus>(std::move(stop_promise));
        inst.stop_waiter = std::make_unique<td::actor::StartedTask<>>(std::move(stop_task));
        bus->shard = SHARD;
        bus->manager = inst.manager_facade.get();
        bus->keyring = keyring_.get();
        bus->validator_opts = ValidatorManagerOptions::create(BlockIdExt{}, BlockIdExt{});
        bus->validator_set = validators;
        bus->total_weight = total_weight;
        bus->local_id = validators[idx];
        bus->config = NewConsensusConfig{.target_rate_ms = TARGET_RATE_MS,
                                         .max_block_size = 1 << 20,
                                         .max_collated_data_size = 1 << 20,
                                         .consensus = NewConsensusConfig::Simplex{}};
        bus->simplex_config = bus->config.consensus.get<NewConsensusConfig::Simplex>();
        bus->min_masterchain_block_id = MIN_MC_BLOCK_ID;
        bus->session_id = SESSION_ID;
        bus->first_block_parents = {FIRST_PARENT};
        bus->cc_seqno = CC_SEQNO;
        bus->validator_set_hash = validator_set_->get_validator_set_hash();
        bus->populate_collator_schedule();
        inst.bus =
            runtime.start(std::static_pointer_cast<simplex::Bus>(bus), PSTRING() << "consensus." << idx << "." << i);
        node.instances.push_back(std::move(inst));
      }
    }

    co_await td::actor::coro_sleep(td::Timestamp::in(DURATION));

    co_return co_await finalize();
  }

  td::actor::Task<> finalize() {
    LOG(WARNING) << "TEST FINISHED";
    for (Node &node : nodes_) {
      for (Instance &inst : node.instances) {
        inst.bus.publish<StopRequested>();
        inst.bus = {};
      }
    }
    for (Node &node : nodes_) {
      for (Instance &inst : node.instances) {
        co_await std::move(*inst.stop_waiter);
      }
    }
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
    std::unique_ptr<td::actor::StartedTask<>> stop_waiter;

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

  std::map<BlockSeqno, td::Ref<BlockData>> accepted_blocks_;
};

td::actor::Task<> TestManagerFacade::accept_block(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                                  td::Ref<block::BlockSignatureSet> signatures, int send_broadcast_mode,
                                                  bool apply) {
  CHECK(id.shard_full() == SHARD);
  LOG(WARNING) << "Accept block #" << id.seqno() << " (" << (signatures->is_final() ? "final" : "notarize")
               << " signatures)";
  CHECK(id == data->block_id());
  td::actor::ask(test_consensus_, &TestConsensus::on_block_accepted, node_idx_, instance_idx_, data, signatures)
      .detach();
  co_return td::Unit{};
}

td::actor::Task<td::Ref<vm::Cell>> TestManagerFacade::wait_block_state_root(BlockIdExt block_id,
                                                                            td::Timestamp timeout) {
  co_return co_await td::actor::ask(test_consensus_, &TestConsensus::wait_block_state_root, block_id);
}

td::actor::Task<td::Ref<BlockData>> TestManagerFacade::wait_block_data(BlockIdExt block_id, td::Timestamp timeout) {
  co_return co_await td::actor::ask(test_consensus_, &TestConsensus::wait_block_data, block_id);
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
  p.add_checked_option('d', "duration", "test duration in seconds (default: 60)", [&](td::Slice arg) {
    DURATION = td::to_double(arg);
    if (DURATION < 0.0) {
      return td::Status::Error(PSTRING() << "invalid duration value " << arg);
    }
    return td::Status::OK();
  });
  p.add_option('m', "masterchain", "masterchain consensus (default is shardchain)", [&]() {
    SHARD = ShardIdFull{masterchainId};
    FIRST_PARENT.id.workchain = masterchainId;
    FIRST_PARENT.id.shard = shardIdAll;
    MIN_MC_BLOCK_ID = FIRST_PARENT;
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
  p.add_checked_option('\0', "target-rate-ms", "target block rate in milliseconds (default: 1000)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(TARGET_RATE_MS, td::to_integer_safe<td::uint32>(arg));
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
