/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

// Tests (global version 16 unless explicitly stated):
// - FetchWithoutPrevWithAndWithoutStoredDispatchBalance: ParsedShardState::fetch
//   with no predecessor and with missing, unknown, or stored dispatch balance.
// - FetchRegular: ParsedShardState::fetch with one predecessor, using both
//   dispatch dictionary differences and stored augmentation.
// - FetchSplit / FetchMerge: ParsedShardState::fetch after a shard split / merge.
// - UpdateMessageQueueAllOutMsgTypesRegular: update_message_queue for every
//   OutMsg constructor, including ignored records, deletions, and requeueing.
// - UpdateMessageQueueSplit / UpdateMessageQueueMerge: update_message_queue
//   filters a parent queue / combines child queues before applying OutMsgDescr.
// - CalculateDispatchQueueBalance: calculate_dispatch_queue_balance for several
//   accounts and an empty queue, with and without stored account balances.
// - CalculateDispatchQueueBalanceDiff: calculate_dispatch_queue_balance_diff
//   for message/account additions, deletions, replacements, and re-encoding.
// - PruneMessageQueue: prune_message_queue preserves hashes and queue entries
//   while discarding referenced message bodies.
// - GetDispatchQueueBalance: get_dispatch_queue_balance without a predecessor,
//   for regular/split/merge predecessors, and without a cached dispatch queue.
// - QueueBalancesWithNonzeroIhrFee: get_out_queue_message_balance,
//   calculate_dispatch_queue_balance[_diff], get_dispatch_queue_balance, and
//   ParsedShardState::fetch with nonzero IHR fields at global versions 16 and 7.

#include <utility>
#include <vector>

#include "block/block-auto.h"
#include "crypto/block/block-parse.h"
#include "crypto/block/block.h"
#include "td/actor/TestScheduler.h"
#include "td/actor/actor.h"
#include "td/utils/tests.h"
#include "validator/fabric.h"
#include "validator/impl/global-balance-calculator-internal.hpp"

namespace ton::validator {
namespace {

using detail::ParsedShardState;

StdSmcAddress make_address(td::uint64 prefix, td::uint64 suffix = 0) {
  StdSmcAddress result;
  result.bits().store_uint(prefix, 64);
  (result.bits() + 64).store_uint(suffix, 64);
  return result;
}

td::Ref<vm::CellSlice> make_grams(td::RefInt256 amount) {
  vm::CellBuilder cb;
  CHECK(block::tlb::t_Grams.store_integer_value(cb, *amount));
  return cb.as_cellslice_ref();
}

td::Ref<vm::CellSlice> make_grams(long long amount) {
  return make_grams(td::make_refint(amount));
}

td::Ref<vm::CellSlice> make_currency(long long amount) {
  return block::CurrencyCollection{amount}.pack();
}

td::Ref<vm::Cell> make_internal_message(td::uint64 src_prefix, td::uint64 dest_prefix, long long value,
                                        long long ihr_fee, long long fwd_fee, LogicalTime created_lt,
                                        td::uint64 body_marker = 0) {
  block::gen::CommonMsgInfo::Record_int_msg_info info{
      .ihr_disabled = true,
      .bounce = false,
      .bounced = false,
      .src = block::tlb::t_MsgAddressInt.pack_std_address(basechainId, make_address(src_prefix, body_marker)),
      .dest = block::tlb::t_MsgAddressInt.pack_std_address(basechainId, make_address(dest_prefix, body_marker + 1)),
      .value = make_currency(value),
      .extra_flags = make_grams(ihr_fee),
      .fwd_fee = make_grams(fwd_fee),
      .created_lt = created_lt,
      .created_at = 0,
  };
  vm::CellBuilder cb;
  CHECK(block::gen::t_CommonMsgInfo.pack(cb, info));
  CHECK(cb.store_zeroes_bool(1));  // no StateInit
  CHECK(cb.store_ones_bool(1));    // body is stored in a reference
  CHECK(cb.store_ref_bool(vm::CellBuilder{}.store_long(body_marker, 64).finalize()));
  return cb.finalize();
}

td::Ref<vm::Cell> make_envelope(td::Ref<vm::Cell> msg, long long fwd_fee_remaining) {
  td::Ref<vm::Cell> result;
  CHECK(block::tlb::t_MsgEnvelope.pack_cell(
      result, block::tlb::MsgEnvelope::Record_std{0, 96, td::make_refint(fwd_fee_remaining), std::move(msg), {}, {}}));
  return result;
}

td::Ref<vm::CellSlice> make_enqueued(LogicalTime enqueued_lt, td::Ref<vm::Cell> envelope) {
  return vm::CellBuilder{}.store_long(enqueued_lt, 64).store_ref(std::move(envelope)).as_cellslice_ref();
}

td::BitArray<352> make_out_queue_key(WorkchainId next_workchain, td::uint64 next_prefix, td::Bits256 msg_hash) {
  td::BitArray<352> key;
  key.bits().store_int(next_workchain, 32);
  (key.bits() + 32).store_uint(next_prefix, 64);
  (key.bits() + 96).copy_from(msg_hash.bits(), 256);
  return key;
}

td::BitArray<352> make_out_queue_key(td::Ref<vm::Cell> msg, td::uint64 next_prefix) {
  return make_out_queue_key(basechainId, next_prefix, msg->get_hash().as_bits256());
}

std::unique_ptr<vm::AugmentedDictionary> make_out_queue() {
  return std::make_unique<vm::AugmentedDictionary>(352, block::tlb::aug_OutMsgQueue);
}

void add_out_queue_entry(vm::AugmentedDictionary& queue, td::Ref<vm::Cell> msg, td::uint64 next_prefix,
                         LogicalTime enqueued_lt, td::Ref<vm::Cell> envelope) {
  auto key = make_out_queue_key(msg, next_prefix);
  CHECK(queue.set(key, make_enqueued(enqueued_lt, std::move(envelope)), vm::Dictionary::SetMode::Add));
}

size_t dictionary_size(vm::DictionaryFixed& dict) {
  size_t count = 0;
  CHECK(dict.check_for_each([&](td::Ref<vm::CellSlice>, td::ConstBitPtr, int) {
              ++count;
              return td::Status::OK();
            })
            .is_ok());
  return count;
}

void expect_int(td::RefInt256 value, long long expected) {
  CHECK(value.not_null());
  CHECK(value->cmp(*td::make_refint(expected)) == 0);
}

BlockIdExt make_block_id(ShardIdFull shard, BlockSeqno seqno) {
  return BlockIdExt{shard.workchain, shard.shard, seqno, RootHash::zero(), FileHash::zero()};
}

std::shared_ptr<ParsedShardState> make_parsed_state(BlockIdExt block_id,
                                                    std::unique_ptr<vm::AugmentedDictionary> msg_queue,
                                                    std::unique_ptr<vm::AugmentedDictionary> dispatch_queue,
                                                    long long dispatch_balance) {
  auto result = std::make_shared<ParsedShardState>();
  result->block_id = block_id;
  result->msg_queue = std::move(msg_queue);
  result->dispatch_queue = std::move(dispatch_queue);
  result->dispatch_queue_balance = td::make_refint(dispatch_balance);
  return result;
}

}  // namespace
}  // namespace ton::validator

namespace ton::validator {
namespace {

struct FetchDispatchMessageSpec {
  LogicalTime lt;
  long long grams;
  long long fwd_fee;
  long long ihr_fee;
  td::uint64 payload;
};

struct FetchDispatchAccountSpec {
  td::uint64 id;
  std::vector<FetchDispatchMessageSpec> messages;
  bool store_total_balance;
};

vm::AugmentedDictionary make_fetch_dispatch_queue(const std::vector<FetchDispatchAccountSpec>& accounts) {
  vm::AugmentedDictionary result{256, block::tlb::aug_DispatchQueue};
  for (const auto& account_spec : accounts) {
    block::AccountDispatchQueue account_queue;
    long long total = 0;
    auto address = make_address(account_spec.id);
    for (const auto& message : account_spec.messages) {
      auto msg = make_internal_message(account_spec.id, message.payload + 1000, message.grams, message.ihr_fee,
                                       message.fwd_fee, message.lt, message.payload);
      td::Ref<vm::Cell> envelope;
      CHECK(block::tlb::t_MsgEnvelope.pack_cell(
          envelope, block::tlb::MsgEnvelope::Record_std{0, 0, td::make_refint(0), msg, {}, {}}));
      auto key = td::BitArray<64>::zero();
      key.bits().store_uint(message.lt, 64);
      CHECK(account_queue.dict.set(key, make_enqueued(message.lt, std::move(envelope)), vm::Dictionary::SetMode::Add));
      ++account_queue.dict_size;
      total += message.grams + message.fwd_fee;
    }
    account_queue.total_balance =
        account_spec.store_total_balance ? block::CurrencyCollection{total} : block::CurrencyCollection{};
    td::Ref<vm::CellSlice> packed;
    CHECK(account_queue.pack(packed));
    CHECK(result.set(address, std::move(packed), vm::Dictionary::SetMode::Add));
  }
  return result;
}

std::unique_ptr<vm::AugmentedDictionary> copy_test_dispatch_queue(const vm::AugmentedDictionary& queue) {
  return std::make_unique<vm::AugmentedDictionary>(queue.get_root(), 256, block::tlb::aug_DispatchQueue);
}

td::Ref<vm::Cell> make_test_shard_accounts(ShardIdFull shard) {
  // An empty ShardAccounts dictionary has no root extra to unpack. Put one
  // syntactically valid, zero-balance, uninitialized account into the shard.
  StdSmcAddress address = StdSmcAddress::zero();
  address.bits().store_uint(shard.shard, 64);
  CHECK(shard_contains(shard, extract_addr_prefix(shard.workchain, address)));

  vm::CellBuilder storage_cb;
  CHECK(storage_cb.store_zeroes_bool(64));  // last_trans_lt:uint64
  CHECK(block::CurrencyCollection::zero().store(storage_cb));
  CHECK(storage_cb.store_zeroes_bool(2));  // account_uninit$00
  auto storage = storage_cb.finalize();

  vm::CellBuilder account_cb;
  CHECK(account_cb.store_ones_bool(1));  // account$1
  CHECK(account_cb.append_cellslice_bool(block::tlb::t_MsgAddressInt.pack_std_address(shard.workchain, address)));
  CHECK(block::store_UInt7(account_cb, 0, 0));  // StorageUsed
  CHECK(account_cb.store_zeroes_bool(3));       // storage_extra_none$000
  CHECK(account_cb.store_zeroes_bool(33));      // last_paid:uint32, due_payment:nothing$0
  CHECK(account_cb.append_data_cell_bool(std::move(storage)));
  auto account = account_cb.finalize();
  CHECK(block::gen::t_Account.validate_ref(account));

  vm::CellBuilder shard_account_cb;
  CHECK(shard_account_cb.store_ref_bool(std::move(account)));
  CHECK(shard_account_cb.store_zeroes_bool(256 + 64));

  vm::AugmentedDictionary accounts{256, block::tlb::aug_ShardAccounts};
  CHECK(accounts.set_builder(address.bits(), 256, shard_account_cb, vm::Dictionary::SetMode::Add));
  auto result = accounts.get_wrapped_dict_root();
  CHECK(result.not_null() && block::gen::t_ShardAccounts.validate_ref(result));
  return result;
}

td::Ref<vm::Cell> make_test_out_msg_queue_info(const vm::AugmentedDictionary& msg_queue,
                                               const vm::AugmentedDictionary* dispatch_queue) {
  vm::CellBuilder cb;
  CHECK(cb.append_cellslice_bool(vm::load_cell_slice_ref(msg_queue.get_wrapped_dict_root())));
  CHECK(cb.store_zeroes_bool(1));  // empty ProcessedInfo
  if (dispatch_queue == nullptr) {
    CHECK(cb.store_zeroes_bool(1));  // extra:nothing$0
  } else {
    CHECK(cb.store_ones_bool(1));    // extra:just$1
    CHECK(cb.store_zeroes_bool(4));  // out_msg_queue_extra#0
    CHECK(cb.append_cellslice_bool(vm::load_cell_slice_ref(dispatch_queue->get_wrapped_dict_root())));
    CHECK(cb.store_zeroes_bool(1));  // out_queue_size:nothing$0
  }
  auto result = cb.finalize();
  CHECK(block::gen::t_OutMsgQueueInfo.validate_ref(result));
  return result;
}

td::Ref<vm::Cell> make_test_shard_state_root(BlockIdExt block_id, const vm::AugmentedDictionary& msg_queue,
                                             const vm::AugmentedDictionary* dispatch_queue) {
  vm::CellBuilder cb;
  CHECK(cb.store_long_bool(0x9023afe2, 32));
  CHECK(cb.store_long_bool(0, 32));  // global_id
  CHECK(block::ShardId{block_id.shard_full()}.serialize(cb));
  CHECK(cb.store_long_bool(block_id.seqno(), 32));
  CHECK(cb.store_zeroes_bool(32));  // vert_seq_no
  CHECK(cb.store_zeroes_bool(32));  // gen_utime
  CHECK(cb.store_zeroes_bool(64));  // gen_lt
  CHECK(cb.store_zeroes_bool(32));  // min_ref_mc_seqno
  CHECK(cb.store_ref_bool(make_test_out_msg_queue_info(msg_queue, dispatch_queue)));
  CHECK(cb.store_zeroes_bool(1));  // before_split
  CHECK(cb.store_ref_bool(make_test_shard_accounts(block_id.shard_full())));

  vm::CellBuilder aux_cb;
  CHECK(aux_cb.store_zeroes_bool(128));  // overload_history, underload_history
  CHECK(block::CurrencyCollection::zero().store(aux_cb));
  CHECK(block::CurrencyCollection::zero().store(aux_cb));
  CHECK(aux_cb.store_zeroes_bool(1));  // empty libraries
  CHECK(aux_cb.store_zeroes_bool(1));  // master_ref:nothing$0
  CHECK(cb.store_ref_bool(aux_cb.finalize()));
  CHECK(cb.store_zeroes_bool(1));  // custom:nothing$0

  auto result = cb.finalize();
  CHECK(block::gen::t_ShardStateUnsplit.validate_ref(result));
  return result;
}

td::Ref<ShardState> make_test_shard_state(BlockIdExt block_id, const vm::AugmentedDictionary& msg_queue,
                                          const vm::AugmentedDictionary* dispatch_queue) {
  return create_shard_state(block_id, make_test_shard_state_root(block_id, msg_queue, dispatch_queue)).move_as_ok();
}

std::shared_ptr<ParsedShardState> fetch_test_shard_state(td::Ref<ShardState> state, td::Ref<vm::Cell> block_root,
                                                         int global_version,
                                                         std::vector<std::shared_ptr<ParsedShardState>> prev = {}) {
  td::Result<std::shared_ptr<ParsedShardState>> result{td::Status::Error("fetch task did not run")};
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    result = co_await ParsedShardState::fetch(std::move(state), std::move(block_root), global_version, std::move(prev))
                 .wrap();
    co_return td::Unit{};
  });
  if (result.is_error()) {
    LOG(ERROR) << "ParsedShardState::fetch failed: " << result.error();
  }
  LOG_CHECK(result.is_ok()) << result.error();
  return result.move_as_ok();
}

td::Ref<vm::Cell> make_empty_test_block(LogicalTime start_lt) {
  vm::AugmentedDictionary out_msgs{256, block::tlb::aug_OutMsgDescrDefault};
  auto dummy = vm::CellBuilder{}.store_zeroes(1).finalize();

  vm::CellBuilder info;
  CHECK(info.store_long_bool(0x9bc7a987U, 32));
  CHECK(info.store_zeroes_bool(32 + 8 + 8));
  CHECK(info.store_long_bool(1, 32));
  CHECK(info.store_zeroes_bool(32 + 104 + 32));
  CHECK(info.store_ulong_rchk_bool(start_lt, 64));
  CHECK(info.store_ulong_rchk_bool(start_lt + 100, 64));
  CHECK(info.store_zeroes_bool(128));
  CHECK(info.store_ref_bool(dummy));

  vm::CellBuilder extra;
  CHECK(extra.store_long_bool(0x4a33f6fd, 32));
  CHECK(extra.store_ref_bool(dummy));
  CHECK(extra.store_ref_bool(out_msgs.get_wrapped_dict_root()));
  CHECK(extra.store_ref_bool(dummy));
  CHECK(extra.store_zeroes_bool(512 + 1));

  vm::CellBuilder block;
  CHECK(block.store_long_bool(0x11ef55aa, 32));
  CHECK(block.store_zeroes_bool(32));
  CHECK(block.store_ref_bool(info.finalize()));
  CHECK(block.store_ref_bool(dummy));
  CHECK(block.store_ref_bool(dummy));
  CHECK(block.store_ref_bool(extra.finalize()));
  return block.finalize();
}

TEST(GlobalBalanceCalculator, FetchWithoutPrevWithAndWithoutStoredDispatchBalance) {
  constexpr int version = 16;
  const auto shard = ShardIdFull{basechainId, shardIdAll};
  auto msg_queue = make_out_queue();

  auto legacy_dispatch = make_fetch_dispatch_queue({{1, {{10, 100, 7, 0, 1}}, false}});
  auto legacy_id = make_block_id(shard, 1);
  auto legacy = fetch_test_shard_state(make_test_shard_state(legacy_id, *msg_queue, &legacy_dispatch), {}, version);
  // The fixture account has no grams. The sole dispatch message carries
  // 100 ng plus 7 forwarding fees; the outgoing queue is empty.
  expect_int(legacy->accounts_balance, 0);
  expect_int(legacy->dispatch_queue_balance, 107);
  CHECK(legacy->dispatch_queue != nullptr);
  CHECK(legacy->msg_queue != nullptr && dictionary_size(*legacy->msg_queue) == 0);

  auto stored_dispatch = make_fetch_dispatch_queue({{1, {{10, 100, 7, 0, 1}}, true}});
  auto stored_id = make_block_id(shard, 2);
  auto stored = fetch_test_shard_state(make_test_shard_state(stored_id, *msg_queue, &stored_dispatch), {}, version);
  // Stored augmentation describes the same 100 + 7 = 107 ng.
  expect_int(stored->accounts_balance, 0);
  expect_int(stored->dispatch_queue_balance, 107);
  CHECK(stored->dispatch_queue == nullptr);

  auto no_dispatch_id = make_block_id(shard, 3);
  auto no_dispatch = fetch_test_shard_state(make_test_shard_state(no_dispatch_id, *msg_queue, nullptr), {}, version);
  // No dispatch dictionary means no dispatch balance.
  expect_int(no_dispatch->dispatch_queue_balance, 0);
  CHECK(no_dispatch->dispatch_queue == nullptr);
}

TEST(GlobalBalanceCalculator, FetchRegular) {
  constexpr int version = 16;
  const auto shard = ShardIdFull{basechainId, shardIdAll};
  const auto prev_id = make_block_id(shard, 1);
  const auto current_id = make_block_id(shard, 2);
  auto prev_msgs = make_out_queue();
  auto current_msgs = make_out_queue();
  auto old_dispatch = make_fetch_dispatch_queue({{1, {{10, 100, 7, 0, 1}}, false}});
  auto new_dispatch = make_fetch_dispatch_queue({{1, {{10, 100, 7, 0, 1}, {20, 200, 11, 0, 2}}, false}});
  auto prev = make_parsed_state(prev_id, std::move(prev_msgs), copy_test_dispatch_queue(old_dispatch), 107);
  auto block = make_empty_test_block(1000);
  auto parsed =
      fetch_test_shard_state(make_test_shard_state(current_id, *current_msgs, &new_dispatch), block, version, {prev});
  // No account funds; retain the 100 + 7 message and add 200 + 11:
  // cached 107 + delta 211 = 318. The empty OutMsgDescr keeps OutMsgQueue empty.
  expect_int(parsed->accounts_balance, 0);
  expect_int(parsed->dispatch_queue_balance, 318);
  CHECK(parsed->dispatch_queue != nullptr);
  CHECK(parsed->msg_queue != nullptr && dictionary_size(*parsed->msg_queue) == 0);

  // A current state with the new augmentation obtains the same balance without
  // retaining the dispatch dictionary or consulting its predecessor.
  auto stored_dispatch = make_fetch_dispatch_queue({{1, {{10, 100, 7, 0, 1}, {20, 200, 11, 0, 2}}, true}});
  auto stored = fetch_test_shard_state(make_test_shard_state(current_id, *current_msgs, &stored_dispatch), block,
                                       version, {prev});
  // The two stored message balances are again (100 + 7) + (200 + 11) = 318.
  expect_int(stored->dispatch_queue_balance, 318);
  CHECK(stored->dispatch_queue == nullptr);
}

TEST(GlobalBalanceCalculator, FetchSplit) {
  constexpr int version = 16;
  const auto parent = ShardIdFull{basechainId, shardIdAll};
  const auto child = shard_child(parent, true);
  const auto prev_id = make_block_id(parent, 1);
  const auto current_id = make_block_id(child, 2);
  auto prev_msgs = make_out_queue();
  auto current_msgs = make_out_queue();
  auto prev_dispatch = make_fetch_dispatch_queue({{1, {{10, 100, 7, 0, 1}}, false}});
  auto current_dispatch = make_fetch_dispatch_queue({{2, {{30, 50, 3, 0, 3}}, false}});
  auto prev = make_parsed_state(prev_id, std::move(prev_msgs), copy_test_dispatch_queue(prev_dispatch), 9999);
  auto parsed = fetch_test_shard_state(make_test_shard_state(current_id, *current_msgs, &current_dispatch),
                                       make_empty_test_block(1000), version, {prev});
  // A split must scan the child's dispatch queue: 50 + 3 = 53 ng.
  // The deliberately wrong parent cache (9999) must not affect that result.
  // Accounts and the outgoing queue are empty of funds/messages.
  expect_int(parsed->accounts_balance, 0);
  expect_int(parsed->dispatch_queue_balance, 53);
  CHECK(parsed->dispatch_queue != nullptr);
  CHECK(parsed->msg_queue != nullptr && dictionary_size(*parsed->msg_queue) == 0);
}

TEST(GlobalBalanceCalculator, FetchMerge) {
  constexpr int version = 16;
  const auto parent = ShardIdFull{basechainId, shardIdAll};
  const auto left = shard_child(parent, true);
  const auto right = shard_child(parent, false);
  const auto left_id = make_block_id(left, 1);
  const auto right_id = make_block_id(right, 1);
  const auto current_id = make_block_id(parent, 2);
  auto left_msgs = make_out_queue();
  auto right_msgs = make_out_queue();
  auto current_msgs = make_out_queue();
  auto left_dispatch = make_fetch_dispatch_queue({{1, {{10, 100, 7, 0, 1}}, false}});
  auto right_dispatch = make_fetch_dispatch_queue({{0x9000000000000000ULL, {{20, 200, 11, 0, 2}}, false}});
  auto current_dispatch = make_fetch_dispatch_queue({
      {1, {{10, 100, 7, 0, 1}}, false},
      {0x9000000000000000ULL, {{20, 200, 11, 0, 2}}, false},
      {3, {{30, 300, 13, 0, 3}}, false},
  });
  auto left_prev = make_parsed_state(left_id, std::move(left_msgs), copy_test_dispatch_queue(left_dispatch), 107);
  auto right_prev = make_parsed_state(right_id, std::move(right_msgs), copy_test_dispatch_queue(right_dispatch), 211);
  auto parsed = fetch_test_shard_state(make_test_shard_state(current_id, *current_msgs, &current_dispatch),
                                       make_empty_test_block(1000), version, {left_prev, right_prev});
  // Merge cached balances 107 + 211, then add the new message's 300 + 13:
  // 107 + 211 + 313 = 631. Both outgoing queues and OutMsgDescr are empty.
  // The fixture account contributes zero grams.
  expect_int(parsed->accounts_balance, 0);
  expect_int(parsed->dispatch_queue_balance, 631);
  CHECK(parsed->dispatch_queue != nullptr);
  CHECK(parsed->msg_queue != nullptr && dictionary_size(*parsed->msg_queue) == 0);
}

}  // namespace
}  // namespace ton::validator

namespace ton::validator {
namespace {

using GbcParsedState = detail::ParsedShardState;

struct GbcMessage {
  td::Ref<vm::Cell> message;
  td::Ref<vm::Cell> envelope;
  td::Bits256 hash;
  td::BitArray<352> queue_key;
  AccountIdPrefixFull next_prefix;
  LogicalTime created_lt;
};

td::Ref<vm::Cell> gbc_dummy_cell(unsigned value = 0) {
  return vm::CellBuilder{}.store_long(value, 32).finalize();
}

StdSmcAddress gbc_address(td::uint64 prefix, unsigned tail) {
  StdSmcAddress result;
  result.set_zero();
  result.bits().store_int(prefix, 64);
  (result.bits() + 64).store_int(tail, 32);
  return result;
}

td::Ref<vm::CellSlice> gbc_grams(td::uint64 amount) {
  vm::CellBuilder cb;
  CHECK(block::tlb::t_Grams.store_integer_value(cb, *td::make_refint(amount)));
  return vm::load_cell_slice_ref(cb.finalize());
}

td::Ref<vm::Cell> gbc_internal_message(td::uint64 src_prefix, td::uint64 dest_prefix, LogicalTime created_lt,
                                       unsigned discriminator) {
  vm::CellBuilder cb;
  CHECK(cb.store_zeroes_bool(1));  // int_msg_info$0
  CHECK(cb.store_ones_bool(1));    // ihr_disabled
  CHECK(cb.store_zeroes_bool(2));  // bounce, bounced
  CHECK(block::tlb::t_MsgAddressInt.store_std_address(cb, basechainId, gbc_address(src_prefix, discriminator)));
  CHECK(block::tlb::t_MsgAddressInt.store_std_address(cb, basechainId, gbc_address(dest_prefix, discriminator + 1)));
  CHECK(block::CurrencyCollection{1000 + discriminator}.store(cb));
  CHECK(block::tlb::t_Grams.store_integer_value(cb, *td::zero_refint()));                   // extra_flags
  CHECK(block::tlb::t_Grams.store_integer_value(cb, *td::make_refint(3 + discriminator)));  // fwd_fee
  CHECK(cb.store_ulong_rchk_bool(created_lt, 64));
  CHECK(cb.store_ulong_rchk_bool(discriminator, 32));
  CHECK(cb.store_zeroes_bool(1));  // no StateInit
  CHECK(cb.store_ones_bool(1));    // body stored by reference
  CHECK(cb.store_ref_bool(gbc_dummy_cell(discriminator)));
  return cb.finalize();
}

td::BitArray<352> gbc_queue_key(AccountIdPrefixFull next_prefix, td::Bits256 msg_hash) {
  td::BitArray<352> key;
  key.bits().store_int(next_prefix.workchain, 32);
  (key.bits() + 32).store_int(next_prefix.account_id_prefix, 64);
  (key.bits() + 96).copy_from(msg_hash.bits(), 256);
  return key;
}

GbcMessage gbc_message(td::uint64 src_prefix, td::uint64 dest_prefix, int cur_addr, int next_addr,
                       LogicalTime created_lt, unsigned discriminator, td::optional<LogicalTime> emitted_lt = {}) {
  GbcMessage result;
  result.message = gbc_internal_message(src_prefix, dest_prefix, created_lt, discriminator);
  block::tlb::MsgEnvelope::Record_std env{cur_addr,       next_addr,  td::make_refint(17 + discriminator),
                                          result.message, emitted_lt, {}};
  CHECK(block::tlb::t_MsgEnvelope.pack_cell(result.envelope, env));
  result.hash = result.message->get_hash().as_bits256();
  auto src = AccountIdPrefixFull{basechainId, src_prefix};
  auto dest = AccountIdPrefixFull{basechainId, dest_prefix};
  result.next_prefix = block::interpolate_addr(src, dest, next_addr);
  result.queue_key = gbc_queue_key(result.next_prefix, result.hash);
  result.created_lt = created_lt;
  return result;
}

td::Ref<vm::CellSlice> gbc_enqueued(const GbcMessage& msg, LogicalTime enqueued_lt) {
  return vm::CellBuilder{}.store_long(enqueued_lt, 64).store_ref(msg.envelope).as_cellslice_ref();
}

void gbc_enqueue(vm::AugmentedDictionary& queue, const GbcMessage& msg, LogicalTime enqueued_lt) {
  CHECK(queue.set(msg.queue_key, gbc_enqueued(msg, enqueued_lt), vm::Dictionary::SetMode::Add));
}

void gbc_expect_enqueued(vm::AugmentedDictionary& queue, const GbcMessage& msg, LogicalTime expected_lt) {
  auto value = queue.lookup(msg.queue_key);
  CHECK(value.not_null());
  block::EnqueuedMsgDescr descr;
  CHECK(descr.unpack(value.write()));
  CHECK(descr.enqueued_lt_ == expected_lt);
  CHECK(descr.hash_ == msg.hash);
}

td::Ref<vm::Cell> gbc_in_msg_tr(const GbcMessage& imported, td::Ref<vm::Cell> out_envelope) {
  td::Ref<vm::Cell> result;
  CHECK(block::gen::t_InMsg.cell_pack_msg_import_tr(result, imported.envelope, std::move(out_envelope), gbc_grams(1)));
  return result;
}

td::Ref<vm::Cell> gbc_out_ext(td::Ref<vm::Cell> message) {
  td::Ref<vm::Cell> result;
  CHECK(block::gen::t_OutMsg.cell_pack_msg_export_ext(result, std::move(message), gbc_dummy_cell(1)));
  return result;
}

td::Ref<vm::Cell> gbc_out_imm(const GbcMessage& msg) {
  td::Ref<vm::Cell> result;
  CHECK(block::gen::t_OutMsg.cell_pack_msg_export_imm(result, msg.envelope, gbc_dummy_cell(2), gbc_dummy_cell(3)));
  return result;
}

td::Ref<vm::Cell> gbc_out_new(const GbcMessage& msg) {
  td::Ref<vm::Cell> result;
  CHECK(block::gen::t_OutMsg.cell_pack_msg_export_new(result, msg.envelope, gbc_dummy_cell(4)));
  return result;
}

td::Ref<vm::Cell> gbc_out_tr(const GbcMessage& msg) {
  td::Ref<vm::Cell> result;
  CHECK(block::gen::t_OutMsg.cell_pack_msg_export_tr(result, msg.envelope, gbc_dummy_cell(5)));
  return result;
}

td::Ref<vm::Cell> gbc_out_deq(const GbcMessage& msg) {
  td::Ref<vm::Cell> result;
  CHECK(block::gen::t_OutMsg.cell_pack_msg_export_deq(result, msg.envelope, 123));
  return result;
}

td::Ref<vm::Cell> gbc_out_deq_short(const GbcMessage& msg) {
  vm::CellBuilder cb;
  CHECK(cb.store_long_bool(0b1101, 4));
  CHECK(cb.store_bits_bool(msg.envelope->get_hash().bits(), 256));
  CHECK(cb.store_long_bool(msg.next_prefix.workchain, 32));
  CHECK(cb.store_ulong_rchk_bool(msg.next_prefix.account_id_prefix, 64));
  CHECK(cb.store_ulong_rchk_bool(124, 64));
  return cb.finalize();
}

td::Ref<vm::Cell> gbc_out_tr_req(const GbcMessage& msg, const GbcMessage& imported) {
  td::Ref<vm::Cell> result;
  CHECK(block::gen::t_OutMsg.cell_pack_msg_export_tr_req(result, msg.envelope, gbc_in_msg_tr(imported, msg.envelope)));
  return result;
}

td::Ref<vm::Cell> gbc_out_deq_imm(const GbcMessage& msg) {
  td::Ref<vm::Cell> result;
  CHECK(block::gen::t_OutMsg.cell_pack_msg_export_deq_imm(result, msg.envelope, gbc_dummy_cell(6)));
  return result;
}

td::Ref<vm::Cell> gbc_out_new_defer(const GbcMessage& msg) {
  td::Ref<vm::Cell> result;
  CHECK(block::gen::t_OutMsg.cell_pack_msg_export_new_defer(result, msg.envelope, gbc_dummy_cell(7)));
  return result;
}

td::Ref<vm::Cell> gbc_out_deferred_tr(const GbcMessage& msg) {
  td::Ref<vm::Cell> result;
  CHECK(block::gen::t_OutMsg.cell_pack_msg_export_deferred_tr(result, msg.envelope, gbc_dummy_cell(8)));
  return result;
}

void gbc_add_out_msg(vm::AugmentedDictionary& descr, td::Bits256 key, td::Ref<vm::Cell> out_msg) {
  CHECK(descr.set(key.bits(), 256, vm::load_cell_slice_ref(std::move(out_msg)), vm::Dictionary::SetMode::Add));
}

td::Ref<vm::Cell> gbc_block(vm::AugmentedDictionary& out_msgs, LogicalTime start_lt) {
  auto dummy = gbc_dummy_cell(0);

  vm::CellBuilder info;
  CHECK(info.store_long_bool(0x9bc7a987U, 32));
  CHECK(info.store_zeroes_bool(32));   // version
  CHECK(info.store_zeroes_bool(8));    // boolean flags
  CHECK(info.store_zeroes_bool(8));    // flags
  CHECK(info.store_long_bool(1, 32));  // seq_no
  CHECK(info.store_zeroes_bool(32));   // vert_seq_no
  CHECK(info.store_zeroes_bool(104));  // ShardIdent (only unpacked here)
  CHECK(info.store_zeroes_bool(32));   // gen_utime
  CHECK(info.store_ulong_rchk_bool(start_lt, 64));
  CHECK(info.store_ulong_rchk_bool(start_lt + 100, 64));
  CHECK(info.store_zeroes_bool(128));
  CHECK(info.store_ref_bool(dummy));  // prev_ref
  auto info_cell = info.finalize();

  vm::CellBuilder extra;
  CHECK(extra.store_long_bool(0x4a33f6fd, 32));
  CHECK(extra.store_ref_bool(dummy));
  CHECK(extra.store_ref_bool(out_msgs.get_wrapped_dict_root()));
  CHECK(extra.store_ref_bool(dummy));
  CHECK(extra.store_zeroes_bool(512));
  CHECK(extra.store_zeroes_bool(1));  // custom:(Maybe ...)
  auto extra_cell = extra.finalize();

  vm::CellBuilder block;
  CHECK(block.store_long_bool(0x11ef55aa, 32));
  CHECK(block.store_zeroes_bool(32));
  CHECK(block.store_ref_bool(info_cell));
  CHECK(block.store_ref_bool(dummy));
  CHECK(block.store_ref_bool(dummy));
  CHECK(block.store_ref_bool(extra_cell));
  return block.finalize();
}

BlockIdExt gbc_block_id(ShardIdFull shard, BlockSeqno seqno) {
  return BlockIdExt{BlockId{shard.workchain, shard.shard, seqno}};
}

std::shared_ptr<GbcParsedState> gbc_prev(ShardIdFull shard, BlockSeqno seqno, vm::AugmentedDictionary queue) {
  auto state = std::make_shared<GbcParsedState>();
  state->block_id = gbc_block_id(shard, seqno);
  state->msg_queue = std::make_unique<vm::AugmentedDictionary>(std::move(queue));
  return state;
}

std::size_t gbc_queue_size(vm::AugmentedDictionary& queue) {
  std::size_t result = 0;
  CHECK(queue
            .check_for_each([&](td::Ref<vm::CellSlice>, td::ConstBitPtr, int) {
              ++result;
              return td::Status::OK();
            })
            .is_ok());
  return result;
}

TEST(GlobalBalanceCalculator, UpdateMessageQueueAllOutMsgTypesRegular) {
  constexpr LogicalTime start_lt = 9000;
  vm::AugmentedDictionary old_queue{352, block::tlb::aug_OutMsgQueue};
  vm::AugmentedDictionary out_msgs{256, block::tlb::aug_OutMsgDescrDefault};

  auto survivor = gbc_message(0x0100000000000000ULL, 0x0200000000000000ULL, 96, 96, 100, 1);
  auto ext = gbc_message(0x0300000000000000ULL, 0x0400000000000000ULL, 96, 96, 101, 2);
  auto imm = gbc_message(0x0500000000000000ULL, 0x0600000000000000ULL, 96, 96, 102, 3);
  auto added_new = gbc_message(0x0700000000000000ULL, 0x0800000000000000ULL, 96, 96, 103, 4);
  auto added_tr = gbc_message(0x0900000000000000ULL, 0x0a00000000000000ULL, 96, 96, 104, 5);
  auto removed_deq = gbc_message(0x0b00000000000000ULL, 0x0c00000000000000ULL, 96, 96, 105, 6);
  auto removed_short = gbc_message(0x0d00000000000000ULL, 0x0e00000000000000ULL, 96, 96, 106, 7);
  auto tr_req_old = gbc_message(0x1100000000000000ULL, 0x1200000000000000ULL, 0, 0, 107, 8);
  auto tr_req_new = gbc_message(0x1100000000000000ULL, 0x1200000000000000ULL, 96, 96, 107, 8);
  auto removed_imm = gbc_message(0x1300000000000000ULL, 0x1400000000000000ULL, 96, 96, 108, 9);
  auto new_defer = gbc_message(0x1500000000000000ULL, 0x1600000000000000ULL, 96, 96, 109, 10);
  auto deferred_tr = gbc_message(0x1700000000000000ULL, 0x1800000000000000ULL, 96, 96, 110, 11, 7777);

  gbc_enqueue(old_queue, survivor, 501);
  gbc_enqueue(old_queue, removed_deq, 502);
  gbc_enqueue(old_queue, removed_short, 503);
  gbc_enqueue(old_queue, tr_req_old, 504);
  gbc_enqueue(old_queue, removed_imm, 505);

  gbc_add_out_msg(out_msgs, ext.hash, gbc_out_ext(ext.message));
  gbc_add_out_msg(out_msgs, imm.hash, gbc_out_imm(imm));
  gbc_add_out_msg(out_msgs, added_new.hash, gbc_out_new(added_new));
  gbc_add_out_msg(out_msgs, added_tr.hash, gbc_out_tr(added_tr));
  gbc_add_out_msg(out_msgs, removed_deq.hash, gbc_out_deq(removed_deq));
  gbc_add_out_msg(out_msgs, removed_short.hash, gbc_out_deq_short(removed_short));
  gbc_add_out_msg(out_msgs, tr_req_new.hash, gbc_out_tr_req(tr_req_new, tr_req_old));
  gbc_add_out_msg(out_msgs, removed_imm.hash, gbc_out_deq_imm(removed_imm));
  gbc_add_out_msg(out_msgs, new_defer.hash, gbc_out_new_defer(new_defer));
  gbc_add_out_msg(out_msgs, deferred_tr.hash, gbc_out_deferred_tr(deferred_tr));

  auto shard = ShardIdFull{basechainId, shardIdAll};
  std::vector<std::shared_ptr<GbcParsedState>> prev;
  prev.push_back(gbc_prev(shard, 1, std::move(old_queue)));
  auto updated = detail::update_message_queue(prev, gbc_block(out_msgs, start_lt), gbc_block_id(shard, 2)).move_as_ok();

  // Five entries remain: the untouched survivor; new and transit messages;
  // tr_req under its NEW routing prefix; and the released deferred message.
  // Their enqueue times are respectively the old 501, created_lt=103,
  // start_lt=9000, start_lt=9000, and emitted_lt=7777.
  CHECK(gbc_queue_size(*updated) == 5);
  gbc_expect_enqueued(*updated, survivor, 501);
  gbc_expect_enqueued(*updated, added_new, added_new.created_lt);
  gbc_expect_enqueued(*updated, added_tr, start_lt);
  gbc_expect_enqueued(*updated, tr_req_new, start_lt);
  gbc_expect_enqueued(*updated, deferred_tr, 7777);
  // Dequeue records remove three messages, and tr_req removes its old key.
  // External, immediately delivered, and newly deferred messages never enter
  // OutMsgQueue (new_defer belongs in DispatchQueue instead).
  CHECK(updated->lookup(removed_deq.queue_key).is_null());
  CHECK(updated->lookup(removed_short.queue_key).is_null());
  CHECK(updated->lookup(tr_req_old.queue_key).is_null());
  CHECK(updated->lookup(removed_imm.queue_key).is_null());
  CHECK(updated->lookup(ext.queue_key).is_null());
  CHECK(updated->lookup(imm.queue_key).is_null());
  CHECK(updated->lookup(new_defer.queue_key).is_null());
}

TEST(GlobalBalanceCalculator, UpdateMessageQueueSplit) {
  constexpr LogicalTime start_lt = 10000;
  auto parent_shard = ShardIdFull{basechainId, shardIdAll};
  auto left_shard = shard_child(parent_shard, true);
  vm::AugmentedDictionary parent_queue{352, block::tlb::aug_OutMsgQueue};
  auto kept = gbc_message(0x1000000000000000ULL, 0x2000000000000000ULL, 96, 96, 201, 21);
  auto filtered = gbc_message(0x9000000000000000ULL, 0xa000000000000000ULL, 96, 96, 202, 22);
  auto added = gbc_message(0x3000000000000000ULL, 0x4000000000000000ULL, 96, 96, 203, 23);
  gbc_enqueue(parent_queue, kept, 601);
  gbc_enqueue(parent_queue, filtered, 602);

  vm::AugmentedDictionary out_msgs{256, block::tlb::aug_OutMsgDescrDefault};
  gbc_add_out_msg(out_msgs, added.hash, gbc_out_new(added));
  std::vector<std::shared_ptr<GbcParsedState>> prev;
  prev.push_back(gbc_prev(parent_shard, 2, std::move(parent_queue)));
  auto updated =
      detail::update_message_queue(prev, gbc_block(out_msgs, start_lt), gbc_block_id(left_shard, 3)).move_as_ok();

  // The left child retains kept (prefix 0x20..., old enqueue time 601),
  // filters out the right child's 0xa0... entry, and adds the new message
  // under prefix 0x40... at its created_lt=203. Exactly two entries remain.
  CHECK(gbc_queue_size(*updated) == 2);
  gbc_expect_enqueued(*updated, kept, 601);
  gbc_expect_enqueued(*updated, added, added.created_lt);
  CHECK(updated->lookup(filtered.queue_key).is_null());
}

TEST(GlobalBalanceCalculator, UpdateMessageQueueMerge) {
  constexpr LogicalTime start_lt = 11000;
  auto parent_shard = ShardIdFull{basechainId, shardIdAll};
  auto left_shard = shard_child(parent_shard, true);
  auto right_shard = shard_child(parent_shard, false);
  vm::AugmentedDictionary left_queue{352, block::tlb::aug_OutMsgQueue};
  vm::AugmentedDictionary right_queue{352, block::tlb::aug_OutMsgQueue};
  auto removed = gbc_message(0x1000000000000000ULL, 0x2000000000000000ULL, 96, 96, 301, 31);
  auto survivor = gbc_message(0x9000000000000000ULL, 0xa000000000000000ULL, 96, 96, 302, 32);
  auto added = gbc_message(0x3000000000000000ULL, 0x4000000000000000ULL, 96, 96, 303, 33);
  gbc_enqueue(left_queue, removed, 701);
  gbc_enqueue(right_queue, survivor, 702);

  vm::AugmentedDictionary out_msgs{256, block::tlb::aug_OutMsgDescrDefault};
  gbc_add_out_msg(out_msgs, removed.hash, gbc_out_deq(removed));
  gbc_add_out_msg(out_msgs, added.hash, gbc_out_new(added));
  std::vector<std::shared_ptr<GbcParsedState>> prev;
  prev.push_back(gbc_prev(left_shard, 3, std::move(left_queue)));
  prev.push_back(gbc_prev(right_shard, 3, std::move(right_queue)));
  auto updated =
      detail::update_message_queue(prev, gbc_block(out_msgs, start_lt), gbc_block_id(parent_shard, 4)).move_as_ok();

  // The merged queue starts with removed + survivor. Dequeue removed and add
  // added: only survivor (old enqueue time 702) and added (created_lt=303)
  // remain, under their original next-address prefixes 0xa0... and 0x40....
  CHECK(gbc_queue_size(*updated) == 2);
  gbc_expect_enqueued(*updated, survivor, 702);
  gbc_expect_enqueued(*updated, added, added.created_lt);
  CHECK(updated->lookup(removed.queue_key).is_null());
}

}  // namespace
}  // namespace ton::validator

namespace ton::validator {
namespace {

struct DispatchMessageSpec {
  LogicalTime lt;
  long long grams;
  long long fwd_fee;
  long long ihr_fee;
  td::uint64 payload;
};

struct DispatchAccountSpec {
  td::uint64 id;
  std::vector<DispatchMessageSpec> messages;
  bool store_total_balance = false;
};

StdSmcAddress make_test_address(td::uint64 id) {
  auto result = StdSmcAddress::zero();
  result.bits().store_uint(id, 64);
  return result;
}

td::Ref<vm::CellSlice> pack_test_grams(long long value) {
  vm::CellBuilder cb;
  CHECK(block::tlb::t_Grams.store_integer_value(cb, *td::make_refint(value)));
  return vm::load_cell_slice_ref(cb.finalize());
}

td::Ref<vm::Cell> make_test_internal_message(const DispatchMessageSpec& spec, const StdSmcAddress& src) {
  block::gen::CommonMsgInfo::Record_int_msg_info info{
      .ihr_disabled = false,
      .bounce = true,
      .bounced = false,
      .src = block::tlb::t_MsgAddressInt.pack_std_address(basechainId, src),
      .dest = block::tlb::t_MsgAddressInt.pack_std_address(basechainId, make_test_address(spec.payload + 1000)),
      .value = block::CurrencyCollection{spec.grams}.pack(),
      .extra_flags = pack_test_grams(spec.ihr_fee),
      .fwd_fee = pack_test_grams(spec.fwd_fee),
      .created_lt = spec.lt,
      .created_at = static_cast<unsigned>(spec.payload),
  };

  // Use a referenced body so prune_message_queue has a branch which it can replace
  // by a pruned cell while preserving the message and dictionary hashes.
  auto body_child = vm::CellBuilder{}.store_long(spec.payload, 64).finalize();
  auto body = vm::CellBuilder{}.store_long(spec.payload, 32).store_ref(body_child).finalize();
  vm::CellBuilder cb;
  CHECK(block::gen::t_CommonMsgInfo.pack(cb, info));
  CHECK(cb.store_zeroes_bool(1));  // init:nothing$0
  CHECK(cb.store_ones_bool(1));    // body:right$1 ^X
  CHECK(cb.store_ref_bool(std::move(body)));
  return cb.finalize();
}

td::Ref<vm::CellSlice> make_test_enqueued_message(const DispatchMessageSpec& spec, const StdSmcAddress& src,
                                                  td::Ref<vm::Cell>* message = nullptr) {
  auto msg = make_test_internal_message(spec, src);
  td::Ref<vm::Cell> envelope;
  CHECK(block::tlb::t_MsgEnvelope.pack_cell(
      envelope, block::tlb::MsgEnvelope::Record_std{0, 0, td::make_refint(0), msg, {}, {}}));
  if (message != nullptr) {
    *message = msg;
  }
  return vm::load_cell_slice_ref(vm::CellBuilder{}.store_long(spec.lt, 64).store_ref(std::move(envelope)).finalize());
}

vm::AugmentedDictionary make_test_dispatch_queue(const std::vector<DispatchAccountSpec>& accounts) {
  vm::AugmentedDictionary result{256, block::tlb::aug_DispatchQueue};
  for (const auto& account_spec : accounts) {
    block::AccountDispatchQueue account_queue;
    long long total = 0;
    const auto address = make_test_address(account_spec.id);
    for (const auto& message : account_spec.messages) {
      auto key = td::BitArray<64>::zero();
      key.bits().store_uint(message.lt, 64);
      CHECK(account_queue.dict.set(key, make_test_enqueued_message(message, address), vm::Dictionary::SetMode::Add));
      ++account_queue.dict_size;
      total += message.grams + message.fwd_fee;
    }
    CHECK(account_queue.dict_size != 0);
    if (account_spec.store_total_balance) {
      account_queue.total_balance = block::CurrencyCollection{total};
    } else {
      account_queue.total_balance.invalidate();
    }
    td::Ref<vm::CellSlice> packed;
    CHECK(account_queue.pack(packed));
    CHECK(result.set(address, std::move(packed), vm::Dictionary::SetMode::Add));
  }
  return result;
}

long long test_int_value(td::Result<td::RefInt256> result) {
  CHECK(result.is_ok());
  auto value = result.move_as_ok();
  CHECK(value.not_null() && value->is_valid());
  return value->to_long();
}

TEST(GlobalBalanceCalculator, CalculateDispatchQueueBalance) {
  const std::vector<DispatchAccountSpec> accounts{
      {1, {{101, 100, 7, 0, 1}, {102, 200, 11, 0, 2}}, false},
      {2, {{201, 300, 13, 0, 3}}, true},
  };

  auto queue = make_test_dispatch_queue(accounts);
  // Account 1: (100 + 7) + (200 + 11) = 318; account 2: 300 + 13 = 313.
  // Total = 318 + 313 = 631, regardless of whether augmentation is stored.
  CHECK(test_int_value(detail::calculate_dispatch_queue_balance(queue, 16)) == 631);

  vm::AugmentedDictionary empty{256, block::tlb::aug_DispatchQueue};
  // No messages contribute to an empty queue's balance.
  CHECK(test_int_value(detail::calculate_dispatch_queue_balance(empty, 16)) == 0);
}

TEST(GlobalBalanceCalculator, CalculateDispatchQueueBalanceDiff) {
  const DispatchAccountSpec old_inner{1, {{10, 100, 7, 0, 10}, {20, 200, 11, 0, 20}}, false};
  const DispatchAccountSpec new_inner{1, {{20, 250, 17, 0, 21}, {40, 400, 19, 0, 40}}, true};

  // Remove lt=10, replace lt=20, and add lt=40. Old = (100+7)+(200+11) = 318;
  // new = (250+17)+(400+19) = 686. Forward/reverse deltas are +368/-368;
  // comparing the same dictionary gives zero.
  auto inner_old = make_test_dispatch_queue({old_inner});
  auto inner_new = make_test_dispatch_queue({new_inner});
  CHECK(test_int_value(detail::calculate_dispatch_queue_balance_diff(inner_old, inner_new, 16)) == 368);
  CHECK(test_int_value(detail::calculate_dispatch_queue_balance_diff(inner_new, inner_old, 16)) == -368);
  CHECK(test_int_value(detail::calculate_dispatch_queue_balance_diff(inner_new, inner_new, 16)) == 0);

  const DispatchAccountSpec removed_account{2, {{30, 300, 13, 0, 30}}, false};
  const DispatchAccountSpec added_account{3, {{50, 500, 23, 0, 50}}, true};

  // Replace account 2 (300 + 13 = 313) with account 3 (500 + 23 = 523).
  // Forward/reverse deltas are 523 - 313 = +210 and -210.
  auto outer_old = make_test_dispatch_queue({removed_account});
  auto outer_new = make_test_dispatch_queue({added_account});
  CHECK(test_int_value(detail::calculate_dispatch_queue_balance_diff(outer_old, outer_new, 16)) == 210);
  CHECK(test_int_value(detail::calculate_dispatch_queue_balance_diff(outer_new, outer_old, 16)) == -210);

  // Exercise all changes together: inner delta 368 + account replacement delta 210 = 578.
  auto combined_old = make_test_dispatch_queue({old_inner, removed_account});
  auto combined_new = make_test_dispatch_queue({new_inner, added_account});
  CHECK(test_int_value(detail::calculate_dispatch_queue_balance_diff(combined_old, combined_new, 16)) == 578);

  // Re-encoding an unchanged per-account dictionary from the old constructor
  // to the constructor with total_balance must not look like a balance change:
  // both contain (600 + 29) + (700 + 31) = 1360, so both deltas are zero.
  const std::vector<DispatchMessageSpec> unchanged_messages{{60, 600, 29, 0, 60}, {70, 700, 31, 0, 70}};
  auto old_encoding = make_test_dispatch_queue({{4, unchanged_messages, false}});
  auto new_encoding = make_test_dispatch_queue({{4, unchanged_messages, true}});
  CHECK(old_encoding.get_wrapped_dict_root()->get_hash() != new_encoding.get_wrapped_dict_root()->get_hash());
  CHECK(test_int_value(detail::calculate_dispatch_queue_balance_diff(old_encoding, new_encoding, 16)) == 0);
  CHECK(test_int_value(detail::calculate_dispatch_queue_balance_diff(new_encoding, old_encoding, 16)) == 0);
}

TEST(GlobalBalanceCalculator, PruneMessageQueue) {
  const std::vector<DispatchMessageSpec> messages{
      {101, 100, 7, 0, 0x11},
      {202, 200, 11, 0, 0x22},
      {303, 300, 13, 0, 0x33},
  };
  vm::AugmentedDictionary queue{352, block::tlb::aug_OutMsgQueue};
  std::vector<td::BitArray<352>> keys;
  std::vector<vm::Cell::Hash> message_hashes;
  for (const auto& message_spec : messages) {
    auto key = td::BitArray<352>::zero();
    key.bits().store_int(basechainId, 32);
    (key.bits() + 32).store_uint(message_spec.payload, 64);
    (key.bits() + 288).store_uint(message_spec.lt, 64);
    td::Ref<vm::Cell> message;
    auto value = make_test_enqueued_message(message_spec, make_test_address(message_spec.payload), &message);
    CHECK(queue.set(key, std::move(value), vm::Dictionary::SetMode::Add));
    keys.push_back(key);
    message_hashes.push_back(message->get_hash());
  }

  const auto root_hash = queue.get_wrapped_dict_root()->get_hash();
  auto pruned_result = detail::prune_message_queue(queue);
  CHECK(pruned_result.is_ok());
  auto pruned = pruned_result.move_as_ok();
  CHECK(pruned != nullptr);
  // All three entries (enqueue times 101, 202, 303) retain the same keys,
  // envelopes, and message hashes. Only referenced bodies become inaccessible.
  CHECK(pruned->get_wrapped_dict_root()->get_hash() == root_hash);
  CHECK(dictionary_size(*pruned) == messages.size());

  for (std::size_t i = 0; i < keys.size(); ++i) {
    auto value = pruned->lookup(keys[i]);
    CHECK(value.not_null());
    auto value_cs = value.write();
    CHECK(value_cs.fetch_ulong(64) == messages[i].lt);
    auto envelope_cell = value_cs.fetch_ref();
    CHECK(envelope_cell.not_null() && value_cs.empty_ext());

    auto envelope_cs = vm::load_cell_slice(envelope_cell);
    block::tlb::MsgEnvelope::Record_std envelope;
    CHECK(block::tlb::t_MsgEnvelope.unpack(envelope_cs, envelope));
    CHECK(envelope_cs.empty_ext());
    CHECK(envelope.msg.not_null() && envelope.msg->get_hash() == message_hashes[i]);

    auto message_cs = vm::load_cell_slice(envelope.msg);
    block::gen::CommonMsgInfo::Record_int_msg_info info;
    CHECK(block::gen::t_CommonMsgInfo.unpack(message_cs, info));
    CHECK(message_cs.fetch_ulong(1) == 0);  // init:nothing$0
    CHECK(message_cs.fetch_ulong(1) == 1);  // body:right$1 ^X
    auto body = message_cs.fetch_ref();
    CHECK(body.not_null() && message_cs.empty_ext());
    CHECK(!body->is_loaded());
    CHECK(vm::load_cell_slice_ref_quiet(body).is_null());
  }
}

TEST(GlobalBalanceCalculator, GetDispatchQueueBalance) {
  auto shard = ShardIdFull{basechainId, shardIdAll};

  // Without a predecessor, scan both messages: (100 + 7) + (200 + 11) = 318.
  auto no_prev_queue = make_test_dispatch_queue({{1, {{10, 100, 7, 0, 1}}, false}, {2, {{20, 200, 11, 0, 2}}, true}});
  CHECK(test_int_value(detail::get_dispatch_queue_balance(no_prev_queue, {}, make_block_id(shard, 1), 16)) == 318);

  // A regular block adds the new message's 200 + 11 = 211 to cached 107,
  // giving 318; the message at lt=10 is unchanged.
  auto regular_old = make_test_dispatch_queue({{1, {{10, 100, 7, 0, 1}}, false}});
  auto regular_new = make_test_dispatch_queue({{1, {{10, 100, 7, 0, 1}, {20, 200, 11, 0, 2}}, true}});
  auto regular_prev = make_parsed_state(make_block_id(shard, 1), {},
                                        std::make_unique<vm::AugmentedDictionary>(std::move(regular_old)), 107);
  CHECK(test_int_value(detail::get_dispatch_queue_balance(regular_new, {regular_prev}, make_block_id(shard, 2), 16)) ==
        318);

  // A split scans the child's sole message, 300 + 13 = 313, instead of
  // adding the parent shard's cached 107.
  auto left_shard = shard_child(shard, true);
  auto split_queue = make_test_dispatch_queue({{0x1000000000000000ULL, {{30, 300, 13, 0, 3}}, false}});
  CHECK(test_int_value(
            detail::get_dispatch_queue_balance(split_queue, {regular_prev}, make_block_id(left_shard, 2), 16)) == 313);

  // Merge cached 107 + 211, retain the left message (100 + 7), and replace
  // the right message (200 + 11) with (300 + 13): 107 + 211 - 211 + 313 = 420.
  auto right_shard = shard_child(shard, false);
  auto left_queue = make_test_dispatch_queue({{0x1000000000000000ULL, {{10, 100, 7, 0, 4}}, false}});
  auto right_queue = make_test_dispatch_queue({{0x9000000000000000ULL, {{20, 200, 11, 0, 5}}, false}});
  auto merged_queue = make_test_dispatch_queue({
      {0x1000000000000000ULL, {{10, 100, 7, 0, 4}}, true},
      {0x9000000000000000ULL, {{30, 300, 13, 0, 6}}, true},
  });
  auto left_prev = make_parsed_state(make_block_id(left_shard, 2), {},
                                     std::make_unique<vm::AugmentedDictionary>(std::move(left_queue)), 107);
  auto right_prev = make_parsed_state(make_block_id(right_shard, 2), {},
                                      std::make_unique<vm::AugmentedDictionary>(std::move(right_queue)), 211);
  CHECK(test_int_value(detail::get_dispatch_queue_balance(merged_queue, {left_prev, right_prev},
                                                          make_block_id(shard, 3), 16)) == 420);

  // With no retained predecessor dictionary, scan both current messages:
  // (100 + 7) + (300 + 13) = 420; the predecessor's zero cache is not used.
  auto unknown_prev = make_parsed_state(make_block_id(shard, 3), {}, {}, 0);
  CHECK(test_int_value(detail::get_dispatch_queue_balance(merged_queue, {unknown_prev}, make_block_id(shard, 4), 16)) ==
        420);
}

TEST(GlobalBalanceCalculator, QueueBalancesWithNonzeroIhrFee) {
  const auto shard = ShardIdFull{basechainId, shardIdAll};
  auto msg_queue = make_out_queue();
  auto first_msg = make_internal_message(1, 2, 100, 3, 17, 101, 1);
  auto second_msg = make_internal_message(1, 3, 200, 2, 23, 102, 2);
  add_out_queue_entry(*msg_queue, first_msg, 2, 101, make_envelope(first_msg, 7));
  add_out_queue_entry(*msg_queue, second_msg, 3, 102, make_envelope(second_msg, 11));
  const auto first_key = make_out_queue_key(first_msg, 2);
  const auto second_key = make_out_queue_key(second_msg, 3);

  // Unknown dispatch augmentation forces calculation from messages in both
  // versions. Keep lt=10, remove lt=15, and add lt=20 in the next state.
  auto old_dispatch = make_test_dispatch_queue({{1, {{10, 300, 13, 1, 1}, {15, 50, 5, 3, 3}}, false}});
  auto new_dispatch = make_test_dispatch_queue({{1, {{10, 300, 13, 1, 1}, {20, 400, 19, 2, 2}}, false}});

  for (int version : {16, 7}) {
    const bool legacy = version == 7;
    // At v16, extra_flags is not money. At v7 the same serialized fields are
    // IHR fees. Values 1..3 are also valid extra_flags in version 16.
    // Old dispatch: (300 + 13) + (50 + 5) = 368; add IHR 1 + 3 at v7 => 372.
    // New dispatch: (300 + 13) + (400 + 19) = 732; add IHR 1 + 2 at v7 => 735.
    // Delta: 732 - 368 = 364 at v16; 735 - 372 = 363 at v7.
    const long long old_dispatch_balance = legacy ? 372 : 368;
    const long long new_dispatch_balance = legacy ? 735 : 732;
    const long long dispatch_delta = legacy ? 363 : 364;
    CHECK(test_int_value(detail::calculate_dispatch_queue_balance(old_dispatch, version)) == old_dispatch_balance);
    CHECK(test_int_value(detail::calculate_dispatch_queue_balance(new_dispatch, version)) == new_dispatch_balance);
    CHECK(test_int_value(detail::calculate_dispatch_queue_balance_diff(old_dispatch, new_dispatch, version)) ==
          dispatch_delta);
    CHECK(test_int_value(detail::calculate_dispatch_queue_balance_diff(new_dispatch, old_dispatch, version)) ==
          -dispatch_delta);

    // Initialization scans the old dispatch queue. The next fetch uses the
    // cached balance plus a dictionary diff; both must use the supplied version.
    auto prev =
        fetch_test_shard_state(make_test_shard_state(make_block_id(shard, 1), *msg_queue, &old_dispatch), {}, version);
    expect_int(prev->dispatch_queue_balance, old_dispatch_balance);
    auto parsed = fetch_test_shard_state(make_test_shard_state(make_block_id(shard, 2), *msg_queue, &new_dispatch),
                                         make_empty_test_block(1000), version, {prev});
    expect_int(parsed->accounts_balance, 0);  // The fixture account has no funds.
    expect_int(parsed->dispatch_queue_balance, new_dispatch_balance);

    // OutMsgDescr is empty: both outgoing entries retain their keys, hashes,
    // and enqueue times 101/102 after initial pruning and incremental fetch.
    for (const auto& state : {prev, parsed}) {
      CHECK(state->dispatch_queue != nullptr);
      CHECK(dictionary_size(*state->msg_queue) == 2);
      CHECK(state->msg_queue->get_wrapped_dict_root()->get_hash() == msg_queue->get_wrapped_dict_root()->get_hash());
      td::RefInt256 out_balance = td::zero_refint();
      for (const auto& key : {first_key, second_key}) {
        auto entry = state->msg_queue->lookup(key);
        CHECK(entry.not_null());
        block::EnqueuedMsgDescr msg;
        CHECK(msg.unpack(entry.write()) && msg.check_key(key.bits()));
        const bool first = msg.hash_ == first_msg->get_hash().as_bits256();
        CHECK(msg.enqueued_lt_ == (first ? 101 : 102));
        auto balance = detail::get_out_queue_message_balance(msg, version).move_as_ok();
        // Outgoing queues count REMAINING forwarding fees (7/11), not the
        // original fees (17/23). First = 100+7 [+3 IHR] = 107/110;
        // second = 200+11 [+2 IHR] = 211/213; total = 318/323.
        expect_int(balance, first ? (legacy ? 110 : 107) : (legacy ? 213 : 211));
        out_balance += balance;
      }
      expect_int(out_balance, legacy ? 323 : 318);
      // Combined queues: old = 318+368 / 323+372 = 686/695;
      // new = 318+732 / 323+735 = 1050/1058. No account funds are added.
      const bool old = state == prev;
      expect_int(out_balance + state->dispatch_queue_balance, old ? (legacy ? 695 : 686) : (legacy ? 1058 : 1050));
    }
  }
}

}  // namespace
}  // namespace ton::validator
