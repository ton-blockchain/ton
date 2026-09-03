/*
* Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "ton/ton-io.hpp"

#include "block-auto.h"
#include "block-parse.h"
#include "global-balance-calculator.hpp"
#include "shard.hpp"

namespace ton::validator {

/*
 * Global balance (only grams, no extra currencies) of the blockchain at a specific masterchain block consists of:
 * 1. For each top shard state and masterchain state, total balance of ShardAccounts
 * 2. For each top shard state and masterchain state, total balance of DispatchQueue
 * 3. For each top shard state and masterchain state, sum of balances of unprocessed messages in OutMsgQueue
 *    Balance of a message in queue is: value + fwd_fee_remaining (+ legacy ihr_fee for old global versions)
 * 4. total_validator_fees in masterchain state - fees that have not been recovered yet
 *
 * Invariant: global balance at masterchain block X+1 is equal to:
 * 1. Global balance at masterchain block X
 * 2. + funds created for all new blocks
 * 3. - funds burned in masterchain block X+1
 */

namespace {

struct ParsedShardState {
  ShardIdFull shard;
  std::unique_ptr<vm::AugmentedDictionary> msg_queue;
  Ref<vm::CellSlice> proc_info;
  td::RefInt256 accounts_balance = td::zero_refint();
  td::RefInt256 dispatch_queue_balance = td::zero_refint();
  td::RefInt256 mc_total_validator_fees = td::zero_refint();

  static td::actor::Task<std::shared_ptr<ParsedShardState>> fetch(
      Ref<ShardState> state, Ref<vm::Cell> block_root, std::vector<std::shared_ptr<ParsedShardState>> prev = {}) {
    co_await td::actor::detach_from_actor();
    try {
      td::Timer timer;
      auto result = std::make_shared<ParsedShardState>();
      result->shard = state->get_shard();
      block::gen::ShardStateUnsplit::Record state_rec;
      CO_TRY_BOOL(tlb::unpack_cell(state->root_cell(), state_rec));

      vm::AugmentedDictionary accounts_dict{vm::load_cell_slice_ref(state_rec.accounts), 256,
                                            block::tlb::aug_ShardAccounts};
      block::gen::DepthBalanceInfo::Record extra;
      block::CurrencyCollection accounts_cc;
      CO_TRY_BOOL(tlb::csr_unpack_safe(accounts_dict.get_root_extra(), extra) && accounts_cc.unpack(extra.balance));
      result->accounts_balance = accounts_cc.grams;

      block::gen::OutMsgQueueInfo::Record qinfo;
      CO_TRY_BOOL(tlb::unpack_cell(state_rec.out_msg_queue_info, qinfo));
      result->proc_info = qinfo.proc_info;

      result->msg_queue =
          std::make_unique<vm::AugmentedDictionary>(std::move(qinfo.out_queue), 352, block::tlb::aug_OutMsgQueue);

      if (qinfo.extra.write().fetch_long(1) != 0) {
        block::gen::OutMsgQueueExtra::Record queue_extra;
        CO_TRY_BOOL(tlb::csr_unpack(qinfo.extra, queue_extra));
        vm::AugmentedDictionary dispatch_queue{queue_extra.dispatch_queue, 256, block::tlb::aug_DispatchQueue};
        block::tlb::DispatchQueueAugData::Record aug_data;
        CO_TRY_BOOL(tlb::csr_unpack_safe(dispatch_queue.get_root_extra(), aug_data));
        if (aug_data.total_balance.is_valid()) {
          result->dispatch_queue_balance = aug_data.total_balance.grams;
        } else if (!dispatch_queue.is_empty()) {
          // TODO: calculate dispatch queue balance (for historical blocks only - new blocks will have stored balance)
          co_return td::Status::Error("dispatch queue balance is not known");
        }
      }

      if (state->get_shard().is_masterchain()) {
        block::CurrencyCollection total_validator_fees;
        CO_TRY_BOOL(total_validator_fees.unpack(state_rec.r1.total_validator_fees));
        result->mc_total_validator_fees = total_validator_fees.grams;
      }

      VLOG(validator, INFO) << "Parsed shard state " << state->get_block_id().id
                            << ": accounts_balance=" << result->accounts_balance
                            << " dispatch_queue_balance=" << result->dispatch_queue_balance
                            << " mc_total_validator_fees=" << result->mc_total_validator_fees
                            << " time=" << timer.elapsed();
      co_return result;
    } catch (vm::VmError& e) {
      co_return e.as_status();
    }
  }
};

struct GlobalInfo {
  Ref<MasterchainStateQ> mc_state;
  td::RefInt256 global_balance = td::zero_refint();
  td::RefInt256 last_mc_block_burned = td::zero_refint();
};

td::actor::Task<std::shared_ptr<GlobalInfo>> compute_global_balance(
    std::vector<std::shared_ptr<ParsedShardState>> all_states, Ref<MasterchainStateQ> mc_state,
    Ref<vm::Cell> mc_block_root, td::actor::ActorId<ValidatorManager> manager) {
  co_await td::actor::detach_from_actor();
  try {
    td::Timer timer;
    auto result = std::make_shared<GlobalInfo>();
    result->mc_state = mc_state;
    for (auto& state : all_states) {
      result->global_balance += state->accounts_balance;
      result->global_balance += state->dispatch_queue_balance;
      result->global_balance += state->mc_total_validator_fees;
    }

    std::map<BlockSeqno, Ref<MasterchainStateQ>> aux_mc_states;
    aux_mc_states[mc_state->get_seqno()] = mc_state;
    auto get_aux_mc_state = [&](BlockSeqno aux_mc_seqno) -> td::actor::Task<Ref<MasterchainStateQ>> {
      auto it = aux_mc_states.find(aux_mc_seqno);
      if (it != aux_mc_states.end()) {
        co_return it->second;
      }
      BlockIdExt block_id;
      CO_TRY_BOOL(mc_state->get_old_mc_block_id(aux_mc_seqno, block_id));
      co_return aux_mc_states[aux_mc_seqno] = Ref<MasterchainStateQ>{
          co_await td::actor::ask(manager, &ValidatorManager::get_shard_state_from_db_short, block_id)};
    };
    std::vector<std::unique_ptr<block::MsgProcessedUptoCollection>> processed_upto;
    for (auto& state : all_states) {
      processed_upto.push_back(block::MsgProcessedUptoCollection::unpack(state->shard, state->proc_info));
      CO_TRY_BOOL(processed_upto.back());
      for (auto& proc : processed_upto.back()->list) {
        auto aux_state = co_await get_aux_mc_state(std::min(proc.mc_seqno, mc_state->get_seqno()));
        proc.compute_shard_end_lt = aux_state->get_config()->get_compute_shard_end_lt_func();
      }
    }
    size_t total_messages = 0;
    for (auto& state : all_states) {
      bool msg_queue_ok = state->msg_queue->check_for_each_extra(
          [&](Ref<vm::CellSlice> value, Ref<vm::CellSlice>, td::ConstBitPtr key, int key_len) {
            CHECK(key_len == 352);
            ++total_messages;
            block::EnqueuedMsgDescr enq_msg_descr;
            if (!enq_msg_descr.unpack(value.write()) || !enq_msg_descr.check_key(key)) {
              return false;
            }
            for (auto& proc : processed_upto) {
              if (proc->already_processed(enq_msg_descr)) {
                return true;
              }
            }
            vm::CellSlice msg_cs = vm::load_cell_slice(enq_msg_descr.msg_);
            block::gen::CommonMsgInfo::Record_int_msg_info info;
            block::CurrencyCollection cc;
            if (!tlb::unpack(msg_cs, info) || !cc.unpack(info.value)) {
              return false;
            }
            result->global_balance += cc.grams + enq_msg_descr.fwd_fee_remaining_;
            return true;
          });
      CO_TRY_BOOL(msg_queue_ok);
    }

    if (mc_state->get_seqno() != 0) {
      block::gen::Block::Record block_rec;
      CO_TRY_BOOL(tlb::unpack_cell(mc_block_root, block_rec));
      block::ValueFlow value_flow;
      CO_TRY_BOOL(value_flow.unpack(vm::load_cell_slice_ref(block_rec.value_flow)));
      result->last_mc_block_burned = value_flow.burned.grams;
    }

    CO_TRY_BOOL(result->global_balance->is_valid());
    VLOG(validator, INFO) << "Calculated global balance at " << mc_state->get_block_id().seqno()
                          << ": balance=" << result->global_balance << " messages=" << total_messages
                          << " time=" << timer.elapsed();
    co_return result;
  } catch (vm::VmError& e) {
    co_return e.as_status();
  }
}

void compare_global_balance(const GlobalInfo& prev, const GlobalInfo& next) {
  td::RefInt256 masterchain_create_fee = td::zero_refint(), basechain_create_fee = td::zero_refint();
  if (auto param = prev.mc_state->get_config()->get_config_param(14); param.not_null()) {
    block::gen::BlockCreateFees::Record create_fees;
    if (!(tlb::unpack_cell(param, create_fees) &&
          block::tlb::t_Grams.as_integer_to(create_fees.masterchain_block_fee, masterchain_create_fee) &&
          block::tlb::t_Grams.as_integer_to(create_fees.basechain_block_fee, basechain_create_fee))) {
      CHECK(false);
    }
  }
  td::RefInt256 funds_created = masterchain_create_fee;
  for (auto desc : next.mc_state->get_shards()) {
    int new_blocks = 0;
    auto prev_desc_left = prev.mc_state->get_shard_from_config(desc->shard() - 1, false);
    auto prev_desc_right = prev.mc_state->get_shard_from_config(desc->shard() + 1, false);
    CHECK(prev_desc_left.is_null() == prev_desc_right.is_null());
    if (prev_desc_left.is_null()) {
      // New workchain
      new_blocks = desc->top_block_id().seqno();
    } else if (prev_desc_left->shard() == desc->shard()) {
      new_blocks = desc->top_block_id().seqno() - prev_desc_left->top_block_id().seqno();
    } else if (prev_desc_left->shard() == prev_desc_right->shard()) {
      CHECK(desc->shard().pfx_len() > 0 && prev_desc_left->shard() == shard_parent(desc->shard()));
      new_blocks = desc->top_block_id().seqno() - prev_desc_left->top_block_id().seqno();
    } else {
      CHECK(prev_desc_left->shard() == shard_child(desc->shard(), true));
      CHECK(prev_desc_right->shard() == shard_child(desc->shard(), false));
      new_blocks = desc->top_block_id().seqno() -
                   std::max(prev_desc_left->top_block_id().seqno(), prev_desc_right->top_block_id().seqno());
    }
    funds_created += (basechain_create_fee >> shard_prefix_length(desc->shard())) * new_blocks;
  }
  CHECK(funds_created->is_valid());

  td::RefInt256 expected_next = prev.global_balance + funds_created - next.last_mc_block_burned;
  CHECK(expected_next->is_valid());
  if (expected_next->cmp(*next.global_balance)) {
    LOG(ERROR) << "Checked global balance at " << next.mc_state->get_seqno() << ", ERROR: prev=" << prev.global_balance
               << " created=" << funds_created << " burned=" << next.last_mc_block_burned
               << " next=" << next.global_balance << " expected_next=" << expected_next
               << " (diff=" << next.global_balance - expected_next << ")";
  } else {
    VLOG(validator, INFO) << "Checked global balance at " << next.mc_state->get_seqno()
                          << ", OK: prev=" << prev.global_balance << " created=" << funds_created
                          << " burned=" << next.last_mc_block_burned << " next=" << next.global_balance;
  }
}

class GlobalBalanceCalculatorImpl : public GlobalBalanceCalculator {
 public:
  explicit GlobalBalanceCalculatorImpl(BlockIdExt start_mc_block, td::actor::ActorId<ValidatorManager> manager)
      : current_mc_block_(start_mc_block), manager_(std::move(manager)) {
  }

  void start_up() override {
    run().start().detach_ensure("GlobalBalance");
  }

  td::actor::Task<> run() {
    co_await init();
    while (true) {
      co_await advance_mc_seqno();
    }
    co_return {};
  }

  td::actor::Task<> init() {
    co_await wait_for_mc_seqno(current_mc_block_.seqno());
    auto [mc_state, mc_block] = co_await load_mc_state_block(current_mc_block_);
    auto mc_block_root = mc_block.not_null() ? mc_block->root_cell() : Ref<vm::Cell>{};
    auto all_states = co_await load_all_states(mc_state, mc_block_root);
    current_ = co_await compute_global_balance(all_states, mc_state, mc_block_root, manager_);
    co_return {};
  }

  td::actor::Task<> advance_mc_seqno() {
    co_await wait_for_mc_seqno(current_mc_block_.seqno() + 1);
    auto next_mc_block_id =
        (co_await td::actor::ask(manager_, &ValidatorManager::get_block_by_seqno_from_db,
                                 AccountIdPrefixFull{masterchainId, shardIdAll}, current_mc_block_.seqno() + 1))
            ->id();
    auto [mc_state, mc_block /* not null! */] = co_await load_mc_state_block(next_mc_block_id);
    auto all_states = co_await load_all_states(mc_state, mc_block->root_cell());
    auto next = co_await compute_global_balance(all_states, mc_state, mc_block->root_cell(), manager_);
    compare_global_balance(*current_, *next);

    current_mc_block_ = next_mc_block_id;
    current_ = std::move(next);
    co_return {};
  }

 private:
  BlockIdExt current_mc_block_;
  td::actor::ActorId<ValidatorManager> manager_;
  std::shared_ptr<GlobalInfo> current_;

  td::actor::Task<std::vector<std::shared_ptr<ParsedShardState>>> load_all_states(Ref<MasterchainStateQ> mc_state,
                                                                                  Ref<vm::Cell> mc_block_root) {
    std::vector<td::actor::StartedTask<std::shared_ptr<ParsedShardState>>> parse_tasks;
    parse_tasks.push_back(ParsedShardState::fetch(mc_state, mc_block_root).start());

    std::vector<td::actor::StartedTask<std::pair<Ref<ShardState>, Ref<BlockData>>>> shard_state_block_tasks;
    for (auto desc : mc_state->get_shards()) {
      shard_state_block_tasks.push_back(load_state_block(desc->top_block_id()).start());
    }
    for (auto& [state, block] : co_await td::actor::all(std::move(shard_state_block_tasks))) {
      parse_tasks.push_back(
          ParsedShardState::fetch(state, block.not_null() ? block->root_cell() : Ref<vm::Cell>{}).start());
    }
    co_return co_await td::actor::all(std::move(parse_tasks));
  }

  td::actor::Task<> wait_for_mc_seqno(BlockSeqno mc_seqno) {
    while (true) {
      auto R = co_await td::actor::ask(manager_, &ValidatorManager::wait_shard_client_state, mc_seqno,
                                       td::Timestamp::in(10.0))
                   .wrap();
      if (R.is_ok()) {
        co_return {};
      }
      co_await td::actor::coro_sleep(td::Timestamp::in(0.1));
    }
  }

  td::actor::Task<std::pair<Ref<ShardState>, Ref<BlockData>>> load_state_block(BlockIdExt block_id) {
    auto state = co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db_short, block_id);
    Ref<BlockData> block;
    if (block_id.seqno() != 0) {
      block = co_await td::actor::ask(manager_, &ValidatorManager::get_block_data_from_db_short, block_id);
    }
    co_return {state, block};
  }

  td::actor::Task<std::pair<Ref<MasterchainStateQ>, Ref<BlockData>>> load_mc_state_block(BlockIdExt block_id) {
    CHECK(block_id.is_masterchain());
    auto [state, block] = co_await load_state_block(block_id);
    co_return {Ref<MasterchainStateQ>{state}, block};
  }
};

}  // namespace

td::actor::ActorOwn<GlobalBalanceCalculator> GlobalBalanceCalculator::create(
    BlockIdExt start_mc_block, td::actor::ActorId<ValidatorManager> manager) {
  return td::actor::create_actor<GlobalBalanceCalculatorImpl>("GlobalBalance", start_mc_block, std::move(manager));
}

}  // namespace ton::validator
