/*
* Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "ton/ton-io.hpp"
#include "vm/cells/PrunnedCell.h"

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

td::Result<td::RefInt256> calculate_dispatch_queue_balance(vm::AugmentedDictionary& dispatch_queue,
                                                           int global_version) {
  td::RefInt256 result = td::zero_refint();
  TRY_STATUS(
      dispatch_queue.check_for_each_extra([&](Ref<vm::CellSlice> value, Ref<vm::CellSlice>, td::ConstBitPtr, int) {
        block::AccountDispatchQueue account_queue;
        TRY_BOOL(account_queue.unpack(value));
        return account_queue.dict.check_for_each([&](Ref<vm::CellSlice> value, td::ConstBitPtr, int) {
          auto balance = block::get_message_balance_for_dispatch_queue(value, global_version);
          TRY_BOOL(balance.is_valid());
          result += balance.grams;
          return td::Status::OK();
          ;
        });
      }));
  if (!result->is_valid()) {
    return td::Status::Error("failed to calculate dispatch queue balance");
  }
  return result;
}

// prune_message_queue converts msg queue from shard state to an identical in-memory msg queue
// with unneeded branches pruned (message contents). This speeds up its processing while keeping memory usage low.
td::Result<vm::CellSlice> prune_message_queue_entry(vm::CellSlice cs) {
  // _ enqueued_lt:uint64 out_msg:^MsgEnvelope = EnqueuedMsg;
  TRY_BOOL(cs.size_refs() == 1);
  vm::CellSlice env_cs = vm::load_cell_slice(cs.fetch_ref());
  // msg_envelope#4 ... msg:^(Message Any) = MsgEnvelope;
  // msg_envelope_v2#5 ... msg:^(Message Any) ... = MsgEnvelope;
  TRY_BOOL(env_cs.size_refs() >= 1);
  vm::CellSlice msg_cs = vm::load_cell_slice(env_cs.fetch_ref());
  vm::CellBuilder msg_cb;
  while (msg_cs.size_refs() > 0) {
    TRY_RESULT(prunned, vm::PrunnedCell<td::Unit>::create(msg_cs.fetch_ref(), td::Unit{}));
    msg_cb.store_ref(prunned);
  }
  msg_cb.append_cellslice(msg_cs);
  vm::CellBuilder env_cb;
  env_cb.store_ref(msg_cb.finalize()).append_cellslice(env_cs);
  return vm::CellBuilder{}.store_ref(env_cb.finalize()).append_cellslice(cs).as_cellslice();
}

td::Result<std::unique_ptr<vm::AugmentedDictionary>> prune_message_queue(vm::AugmentedDictionary& msg_queue) {
  auto new_msg_queue = std::make_unique<vm::AugmentedDictionary>(352, block::tlb::aug_OutMsgQueue);
  TRY_STATUS(msg_queue.check_for_each_extra(
      [&](Ref<vm::CellSlice> value, Ref<vm::CellSlice>, td::ConstBitPtr key, int key_len) {
        CHECK(key_len == 352);
        TRY_RESULT(pruned, prune_message_queue_entry(*value));
        TRY_BOOL(new_msg_queue->set(key, key_len, std::move(pruned)));
        return td::Status::OK();
      }));
  return std::move(new_msg_queue);
}

struct ParsedShardState {
  BlockIdExt block_id;
  std::unique_ptr<vm::AugmentedDictionary> msg_queue;
  Ref<vm::CellSlice> proc_info;
  td::RefInt256 accounts_balance = td::zero_refint();
  td::RefInt256 dispatch_queue_balance = td::zero_refint();
  td::RefInt256 mc_total_validator_fees = td::zero_refint();

  static td::actor::Task<std::shared_ptr<ParsedShardState>> fetch(
      Ref<ShardState> state, Ref<vm::Cell> block_root, int global_version,
      std::vector<std::shared_ptr<ParsedShardState>> prev = {}) {
    co_await td::actor::detach_from_actor();
    try {
      td::Timer timer;
      auto result = std::make_shared<ParsedShardState>();
      result->block_id = state->get_block_id();
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

      vm::AugmentedDictionary state_msg_queue{std::move(qinfo.out_queue), 352, block::tlb::aug_OutMsgQueue};
      if (prev.empty()) {
        result->msg_queue = CO_TRY(prune_message_queue(state_msg_queue));
      } else {
        result->msg_queue = CO_TRY(update_message_queue(prev, block_root, state->get_shard()));
      }
      CO_TRY_BOOL(result->msg_queue->get_wrapped_dict_root()->get_hash() ==
                  state_msg_queue.get_wrapped_dict_root()->get_hash());

      if (qinfo.extra.write().fetch_long(1) != 0) {
        block::gen::OutMsgQueueExtra::Record queue_extra;
        CO_TRY_BOOL(tlb::csr_unpack(qinfo.extra, queue_extra));
        vm::AugmentedDictionary dispatch_queue{queue_extra.dispatch_queue, 256, block::tlb::aug_DispatchQueue};
        block::tlb::DispatchQueueAugData::Record aug_data;
        CO_TRY_BOOL(tlb::csr_unpack_safe(dispatch_queue.get_root_extra(), aug_data));
        if (aug_data.total_balance.is_valid()) {
          result->dispatch_queue_balance = aug_data.total_balance.grams;
        } else if (!dispatch_queue.is_empty()) {
          result->dispatch_queue_balance = CO_TRY(calculate_dispatch_queue_balance(dispatch_queue, global_version));
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

 private:
  static td::Result<std::unique_ptr<vm::AugmentedDictionary>> update_message_queue(
      const std::vector<std::shared_ptr<ParsedShardState>>& prev, Ref<vm::Cell> block_root, ShardIdFull shard) {
    TRY_BOOL(!prev.empty());
    auto msg_queue =
        std::make_unique<vm::AugmentedDictionary>(prev[0]->msg_queue->get_root(), 352, block::tlb::aug_OutMsgQueue);
    if (prev.size() == 1) {
      if (prev[0]->block_id.shard_full() != shard) {
        TRY_BOOL(shard.pfx_len() > 0 && prev[0]->block_id.shard_full() == shard_parent(shard));
        TRY_BOOL(block::filter_out_msg_queue(*msg_queue, prev[0]->block_id.shard_full(), shard) >= 0);
      }
    } else {
      TRY_BOOL(prev.size() == 2);
      TRY_BOOL(prev[0]->block_id.shard_full() == shard_child(shard, true));
      TRY_BOOL(prev[1]->block_id.shard_full() == shard_child(shard, false));
      TRY_BOOL(msg_queue->combine_with(*prev[1]->msg_queue));
    }

    block::gen::Block::Record block_rec;
    block::gen::BlockInfo::Record block_info;
    block::gen::BlockExtra::Record block_extra;
    TRY_BOOL(tlb::unpack_cell(block_root, block_rec) && tlb::unpack_cell(block_rec.info, block_info) &&
             tlb::unpack_cell(block_rec.extra, block_extra));
    vm::AugmentedDictionary out_msg_dict{vm::load_cell_slice_ref(block_extra.out_msg_descr), 256,
                                         block::tlb::aug_OutMsgDescrDefault};
    // out_msg_dict reflects changes in out msg queue:
    // - msg_export_new: add entry, enqueued_lt = message created_lt
    // - msg_export_tr: add entry, enqueued_lt = block started_lt
    // - msg_export_deferred_tr: add entry, enqueued_lt = message emitted_lt
    // - msg_export_deq_short: delete entry
    // - msg_export_deq_short: delete entry
    // - msg_export_deq: delete entry
    // - msg_export_tr_req: delete entry with old prefix, add entry with new prefix, enqueued_lt = block started_lt
    TRY_STATUS(out_msg_dict.check_for_each_extra(
        [&](Ref<vm::CellSlice> value, Ref<vm::CellSlice>, td::ConstBitPtr key, int key_len) {
          CHECK(key_len == 256);
          int tag = block::gen::t_OutMsg.get_tag(*value);
          if (tag == block::gen::OutMsg::msg_export_deq_short) {
            block::gen::OutMsg::Record_msg_export_deq_short out_msg;
            TRY_BOOL(tlb::csr_unpack(value, out_msg));
            td::BitArray<352> msg_queue_key;
            msg_queue_key.bits().store_int(out_msg.next_workchain, 32);
            (msg_queue_key.bits() + 32).store_int(out_msg.next_addr_pfx, 64);
            (msg_queue_key.bits() + 96).copy_from(key, 256);
            TRY_BOOL(msg_queue->lookup_delete(msg_queue_key).not_null());
            return td::Status::OK();
          }
          if (tag != block::gen::OutMsg::msg_export_deq && tag != block::gen::OutMsg::msg_export_deq_imm &&
              tag != block::gen::OutMsg::msg_export_tr && tag != block::gen::OutMsg::msg_export_tr_req &&
              tag != block::gen::OutMsg::msg_export_new && tag != block::gen::OutMsg::msg_export_deferred_tr) {
            return td::Status::OK();
          }
          Ref<vm::Cell> env_cell = value->prefetch_ref();
          TRY_BOOL(env_cell.not_null());
          auto env_csr = vm::load_cell_slice_ref(env_cell);
          TRY_RESULT(msg_queue_key, get_out_queue_key_from_msg_env(env_csr, key));
          if (tag == block::gen::OutMsg::msg_export_tr_req) {
            block::gen::OutMsg::Record_msg_export_tr_req out_msg;
            block::gen::InMsg::Record_msg_import_tr in_msg;
            TRY_BOOL(tlb::csr_unpack(value, out_msg) && tlb::unpack_cell(out_msg.imported, in_msg));
            TRY_RESULT(old_msg_queue_key, get_out_queue_key_from_msg_env(vm::load_cell_slice_ref(in_msg.in_msg), key));
            TRY_BOOL(msg_queue->lookup_delete(old_msg_queue_key).not_null());
          }
          if (tag == block::gen::OutMsg::msg_export_deq || tag == block::gen::OutMsg::msg_export_deq_imm) {
            TRY_BOOL(msg_queue->lookup_delete(msg_queue_key).not_null());
          }
          if (tag == block::gen::OutMsg::msg_export_new || tag == block::gen::OutMsg::msg_export_tr ||
              tag == block::gen::OutMsg::msg_export_tr_req || tag == block::gen::OutMsg::msg_export_deferred_tr) {
            unsigned long long enqueued_lt;
            if (tag == block::gen::OutMsg::msg_export_tr || tag == block::gen::OutMsg::msg_export_tr_req) {
              enqueued_lt = block_info.start_lt;
            } else {
              TRY_BOOL(block::tlb::t_MsgEnvelope.get_emitted_lt(*env_csr, enqueued_lt));
            }
            TRY_RESULT(cs, prune_message_queue_entry(
                               vm::CellBuilder{}.store_long(enqueued_lt, 64).store_ref(env_cell).as_cellslice()));
            TRY_BOOL(msg_queue->set(msg_queue_key, std::move(cs), vm::Dictionary::SetMode::Add));
          }
          return td::Status::OK();
        }));

    return std::move(msg_queue);
  }

  static td::Result<td::BitArray<352>> get_out_queue_key_from_msg_env(Ref<vm::CellSlice> env_csr,
                                                                      td::Bits256 expected_msg_hash) {
    block::tlb::MsgEnvelope::Record_std env;
    block::gen::CommonMsgInfo::Record_int_msg_info info;
    if (!tlb::csr_unpack(env_csr, env) || !tlb::unpack_cell_inexact(env.msg, info) ||
        env.msg->get_hash().as_bits256() != expected_msg_hash) {
      return td::Status::Error("failed to unpack MsgEnvelope");
    }
    AccountIdPrefixFull src_prefix, dest_prefix;
    if (!block::tlb::t_MsgAddressInt.get_prefix_to(info.src, src_prefix) ||
        !block::tlb::t_MsgAddressInt.get_prefix_to(info.dest, dest_prefix)) {
      return td::Status::Error("failed to unpack msg addresses");
    }
    AccountIdPrefixFull next_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.next_addr);
    td::BitArray<352> key;
    key.bits().store_int(next_prefix.workchain, 32);
    (key.bits() + 32).store_int(next_prefix.account_id_prefix, 64);
    (key.bits() + 96).copy_from(expected_msg_hash);
    return key;
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
      processed_upto.push_back(
          block::MsgProcessedUptoCollection::unpack(state->block_id.shard_full(), state->proc_info));
      CO_TRY_BOOL(processed_upto.back());
      for (auto& proc : processed_upto.back()->list) {
        auto aux_state = co_await get_aux_mc_state(std::min(proc.mc_seqno, mc_state->get_seqno()));
        proc.compute_shard_end_lt = aux_state->get_config()->get_compute_shard_end_lt_func();
      }
    }
    size_t total_messages = 0;
    for (auto& state : all_states) {
      CO_TRY(state->msg_queue->check_for_each_extra(
          [&](Ref<vm::CellSlice> value, Ref<vm::CellSlice>, td::ConstBitPtr key, int key_len) {
            CHECK(key_len == 352);
            ++total_messages;
            block::EnqueuedMsgDescr enq_msg_descr;
            TRY_BOOL(enq_msg_descr.unpack(value.write()) && enq_msg_descr.check_key(key));
            for (auto& proc : processed_upto) {
              if (proc->already_processed(enq_msg_descr)) {
                return td::Status::OK();
              }
            }
            vm::CellSlice msg_cs = vm::load_cell_slice(enq_msg_descr.msg_);
            block::gen::CommonMsgInfo::Record_int_msg_info info;
            block::CurrencyCollection cc;
            TRY_BOOL(tlb::unpack(msg_cs, info) && cc.unpack(info.value));
            result->global_balance += cc.grams + enq_msg_descr.fwd_fee_remaining_;
            return td::Status::OK();
          }));
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
    int global_version = mc_state->get_config()->get_global_version();
    parse_tasks.push_back(ParsedShardState::fetch(mc_state, mc_block_root, global_version).start());

    std::vector<td::actor::StartedTask<std::pair<Ref<ShardState>, Ref<BlockData>>>> shard_state_block_tasks;
    for (auto desc : mc_state->get_shards()) {
      shard_state_block_tasks.push_back(load_state_block(desc->top_block_id()).start());
    }
    for (auto& [state, block] : co_await td::actor::all(std::move(shard_state_block_tasks))) {
      parse_tasks.push_back(
          ParsedShardState::fetch(state, block.not_null() ? block->root_cell() : Ref<vm::Cell>{}, global_version)
              .start());
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
