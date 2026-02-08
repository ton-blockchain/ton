/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "adnl/utils.hpp"
#include "auto/tl/ton_api.h"
#include "block/block.h"
#include "block/validator-set.h"
#include "consensus/runtime.h"
#include "consensus/simplex/bus.h"
#include "consensus/utils.h"
#include "td/actor/coro_utils.h"
#include "td/db/MemoryKeyValue.h"
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

td::Result<std::pair<double, double>> parse_range(td::Slice s) {
  auto pos = s.find(':');
  if (pos == td::Slice::npos) {
    double x = td::to_double(s);
    return std::make_pair(x, x);
  }
  double x = td::to_double(s.substr(0, pos));
  double y = td::to_double(s.substr(pos + 1, s.size()));
  if (x > y) {
    return td::Status::Error(PSTRING() << "invalid range " << s);
  }
  return std::make_pair(x, y);
}

template <typename T>
td::Result<std::pair<T, T>> parse_int_range(td::Slice s) {
  auto pos = s.find(':');
  if (pos == td::Slice::npos) {
    TRY_RESULT(x, td::to_integer_safe<T>(s));
    return std::make_pair(x, x);
  }
  TRY_RESULT(x, td::to_integer_safe<T>(s.substr(0, pos)));
  TRY_RESULT(y, td::to_integer_safe<T>(s.substr(pos + 1, s.size())));
  if (x > y) {
    return td::Status::Error(PSTRING() << "invalid range " << s);
  }
  return std::make_pair(x, y);
}

Ref<vm::Cell> make_ext_blk_ref(BlockIdExt block_id, LogicalTime lt) {
  vm::CellBuilder cb;
  cb.store_long_bool(lt, 64);
  cb.store_long_bool(block_id.seqno(), 32);
  cb.store_bits_bool(block_id.root_hash);
  cb.store_bits_bool(block_id.file_hash);
  return cb.finalize_novm();
}

CatchainSeqno CC_SEQNO = 123;
BlockIdExt MIN_MC_BLOCK_ID{masterchainId, shardIdAll, 0,
                           from_hex("AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDDAAAAAAAABBBBBBBBCCCCCCCCDDDDDDDD"),
                           from_hex("0123456012345601234560123456012345601234560123456777777701234567")};
td::Bits256 SESSION_ID = from_hex("00001234000012340000123400001234aaaaaaaabbbbbbbbcccccccceeeeeeee");

ShardIdFull SHARD{basechainId, shardIdAll};
BlockIdExt FIRST_PARENT{basechainId, shardIdAll, 0, td::Bits256(gen_shard_state(0)->get_hash().bits()),
                        from_hex("89abcde89abcde89abcde89abcde89abcde89abcde89abcdefffffff89abcdef")};

std::pair<double, double> NET_PING = {0.05, 0.1};
double NET_LOSS = 0.0;

size_t N_NODES = 8;
size_t N_DOUBLE_NODES = 0;

double DURATION = 60.0;
td::uint32 TARGET_RATE_MS = 1000;
td::uint32 SLOTS_PER_LEADER_WINDOW = 4;

std::pair<double, double> GREMLIN_PERIOD = {-1.0, -1.0};
std::pair<double, double> GREMLIN_DOWNTIME = {1.0, 1.0};
std::pair<size_t, size_t> GREMLIN_N = {1, 1};
size_t GREMLIN_TIMES = 1000000000;
bool GREMLIN_KILLS_LEADER = false;

std::pair<double, double> NET_GREMLIN_PERIOD = {-1.0, -1.0};
std::pair<double, double> NET_GREMLIN_DOWNTIME = {10.0, 10.0};
std::pair<size_t, size_t> NET_GREMLIN_N = {1, 1};
size_t NET_GREMLIN_TIMES = 1000000000;
bool NET_GREMLIN_KILLS_LEADER = false;

std::pair<double, double> DB_DELAY = {0.0, 0.0};
std::pair<double, double> COLLATION_TIME = {0.0, 0.0};
std::pair<double, double> VALIDATION_TIME = {0.0, 0.0};

class TestSimplexBus : public simplex::Bus {
 public:
  using Parent = simplex::Bus;
  size_t instance_idx = 0;
};

class TestOverlayNode;

class TestOverlay : public td::actor::Actor {
 public:
  void register_node(size_t idx, size_t instance_idx, td::actor::ActorId<TestOverlayNode> node) {
    Instance &inst = get_inst(idx, instance_idx);
    CHECK(inst.actor.empty());
    inst.actor = std::move(node);
  }

  void unregister_node(size_t idx, size_t instance_idx) {
    Instance &inst = get_inst(idx, instance_idx);
    CHECK(!inst.actor.empty());
    inst.actor = {};
  }

  td::actor::Task<> set_instance_disabled(size_t idx, size_t instance_idx, bool value) {
    get_inst(idx, instance_idx).disabled = value;
    LOG(ERROR) << "Node #" << idx << "." << instance_idx << ": " << (value ? "disable" : "enable") << " network";
    co_return td::Unit{};
  }

  td::actor::Task<> send_message(PeerValidator src, size_t src_instance_idx, size_t dst_idx, td::BufferSlice message);
  td::actor::Task<> send_candidate(PeerValidator src, size_t src_instance_idx, size_t dst_idx, CandidateRef candidate);
  td::actor::Task<td::BufferSlice> send_query(PeerValidator src, size_t src_instance_idx, size_t dst_idx,
                                              td::BufferSlice message);

 private:
  struct Instance {
    td::actor::ActorId<TestOverlayNode> actor;
    bool disabled = false;
  };
  std::vector<std::vector<Instance>> nodes_;

  Instance &get_inst(size_t idx, size_t instance_idx) {
    if (nodes_.size() <= idx) {
      nodes_.resize(idx + 1);
    }
    if (nodes_[idx].size() <= instance_idx) {
      nodes_[idx].resize(instance_idx + 1);
    }
    return nodes_[idx][instance_idx];
  }

  td::actor::Task<> before_receive(size_t src_idx, size_t src_instance_idx, size_t dst_idx, bool no_loss) {
    if (get_inst(src_idx, src_instance_idx).disabled) {
      co_return td::Status::Error("src is disabled");
    }
    if (!no_loss && td::Random::fast(0.0, 1.0) < NET_LOSS) {
      co_return td::Status::Error("packet lost");
    }
    co_await td::actor::coro_sleep(td::Timestamp::in(td::Random::fast(NET_PING.first, NET_PING.second)));
    co_return td::Unit{};
  }
};

td::actor::ActorOwn<TestOverlay> test_overlay;

class TestOverlayNode : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    instance_idx_ = dynamic_cast<const TestSimplexBus &>(*owning_bus()).instance_idx;
    td::actor::send_closure(test_overlay, &TestOverlay::register_node, owning_bus()->local_id.idx.value(),
                            instance_idx_, actor_id(this));
  }

  void tear_down() override {
    td::actor::send_closure(test_overlay, &TestOverlay::unregister_node, owning_bus()->local_id.idx.value(),
                            instance_idx_);
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const OutgoingProtocolMessage> message) {
    if (message->recipient.has_value()) {
      CHECK(message->recipient.value() != bus->local_id.idx);
      td::actor::ask(test_overlay, &TestOverlay::send_message, bus->local_id, instance_idx_,
                     message->recipient->value(), message->message.data.clone())
          .detach_silent();
    } else {
      for (size_t i = 0; i < bus->validator_set.size(); ++i) {
        if (bus->local_id.idx.value() != i) {
          td::actor::ask(test_overlay, &TestOverlay::send_message, bus->local_id, instance_idx_, i,
                         message->message.data.clone())
              .detach_silent();
        }
      }
    }
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const CandidateGenerated> event) {
    for (size_t i = 0; i < bus->validator_set.size(); ++i) {
      if (bus->local_id.idx.value() != i) {
        td::actor::ask(test_overlay, &TestOverlay::send_candidate, bus->local_id, instance_idx_, i, event->candidate)
            .detach_silent();
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
    auto r_response = co_await td::actor::ask(test_overlay, &TestOverlay::send_query, bus->local_id, instance_idx_,
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

  void receive_candidate(CandidateRef candidate) {
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

 private:
  size_t instance_idx_ = 0;
};

td::actor::Task<> TestOverlay::send_message(PeerValidator src, size_t src_instance_idx, size_t dst_idx,
                                            td::BufferSlice message) {
  co_await before_receive(src.idx.value(), src_instance_idx, dst_idx, false);
  for (const auto &instance : nodes_[dst_idx]) {
    if (instance.actor.empty() || instance.disabled) {
      continue;
    }
    td::actor::send_closure(instance.actor, &TestOverlayNode::receive_message, src, message.clone());
  }
  co_return td::Unit{};
}

td::actor::Task<> TestOverlay::send_candidate(PeerValidator src, size_t src_instance_idx, size_t dst_idx,
                                              CandidateRef candidate) {
  co_await before_receive(src.idx.value(), src_instance_idx, dst_idx, true);
  for (const auto &instance : nodes_[dst_idx]) {
    if (instance.actor.empty() || instance.disabled) {
      continue;
    }
    td::actor::send_closure(instance.actor, &TestOverlayNode::receive_candidate, candidate);
  }
  co_return td::Unit{};
}

td::actor::Task<td::BufferSlice> TestOverlay::send_query(PeerValidator src, size_t src_instance_idx, size_t dst_idx,
                                                         td::BufferSlice message) {
  if (nodes_[dst_idx].empty()) {
    co_return td::Status::Error("no instances");
  }
  auto dst_instance_idx = (size_t)td::Random::fast(0, (int)nodes_[dst_idx].size() - 1);
  const auto &instance = nodes_[dst_idx][dst_instance_idx];
  co_await before_receive(src.idx.value(), src_instance_idx, dst_idx, true);
  if (instance.actor.empty() || instance.disabled) {
    co_return td::Status::Error("instance is stopped/disabled");
  }
  auto response = co_await td::actor::ask(instance.actor, &TestOverlayNode::receive_query, src, std::move(message));
  co_await before_receive(dst_idx, dst_instance_idx, src.idx.value(), true);
  co_return response;
}

class TestConsensus;

class CandidateStorage : public td::actor::Actor {
 public:
  td::actor::Task<BlockCandidate> load_block_candidate(PublicKey source, BlockIdExt block_id,
                                                       FileHash collated_data_hash) {
    auto it = candidates_.find({source.ed25519_value().raw(), block_id, collated_data_hash});
    if (it == candidates_.end()) {
      co_return td::Status::Error("no candidate in db");
    }
    co_return it->second.clone();
  }

  td::actor::Task<> store_block_candidate(BlockCandidate candidate) {
    std::tuple key{candidate.pubkey.as_bits256(), candidate.id, candidate.collated_file_hash};
    candidates_.emplace(key, std::move(candidate));
    co_return {};
  }

 private:
  std::map<std::tuple<td::Bits256, BlockIdExt, FileHash>, BlockCandidate> candidates_;
};

class TestManagerFacade : public ManagerFacade {
 public:
  explicit TestManagerFacade(size_t node_idx, size_t instance_idx, Ref<block::ValidatorSet> validator_set,
                             td::actor::ActorId<TestConsensus> test_consensus,
                             td::actor::ActorId<CandidateStorage> candidate_storage)
      : node_idx_(node_idx)
      , instance_idx_(instance_idx)
      , validator_set_(validator_set)
      , test_consensus_(test_consensus)
      , candidate_storage_(candidate_storage) {
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
    double gen_utime = td::Clocks::system();

    block::gen::BlockInfo::Record info;
    info.version = 0;
    info.not_master = !SHARD.is_masterchain();
    info.after_merge = info.before_split = info.after_split = false;
    info.want_split = info.want_merge = false;
    info.key_block = info.vert_seqno_incr = false;
    info.flags = 0;
    info.seq_no = prev_seqno + 1;
    info.vert_seq_no = 0;

    vm::CellBuilder cb;
    block::ShardId{SHARD}.serialize(cb);
    info.shard = cb.as_cellslice_ref();

    info.gen_utime = (UnixTime)gen_utime;
    info.start_lt = (LogicalTime)info.seq_no * 1000;
    info.end_lt = (LogicalTime)info.seq_no * 1000 + 1;
    info.gen_validator_list_hash_short = validator_set_->get_validator_set_hash();
    info.gen_catchain_seqno = validator_set_->get_catchain_seqno();
    info.min_ref_mc_seqno = MIN_MC_BLOCK_ID.seqno();
    info.prev_key_block_seqno = MIN_MC_BLOCK_ID.seqno();
    if (!SHARD.is_masterchain()) {
      info.master_ref = make_ext_blk_ref(MIN_MC_BLOCK_ID, 0);
    }
    info.prev_ref = make_ext_blk_ref(params.prev[0], (LogicalTime)prev_seqno * 1000 + 1);
    td::Ref<vm::Cell> block_info;
    CHECK(block::gen::pack_cell(block_info, info));

    td::Ref<vm::Cell> value_flow = vm::CellBuilder{}.finalize_novm();
    td::Ref<vm::Cell> merkle_update =
        vm::CellBuilder::create_merkle_update(gen_shard_state(prev_seqno), gen_shard_state(prev_seqno + 1));

    td::Bits256 rand_data;
    td::Random::secure_bytes(rand_data.as_slice());
    td::Ref<vm::Cell> block_extra = vm::CellBuilder{}.store_bytes(rand_data.as_slice()).finalize_novm();

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
                    .store_long((td::uint64)(gen_utime * 1000.0), 64)
                    .finalize_novm();
    collated_roots.push_back(std::move(cell));
    td::BufferSlice collated_data = co_await vm::std_boc_serialize_multi(collated_roots, 2);

    co_await td::actor::coro_sleep(td::Timestamp::in(td::Random::fast(COLLATION_TIME.first, COLLATION_TIME.second)));

    BlockCandidate candidate(
        params.creator,
        BlockIdExt(BlockId(params.shard, prev_seqno + 1), block_root->get_hash().bits(), td::sha256_bits256(data)),
        td::sha256_bits256(collated_data), data.clone(), collated_data.clone());
    if (!params.skip_store_candidate) {
      co_await store_block_candidate(candidate.clone());
    }
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
    co_await td::actor::coro_sleep(td::Timestamp::in(td::Random::fast(VALIDATION_TIME.first, VALIDATION_TIME.second)));
    co_await store_block_candidate(candidate.clone());
    co_return CandidateAccept{.ok_from_utime = co_await get_candidate_gen_utime_exact(candidate)};
  }

  td::actor::Task<> accept_block(BlockIdExt id, td::Ref<BlockData> data, size_t creator_idx,
                                 td::Ref<block::BlockSignatureSet> signatures, int send_broadcast_mode,
                                 bool apply) override;

  td::actor::Task<td::Ref<vm::Cell>> wait_block_state_root(BlockIdExt block_id, td::Timestamp timeout) override;
  td::actor::Task<td::Ref<BlockData>> wait_block_data(BlockIdExt block_id, td::Timestamp timeout) override;

  td::actor::Task<BlockCandidate> load_block_candidate(PublicKey source, BlockIdExt block_id,
                                                       FileHash collated_data_hash) override {
    co_return co_await td::actor::ask(candidate_storage_, &CandidateStorage::load_block_candidate, source, block_id,
                                      collated_data_hash);
  }

  td::actor::Task<> store_block_candidate(BlockCandidate candidate) override {
    candidate.out_msg_queue_proof_broadcasts = {};
    co_return co_await td::actor::ask(candidate_storage_, &CandidateStorage::store_block_candidate,
                                      std::move(candidate));
  }

 private:
  size_t node_idx_;
  size_t instance_idx_;
  Ref<block::ValidatorSet> validator_set_;
  td::actor::ActorId<TestConsensus> test_consensus_;
  td::actor::ActorId<CandidateStorage> candidate_storage_;
};

class TestDbImpl : public consensus::Db {
 public:
  struct DbInner {
    std::map<td::BufferSlice, td::BufferSlice> map;
    std::mutex mutex;
  };

  explicit TestDbImpl(std::shared_ptr<DbInner> db) : db_(std::move(db)) {
    std::scoped_lock lock(db_->mutex);
    for (auto &[key, value] : db_->map) {
      snapshot_.emplace(key.clone(), value.clone());
    }
  }
  ~TestDbImpl() override = default;

  void disable() {
    std::scoped_lock lock(db_->mutex);
    disabled_ = true;
  }

  std::optional<td::BufferSlice> get(td::Slice key) const override {
    auto it = snapshot_.find(td::BufferSlice{key});
    if (it == snapshot_.end()) {
      return std::nullopt;
    }
    return it->second.clone();
  }
  std::vector<std::pair<td::BufferSlice, td::BufferSlice>> get_by_prefix(td::uint32 prefix) const override {
    std::vector<std::pair<td::BufferSlice, td::BufferSlice>> result;
    td::BufferSlice begin{(const char *)&prefix, 4};
    td::uint32 prefix2 = prefix + 1;
    td::BufferSlice end{(const char *)&prefix2, 4};
    for (auto it = snapshot_.lower_bound(begin); it != snapshot_.end() && it->first < end; ++it) {
      result.emplace_back(it->first.clone(), it->second.clone());
    }
    return result;
  }
  td::actor::Task<> set(td::BufferSlice key, td::BufferSlice value) override {
    co_await td::actor::coro_sleep(td::Timestamp::in(td::Random::fast(DB_DELAY.first, DB_DELAY.second)));
    std::scoped_lock lock(db_->mutex);
    if (disabled_) {
      co_return td::Status::Error("db is disabled");
    }
    db_->map[std::move(key)] = std::move(value);
    co_return {};
  }

 private:
  std::map<td::BufferSlice, td::BufferSlice> snapshot_;
  std::shared_ptr<DbInner> db_;
  bool disabled_ = false;
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
                                      size_t creator_idx, td::Ref<block::BlockSignatureSet> signatures) {
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
    }
    Instance &inst = nodes_[node_idx].instances[instance_idx];
    inst.last_accepted_block = std::max(inst.last_accepted_block, seqno);
    if (last_accepted_block_.seqno() < seqno && signatures->is_final()) {
      last_accepted_block_ = block_id;
      last_accepted_block_leader_idx_ = creator_idx;
      for (Node &node : nodes_) {
        for (Instance &inst : node.instances) {
          if (inst.status == Instance::Running) {
            inst.bus.publish<BlockFinalizedInMasterchain>(block_id);
          }
        }
      }
    }
    co_return td::Unit{};
  }

  td::actor::Task<> wait_block_accepted(BlockIdExt block_id) {
    if (block_id == FIRST_PARENT) {
      co_return {};
    }
    td::Timestamp timeout = td::Timestamp::in(10.0);
    while (!timeout.is_in_past()) {
      auto it = accepted_blocks_.find(block_id.seqno());
      if (it != accepted_blocks_.end() && it->second->block_id() == block_id) {
        co_return {};
      }
      co_await td::actor::coro_sleep(td::Timestamp::in(0.1));
    }
    co_return td::Status::Error(ErrorCode::timeout, "timeout");
  }

  td::actor::Task<td::Ref<vm::Cell>> wait_block_state_root(BlockIdExt block_id) {
    co_await wait_block_accepted(block_id);
    co_return gen_shard_state(block_id.seqno());
  }

  td::actor::Task<td::Ref<BlockData>> wait_block_data(BlockIdExt block_id) {
    CHECK(block_id != FIRST_PARENT);
    co_await wait_block_accepted(block_id);
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
    for (size_t idx = 0; idx < nodes_.size(); ++idx) {
      Node &node = nodes_[idx];
      validator_descrs.push_back(ValidatorDescr(Ed25519_PublicKey{node.public_key.ed25519_value().raw()}, node.weight,
                                                node.adnl_id.bits256_value()));
      validators_.push_back(PeerValidator{.idx = PeerValidatorId((int)idx),
                                          .key = node.public_key,
                                          .short_id = node.node_id,
                                          .adnl_id = node.adnl_id,
                                          .weight = node.weight});
      total_weight_ += node.weight;
    }
    validator_set_ = td::Ref<block::ValidatorSet>{true, CC_SEQNO, SHARD, std::move(validator_descrs)};

    test_overlay = td::actor::create_actor<TestOverlay>("test-overlay");

    for (size_t idx = 0; idx < N_NODES; ++idx) {
      Node &node = nodes_[idx];
      size_t n_instances = idx < N_DOUBLE_NODES ? 2 : 1;
      for (size_t i = 0; i < n_instances; ++i) {
        Instance inst;
        inst.db_inner = std::make_shared<TestDbImpl::DbInner>();
        inst.candidate_storage =
            td::actor::create_actor<CandidateStorage>(PSTRING() << "ManagerFacade." << idx << "." << i);
        node.instances.push_back(std::move(inst));
      }
    }

    for (size_t idx = 0; idx < N_NODES; ++idx) {
      for (size_t i = 0; i < nodes_[idx].instances.size(); ++i) {
        start_instance(idx, i);
      }
    }

    if (GREMLIN_PERIOD.first >= 0.0) {
      run_gremlin().start().detach();
    }
    if (NET_GREMLIN_PERIOD.first >= 0.0) {
      run_net_gremlin().start().detach();
    }

    co_await td::actor::coro_sleep(td::Timestamp::in(DURATION));

    co_return co_await finalize();
  }

  void start_instance(size_t node_idx, size_t instance_idx) {
    Node &node = nodes_[node_idx];
    Instance &inst = node.instances[instance_idx];
    CHECK(inst.status == Instance::Stopped);
    auto &runtime = inst.runtime;
    BlockAccepter::register_in(runtime);
    BlockProducer::register_in(runtime);
    BlockValidator::register_in(runtime);
    runtime.register_actor<TestOverlayNode>("PrivateOverlay");
    simplex::CandidateResolver::register_in(runtime);
    simplex::Consensus::register_in(runtime);
    simplex::Pool::register_in(runtime);

    inst.manager_facade = td::actor::create_actor<TestManagerFacade>(
        PSTRING() << "ManagerFacade." << node_idx << "." << instance_idx, node_idx, instance_idx, validator_set_,
        actor_id(this), inst.candidate_storage.get());
    auto [stop_task, stop_promise] = td::actor::StartedTask<>::make_bridge();
    auto bus = std::make_shared<TestSimplexBus>();
    inst.stop_waiter = std::move(stop_task);
    bus->instance_idx = instance_idx;
    bus->stop_promise = std::move(stop_promise);
    bus->shard = SHARD;
    bus->manager = inst.manager_facade.get();
    bus->keyring = keyring_.get();
    bus->validator_opts = ValidatorManagerOptions::create(BlockIdExt{}, BlockIdExt{});
    bus->validator_set = validators_;
    bus->total_weight = total_weight_;
    bus->local_id = validators_[node_idx];
    bus->config = NewConsensusConfig{
        .target_rate_ms = TARGET_RATE_MS,
        .max_block_size = 1 << 20,
        .max_collated_data_size = 1 << 20,
        .consensus = NewConsensusConfig::Simplex{.slots_per_leader_window = SLOTS_PER_LEADER_WINDOW}};
    bus->simplex_config = bus->config.consensus.get<NewConsensusConfig::Simplex>();
    bus->session_id = SESSION_ID;
    bus->cc_seqno = CC_SEQNO;
    bus->validator_set_hash = validator_set_->get_validator_set_hash();
    bus->populate_collator_schedule();
    bus->db = std::make_unique<TestDbImpl>(inst.db_inner);
    bus->load_bootstrap_state();
    inst.bus = runtime.start(std::static_pointer_cast<simplex::Bus>(bus),
                             PSTRING() << "consensus." << node_idx << "." << instance_idx);
    inst.status = Instance::Running;
    inst.bus.publish<BlockFinalizedInMasterchain>(last_accepted_block_);
    inst.bus.publish<Start>(ChainState::from_zerostate(FIRST_PARENT, gen_shard_state(0), MIN_MC_BLOCK_ID));
    LOG(ERROR) << "Starting node #" << node_idx << "." << instance_idx;
  }

  td::actor::Task<> stop_instance(size_t node_idx, size_t instance_idx) {
    Node &node = nodes_[node_idx];
    Instance &inst = node.instances[instance_idx];
    if (inst.status == Instance::Stopped) {
      co_return td::Unit{};
    }
    if (inst.status == Instance::Stopping) {
      auto [task, promise] = td::actor::StartedTask<>::make_bridge();
      inst.extra_stop_waiters.push_back(std::move(promise));
      co_return co_await std::move(task);
    }
    LOG(ERROR) << "Stopping node #" << node_idx << "." << instance_idx;
    inst.bus.publish<StopRequested>();
    dynamic_cast<TestDbImpl &>(*inst.bus->db).disable();
    inst.bus = {};
    inst.status = Instance::Stopping;
    co_await std::move(*inst.stop_waiter);
    //std::move(inst.stop_waiter.value()).detach();
    //co_await td::actor::coro_sleep(td::Timestamp::in(0.5));
    inst.status = Instance::Stopped;
    inst.runtime = {};
    LOG(ERROR) << "Stopped node #" << node_idx << "." << instance_idx;
    for (auto &promise : inst.extra_stop_waiters) {
      promise.set_value(td::Unit{});
    }
    inst.extra_stop_waiters.clear();
    co_return {};
  }

  td::actor::Task<> run_gremlin() {
    for (size_t i = 0; i < GREMLIN_TIMES && !finishing_; ++i) {
      co_await td::actor::coro_sleep(td::Timestamp::in(td::Random::fast(GREMLIN_PERIOD.first, GREMLIN_PERIOD.second)));
      int cnt = td::Random::fast((int)GREMLIN_N.first, (int)GREMLIN_N.second);
      for (int i = 0; i < cnt; ++i) {
        run_gremlin_once().start().detach();
      }
    }
    co_return {};
  }

  td::actor::Task<> run_gremlin_once() {
    if (finishing_) {
      co_return {};
    }
    size_t kill_node_idx = 0, kill_inst_idx = 0;
    int cnt = 0;
    for (size_t node_idx = 0; node_idx < N_NODES; ++node_idx) {
      if (GREMLIN_KILLS_LEADER &&
          (!last_accepted_block_leader_idx_ || last_accepted_block_leader_idx_.value() != node_idx)) {
        continue;
      }
      for (size_t inst_idx = 0; inst_idx < nodes_[inst_idx].instances.size(); ++inst_idx) {
        if (nodes_[node_idx].instances[inst_idx].status == Instance::Running) {
          ++cnt;
          if (td::Random::fast(1, cnt) == 1) {
            kill_node_idx = node_idx;
            kill_inst_idx = inst_idx;
          }
        }
      }
    }
    if (cnt == 0) {
      co_return {};
    }
    co_await stop_instance(kill_node_idx, kill_inst_idx);
    co_await td::actor::coro_sleep(
        td::Timestamp::in(td::Random::fast(GREMLIN_DOWNTIME.first, GREMLIN_DOWNTIME.second)));
    if (finishing_) {
      co_return {};
    }
    start_instance(kill_node_idx, kill_inst_idx);
    co_return {};
  }

  td::actor::Task<> run_net_gremlin() {
    for (size_t i = 0; i < NET_GREMLIN_TIMES && !finishing_; ++i) {
      co_await td::actor::coro_sleep(
          td::Timestamp::in(td::Random::fast(NET_GREMLIN_PERIOD.first, NET_GREMLIN_PERIOD.second)));
      int cnt = td::Random::fast((int)NET_GREMLIN_N.first, (int)NET_GREMLIN_N.second);
      for (int i = 0; i < cnt; ++i) {
        run_net_gremlin_once().start().detach();
      }
    }
    co_return {};
  }

  td::actor::Task<> run_net_gremlin_once() {
    if (finishing_) {
      co_return {};
    }
    size_t selected_node_idx = 0, selected_inst_idx = 0;
    int cnt = 0;
    for (size_t node_idx = 0; node_idx < N_NODES; ++node_idx) {
      if (NET_GREMLIN_KILLS_LEADER &&
          (!last_accepted_block_leader_idx_ || last_accepted_block_leader_idx_.value() != node_idx)) {
        continue;
      }
      for (size_t inst_idx = 0; inst_idx < nodes_[inst_idx].instances.size(); ++inst_idx) {
        if (!nodes_[node_idx].instances[inst_idx].net_gremlin_active) {
          ++cnt;
          if (td::Random::fast(1, cnt) == 1) {
            selected_node_idx = node_idx;
            selected_inst_idx = inst_idx;
          }
        }
      }
    }
    if (cnt == 0) {
      co_return {};
    }
    nodes_[selected_node_idx].instances[selected_inst_idx].net_gremlin_active = true;
    co_await td::actor::ask(test_overlay, &TestOverlay::set_instance_disabled, selected_node_idx, selected_inst_idx,
                            true);
    co_await td::actor::coro_sleep(
        td::Timestamp::in(td::Random::fast(NET_GREMLIN_DOWNTIME.first, NET_GREMLIN_DOWNTIME.second)));
    co_await td::actor::ask(test_overlay, &TestOverlay::set_instance_disabled, selected_node_idx, selected_inst_idx,
                            false);
    nodes_[selected_node_idx].instances[selected_inst_idx].net_gremlin_active = false;
    co_return {};
  }

  td::actor::Task<> finalize() {
    finishing_ = true;
    LOG(WARNING) << "TEST FINISHED";
    std::vector<td::actor::Task<>> tasks;
    for (size_t idx = 0; idx < N_NODES; ++idx) {
      for (size_t i = 0; i < nodes_[i].instances.size(); ++i) {
        tasks.push_back(stop_instance(idx, i));
      }
    }
    co_await td::actor::all(std::move(tasks));
    LOG(WARNING) << "TEST RESULTS:";
    for (size_t idx = 0; idx < N_NODES; ++idx) {
      for (size_t inst_idx = 0; inst_idx < nodes_[idx].instances.size(); ++inst_idx) {
        Instance &inst = nodes_[idx].instances[inst_idx];
        LOG(WARNING) << "Node #" << idx << " instance #" << inst_idx << " : synced up to block "
                     << inst.last_accepted_block;
      }
    }
    co_return td::Unit{};
  }

  struct Instance {
    runtime::Runtime runtime;
    td::actor::ActorOwn<TestManagerFacade> manager_facade;
    simplex::BusHandle bus;

    BlockSeqno last_accepted_block = FIRST_PARENT.seqno();
    std::shared_ptr<TestDbImpl::DbInner> db_inner;
    td::actor::ActorOwn<CandidateStorage> candidate_storage;

    enum Status { Stopped, Running, Stopping };
    Status status = Stopped;
    td::optional<td::actor::StartedTask<>> stop_waiter;
    std::vector<td::Promise<td::Unit>> extra_stop_waiters;

    bool net_gremlin_active = false;
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
  std::vector<PeerValidator> validators_;
  ValidatorWeight total_weight_ = 0;

  td::actor::ActorOwn<keyring::Keyring> keyring_;

  std::map<BlockSeqno, td::Ref<BlockData>> accepted_blocks_;
  BlockIdExt last_accepted_block_ = FIRST_PARENT;
  td::optional<size_t> last_accepted_block_leader_idx_;
  bool finishing_ = false;
};

td::actor::Task<> TestManagerFacade::accept_block(BlockIdExt id, td::Ref<BlockData> data, size_t creator_idx,
                                                  td::Ref<block::BlockSignatureSet> signatures, int send_broadcast_mode,
                                                  bool apply) {
  CHECK(id.shard_full() == SHARD);
  LOG(WARNING) << "Accept block #" << id.seqno() << " (" << (signatures->is_final() ? "final" : "notarize")
               << " signatures), creator_idx=" << creator_idx;
  CHECK(id == data->block_id());
  td::actor::ask(test_consensus_, &TestConsensus::on_block_accepted, node_idx_, instance_idx_, data, creator_idx,
                 signatures)
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
  p.add_checked_option('\0', "slots-per-leader-window", "slots per leader window (default: 4)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(SLOTS_PER_LEADER_WINDOW, td::to_integer_safe<td::uint32>(arg));
    return td::Status::OK();
  });
  p.add_checked_option('\0', "net-ping", "network ping (range, default: 0.05:0.1)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(NET_PING, parse_range(arg));
    if (NET_PING.first < 0.0) {
      return td::Status::Error(PSTRING() << "invalid ping value " << arg);
    }
    return td::Status::OK();
  });
  p.add_checked_option('\0', "net-loss", "packet loss probability (default: 0)", [&](td::Slice arg) {
    NET_LOSS = td::to_double(arg);
    if (NET_LOSS < 0.0 || NET_LOSS > 1.0) {
      return td::Status::Error(PSTRING() << "invalid loss value " << arg);
    }
    return td::Status::OK();
  });

  p.add_checked_option('\0', "gremlin-period", "gremlin period (range, default: no gremlin)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(GREMLIN_PERIOD, parse_range(arg));
    if (GREMLIN_PERIOD.first < 0.0 || GREMLIN_PERIOD.second <= 0.0) {
      return td::Status::Error(PSTRING() << "invalid gremlin period value " << arg);
    }
    return td::Status::OK();
  });
  p.add_checked_option('\0', "gremlin-downtime", "gremlin downtime duration (range, default: 1)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(GREMLIN_DOWNTIME, parse_range(arg));
    if (GREMLIN_DOWNTIME.first < 0.0) {
      return td::Status::Error(PSTRING() << "invalid gremlin downtime value " << arg);
    }
    return td::Status::OK();
  });
  p.add_checked_option('\0', "gremlin-n", "how many nodes gremlin restarts at once (range, default: 1)",
                       [&](td::Slice arg) {
                         TRY_RESULT_ASSIGN(GREMLIN_N, parse_int_range<size_t>(arg));
                         return td::Status::OK();
                       });
  p.add_checked_option('\0', "gremlin-times", "how many times gremlin runs (default: unlimited)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(GREMLIN_TIMES, td::to_integer_safe<size_t>(arg));
    return td::Status::OK();
  });
  p.add_option('\0', "gremlin-kills-leader", "gremlin always restarts the current leader",
               [&]() { GREMLIN_KILLS_LEADER = true; });

  p.add_checked_option('\0', "net-gremlin-period", "network gremlin period (range, default: no gremlin)",
                       [&](td::Slice arg) {
                         TRY_RESULT_ASSIGN(NET_GREMLIN_PERIOD, parse_range(arg));
                         if (NET_GREMLIN_PERIOD.first < 0.0 || NET_GREMLIN_PERIOD.second <= 0.0) {
                           return td::Status::Error(PSTRING() << "invalid net gremlin period value " << arg);
                         }
                         return td::Status::OK();
                       });
  p.add_checked_option('\0', "net-gremlin-downtime", "network gremlin downtime duration (range, default: 10)",
                       [&](td::Slice arg) {
                         TRY_RESULT_ASSIGN(NET_GREMLIN_DOWNTIME, parse_range(arg));
                         if (NET_GREMLIN_DOWNTIME.first < 0.0) {
                           return td::Status::Error(PSTRING() << "invalid network gremlin downtime value " << arg);
                         }
                         return td::Status::OK();
                       });
  p.add_checked_option('\0', "net-gremlin-n", "how many nodes network gremlin disables at once (range, default: 1)",
                       [&](td::Slice arg) {
                         TRY_RESULT_ASSIGN(NET_GREMLIN_N, parse_int_range<size_t>(arg));
                         return td::Status::OK();
                       });
  p.add_checked_option('\0', "net-gremlin-times", "how many times network gremlin runs (default: unlimited)",
                       [&](td::Slice arg) {
                         TRY_RESULT_ASSIGN(NET_GREMLIN_TIMES, td::to_integer_safe<size_t>(arg));
                         return td::Status::OK();
                       });
  p.add_option('\0', "net-gremlin-kills-leader", "network gremlin always disables the current leader",
               [&]() { NET_GREMLIN_KILLS_LEADER = true; });
  p.add_checked_option('\0', "db-delay", "delay before db values are stored to disk (range, default: 0)",
                       [&](td::Slice arg) {
                         TRY_RESULT_ASSIGN(DB_DELAY, parse_range(arg));
                         if (DB_DELAY.first < 0.0) {
                           return td::Status::Error(PSTRING() << "invalid db delay value " << arg);
                         }
                         return td::Status::OK();
                       });
  p.add_checked_option('\0', "collation-time", "time it takes to collate a block (range, default: 0)",
                       [&](td::Slice arg) {
                         TRY_RESULT_ASSIGN(COLLATION_TIME, parse_range(arg));
                         if (COLLATION_TIME.first < 0.0) {
                           return td::Status::Error(PSTRING() << "invalid collation time " << arg);
                         }
                         return td::Status::OK();
                       });
  p.add_checked_option('\0', "validation-time", "time it takes to validate a block (range, default: 0)",
                       [&](td::Slice arg) {
                         TRY_RESULT_ASSIGN(VALIDATION_TIME, parse_range(arg));
                         if (VALIDATION_TIME.first < 0.0) {
                           return td::Status::Error(PSTRING() << "invalid validation time " << arg);
                         }
                         return td::Status::OK();
                       });

  p.run(argc, argv).ensure();
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
