/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
*/

#include <deque>
#include <memory>
#include <vector>

#include "td/utils/tests.h"
#include "validator/impl/expiry-ordered-list.hpp"
#include "validator/impl/ext-message-pool.hpp"
#include "validator/impl/external-message.hpp"

namespace ton::validator {

struct ExtMessagePoolTestPeer {
  using EntryPtr = std::shared_ptr<ExtMessagePool::MempoolMsg>;

  static std::unique_ptr<ExtMessagePool> make_pool() {
    auto options = ValidatorManagerOptions::create(BlockIdExt{}, BlockIdExt{});
    return std::make_unique<ExtMessagePool>(std::move(options), td::actor::ActorId<ValidatorManager>{});
  }

  static td::Ref<ExtMessage> make_message(td::uint32 id) {
    auto hash = Bits256::zero();
    hash.bits().store_uint(id, 32);
    StdSmcAddress address = hash;
    td::Ref<ExtMessageQ> message{
        true, td::BufferSlice{}, td::Ref<vm::Cell>{}, AccountIdPrefixFull{basechainId, id}, basechainId, address, hash,
        hash};
    return std::move(message);
  }

  static metrics::ExtMessageAdmissionOutcome add(ExtMessagePool &pool, td::Ref<ExtMessage> message, int priority,
                                                 td::Timestamp &alarm) {
    return pool.add_message_to_mempool(std::move(message), priority, alarm);
  }

  static metrics::ExtMessageAdmissionOutcome admit(ExtMessagePool &pool, td::Ref<ExtMessage> message, int priority,
                                                   bool add_to_mempool, td::Timestamp &alarm) {
    return pool.finalize_admission(std::move(message), priority, add_to_mempool, alarm);
  }

  static EntryPtr find(ExtMessagePool &pool, ExtMessage::Hash hash) {
    auto index = pool.ext_messages_hashes_.find(hash);
    CHECK(index != pool.ext_messages_hashes_.end());
    auto priority = pool.ext_msgs_.find(index->second.first);
    CHECK(priority != pool.ext_msgs_.end());
    auto entry = priority->second.ext_messages_.find(index->second.second);
    CHECK(entry);
    return entry.value();
  }

  static std::vector<ExtMessage::Hash> expiry_order(const ExtMessagePool &pool) {
    std::vector<ExtMessage::Hash> result;
    for (auto *entry = pool.expiry_order_.oldest(); entry != nullptr; entry = entry->newer) {
      result.push_back(entry->message->hash());
    }
    return result;
  }

  static int priority(const ExtMessagePool &pool, ExtMessage::Hash hash) {
    auto it = pool.ext_messages_hashes_.find(hash);
    CHECK(it != pool.ext_messages_hashes_.end());
    return it->second.first;
  }

  static td::uint64 state(const ExtMessagePool &pool, metrics::ExtMessageState state) {
    return pool.ext_message_states_.value(state);
  }

  static td::uint64 state_total(const ExtMessagePool &pool) {
    return pool.ext_message_states_.total();
  }

  static void postpone(ExtMessagePool &pool, ExtMessage::Hash hash) {
    pool.complete_external_messages({hash}, {});
  }

  static bool prepare_for_collation(ExtMessagePool &pool, const EntryPtr &entry) {
    return pool.prepare_message_for_collation(entry.get());
  }

  static void apply(ExtMessagePool &pool, ExtMessage::Hash normalized_hash) {
    pool.erase_external_messages(BlockIdExt{}, 1, {normalized_hash});
  }

  static size_t expire(ExtMessagePool &pool, td::Timestamp now) {
    return pool.cleanup_expired_messages(now);
  }

  static td::uint64 removed(const ExtMessagePool &pool, metrics::ExtMessageRemovalReason reason) {
    return pool.removal_reasons_[static_cast<size_t>(reason)];
  }

  static td::uint64 admitted(const ExtMessagePool &pool, metrics::ExtMessageAdmissionOutcome outcome) {
    return pool.admission_outcomes_[static_cast<size_t>(outcome)];
  }

  static td::uint64 removed_total(const ExtMessagePool &pool) {
    td::uint64 result = 0;
    for (auto count : pool.removal_reasons_) {
      result += count;
    }
    return result;
  }
};

namespace {

using PoolPeer = ExtMessagePoolTestPeer;

TEST(ExtMessagePool, ReprioritizationReplacesEntryAndMovesItToExpiryTail) {
  auto pool = PoolPeer::make_pool();
  auto first = PoolPeer::make_message(1);
  auto second = PoolPeer::make_message(2);
  auto first_hash = first->hash();
  auto second_hash = second->hash();
  auto alarm = td::Timestamp::never();

  EXPECT_EQ(PoolPeer::add(*pool, first, 1, alarm), metrics::ExtMessageAdmissionOutcome::accepted);
  auto old_entry = PoolPeer::find(*pool, first_hash);
  auto old_deadline = old_entry->delete_at;
  EXPECT_EQ(alarm, old_deadline);
  EXPECT_EQ(PoolPeer::add(*pool, second, 1, alarm), metrics::ExtMessageAdmissionOutcome::accepted);
  EXPECT_EQ(alarm, old_deadline);
  EXPECT_EQ(PoolPeer::add(*pool, first, 2, alarm), metrics::ExtMessageAdmissionOutcome::reprioritized);
  EXPECT_EQ(alarm, old_deadline);

  auto replacement = PoolPeer::find(*pool, first_hash);
  auto order = PoolPeer::expiry_order(*pool);
  ASSERT_EQ(order.size(), 2u);
  EXPECT(old_entry.get() != replacement.get());
  EXPECT(old_deadline <= replacement->delete_at);
  EXPECT_EQ(order[0], second_hash);
  EXPECT_EQ(order[1], first_hash);
  EXPECT_EQ(PoolPeer::priority(*pool, first_hash), 2);
  EXPECT_EQ(PoolPeer::state_total(*pool), 2u);
}

TEST(ExtMessagePool, DuplicateDoesNotRefreshDeadlineOrReplaceEntry) {
  auto pool = PoolPeer::make_pool();
  auto message = PoolPeer::make_message(3);
  auto hash = message->hash();
  auto alarm = td::Timestamp::never();

  EXPECT_EQ(PoolPeer::add(*pool, message, 1, alarm), metrics::ExtMessageAdmissionOutcome::accepted);
  auto original = PoolPeer::find(*pool, hash);
  auto deadline = original->delete_at;
  EXPECT_EQ(PoolPeer::add(*pool, message, 1, alarm), metrics::ExtMessageAdmissionOutcome::duplicate);
  EXPECT_EQ(PoolPeer::add(*pool, message, 0, alarm), metrics::ExtMessageAdmissionOutcome::duplicate);

  auto stored = PoolPeer::find(*pool, hash);
  auto order = PoolPeer::expiry_order(*pool);
  ASSERT_EQ(order.size(), 1u);
  EXPECT_EQ(stored.get(), original.get());
  EXPECT_EQ(stored->delete_at, deadline);
  EXPECT_EQ(order[0], hash);
  EXPECT_EQ(PoolPeer::state_total(*pool), 1u);
}

TEST(ExtMessagePool, StateCountsFollowActualPostponeAndLazyReactivation) {
  auto pool = PoolPeer::make_pool();
  auto message = PoolPeer::make_message(4);
  auto hash = message->hash();
  auto alarm = td::Timestamp::never();
  ASSERT_EQ(PoolPeer::add(*pool, message, 1, alarm), metrics::ExtMessageAdmissionOutcome::accepted);
  auto entry = PoolPeer::find(*pool, hash);

  EXPECT_EQ(PoolPeer::state(*pool, metrics::ExtMessageState::eligible), 1u);
  EXPECT_EQ(PoolPeer::state(*pool, metrics::ExtMessageState::postponed), 0u);
  PoolPeer::postpone(*pool, hash);
  EXPECT_EQ(PoolPeer::state(*pool, metrics::ExtMessageState::eligible), 0u);
  EXPECT_EQ(PoolPeer::state(*pool, metrics::ExtMessageState::postponed), 1u);

  EXPECT(PoolPeer::prepare_for_collation(*pool, entry));
  EXPECT_EQ(PoolPeer::state(*pool, metrics::ExtMessageState::eligible), 1u);
  EXPECT_EQ(PoolPeer::state(*pool, metrics::ExtMessageState::postponed), 0u);
}

TEST(ExtMessagePool, RetainedSnapshotEntryCannotChangeCountsAfterRemoval) {
  auto pool = PoolPeer::make_pool();
  auto message = PoolPeer::make_message(5);
  auto hash = message->hash();
  auto alarm = td::Timestamp::never();
  ASSERT_EQ(PoolPeer::add(*pool, message, 1, alarm), metrics::ExtMessageAdmissionOutcome::accepted);
  auto retained = PoolPeer::find(*pool, hash);
  PoolPeer::postpone(*pool, hash);

  PoolPeer::apply(*pool, message->hash_norm());
  ASSERT_EQ(PoolPeer::state_total(*pool), 0u);
  EXPECT(PoolPeer::prepare_for_collation(*pool, retained));
  EXPECT_EQ(PoolPeer::state_total(*pool), 0u);
}

TEST(ExtMessagePool, AppliedAndExpiredEntriesRecordExactlyOneDistinctRemoval) {
  auto applied_pool = PoolPeer::make_pool();
  auto applied = PoolPeer::make_message(6);
  auto alarm = td::Timestamp::never();
  ASSERT_EQ(PoolPeer::add(*applied_pool, applied, 1, alarm), metrics::ExtMessageAdmissionOutcome::accepted);
  auto applied_deadline = PoolPeer::find(*applied_pool, applied->hash())->delete_at;
  PoolPeer::apply(*applied_pool, applied->hash_norm());
  EXPECT_EQ(PoolPeer::expire(*applied_pool, applied_deadline), 0u);

  EXPECT_EQ(PoolPeer::state_total(*applied_pool), 0u);
  EXPECT_EQ(PoolPeer::removed_total(*applied_pool), 1u);
  EXPECT_EQ(PoolPeer::removed(*applied_pool, metrics::ExtMessageRemovalReason::applied), 1u);
  EXPECT_EQ(PoolPeer::removed(*applied_pool, metrics::ExtMessageRemovalReason::expired), 0u);

  auto expired_pool = PoolPeer::make_pool();
  auto expired = PoolPeer::make_message(7);
  alarm = td::Timestamp::never();
  ASSERT_EQ(PoolPeer::add(*expired_pool, expired, 1, alarm), metrics::ExtMessageAdmissionOutcome::accepted);
  auto deadline = PoolPeer::find(*expired_pool, expired->hash())->delete_at;
  EXPECT_EQ(PoolPeer::expire(*expired_pool, deadline), 1u);
  PoolPeer::apply(*expired_pool, expired->hash_norm());

  EXPECT_EQ(PoolPeer::state_total(*expired_pool), 0u);
  EXPECT_EQ(PoolPeer::removed_total(*expired_pool), 1u);
  EXPECT_EQ(PoolPeer::removed(*expired_pool, metrics::ExtMessageRemovalReason::applied), 0u);
  EXPECT_EQ(PoolPeer::removed(*expired_pool, metrics::ExtMessageRemovalReason::expired), 1u);
}

TEST(ExtMessagePool, AdmissionAndRemovalCountersReconcileWithStoredStock) {
  auto pool = PoolPeer::make_pool();
  auto first = PoolPeer::make_message(8);
  auto second = PoolPeer::make_message(9);
  auto validated_only = PoolPeer::make_message(10);
  auto alarm = td::Timestamp::never();
  auto expect_identity = [&] {
    EXPECT_EQ(PoolPeer::state_total(*pool), PoolPeer::admitted(*pool, metrics::ExtMessageAdmissionOutcome::accepted) -
                                                PoolPeer::removed_total(*pool));
  };

  EXPECT_EQ(PoolPeer::admit(*pool, first, 1, true, alarm), metrics::ExtMessageAdmissionOutcome::accepted);
  expect_identity();
  EXPECT_EQ(PoolPeer::admit(*pool, second, 1, true, alarm), metrics::ExtMessageAdmissionOutcome::accepted);
  expect_identity();
  EXPECT_EQ(PoolPeer::admit(*pool, first, 2, true, alarm), metrics::ExtMessageAdmissionOutcome::reprioritized);
  expect_identity();
  EXPECT_EQ(PoolPeer::admit(*pool, validated_only, 1, false, alarm),
            metrics::ExtMessageAdmissionOutcome::validated_only);
  expect_identity();

  PoolPeer::apply(*pool, first->hash_norm());
  expect_identity();
  auto second_deadline = PoolPeer::find(*pool, second->hash())->delete_at;
  EXPECT_EQ(PoolPeer::expire(*pool, second_deadline), 1u);
  expect_identity();

  EXPECT_EQ(PoolPeer::admitted(*pool, metrics::ExtMessageAdmissionOutcome::accepted), 2u);
  EXPECT_EQ(PoolPeer::admitted(*pool, metrics::ExtMessageAdmissionOutcome::reprioritized), 1u);
  EXPECT_EQ(PoolPeer::admitted(*pool, metrics::ExtMessageAdmissionOutcome::validated_only), 1u);
  EXPECT_EQ(PoolPeer::removed_total(*pool), 2u);
  EXPECT_EQ(PoolPeer::state_total(*pool), 0u);
}

}  // namespace
}  // namespace ton::validator

namespace ton::validator::detail {
namespace {

struct Entry {
  int id;
  const td::Timestamp delete_at;
  Entry *older{nullptr};
  Entry *newer{nullptr};
};

TEST(ExpiryOrderedList, PopsExpiredHeadInOrderExactlyOnce) {
  auto now = td::Timestamp::now();
  Entry first{1, now - 3.0};
  Entry second{2, now - 1.0};
  Entry future{3, now + 10.0};
  ExpiryOrderedList<Entry> list;
  list.append(&first);
  list.append(&second);
  list.append(&future);

  std::vector<int> removed;
  auto count = list.pop_expired(now, [&](Entry *entry) {
    removed.push_back(entry->id);
    list.unlink(entry);
    return true;
  });

  ASSERT_EQ(count, 2u);
  ASSERT_EQ(removed.size(), 2u);
  EXPECT_EQ(removed[0], 1);
  EXPECT_EQ(removed[1], 2);
  EXPECT_EQ(list.oldest(), &future);
  EXPECT_EQ(list.newest(), &future);
  EXPECT_EQ(future.older, nullptr);
  EXPECT_EQ(future.newer, nullptr);
}

TEST(ExpiryOrderedList, ReprioritizedReplacementMovesToTail) {
  auto now = td::Timestamp::now();
  Entry old_first{1, now + 1.0};
  Entry second{2, now + 2.0};
  Entry replacement{1, now + 3.0};
  ExpiryOrderedList<Entry> list;
  list.append(&old_first);
  list.append(&second);

  list.unlink(&old_first);
  list.append(&replacement);

  EXPECT_EQ(list.oldest(), &second);
  EXPECT_EQ(list.newest(), &replacement);
  EXPECT_EQ(second.older, nullptr);
  EXPECT_EQ(second.newer, &replacement);
  EXPECT_EQ(replacement.older, &second);
  EXPECT_EQ(replacement.newer, nullptr);
  EXPECT_EQ(old_first.older, nullptr);
  EXPECT_EQ(old_first.newer, nullptr);
}

TEST(ExpiryOrderedList, UnlinkPreservesHeadMiddleAndTailIntegrity) {
  auto now = td::Timestamp::now();
  Entry first{1, now + 1.0};
  Entry middle{2, now + 2.0};
  Entry third{3, now + 3.0};
  Entry last{4, now + 4.0};
  ExpiryOrderedList<Entry> list;
  list.append(&first);
  list.append(&middle);
  list.append(&third);
  list.append(&last);
  EXPECT(list.contains(&first));
  EXPECT(list.contains(&middle));
  EXPECT(list.contains(&third));
  EXPECT(list.contains(&last));

  list.unlink(&middle);
  EXPECT(!list.contains(&middle));
  EXPECT_EQ(first.newer, &third);
  EXPECT_EQ(third.older, &first);
  EXPECT_EQ(middle.older, nullptr);
  EXPECT_EQ(middle.newer, nullptr);

  list.unlink(&first);
  EXPECT(!list.contains(&first));
  EXPECT_EQ(list.oldest(), &third);
  EXPECT_EQ(third.older, nullptr);

  list.unlink(&last);
  EXPECT(!list.contains(&last));
  EXPECT_EQ(list.newest(), &third);
  EXPECT_EQ(third.newer, nullptr);

  list.unlink(&third);
  EXPECT(!list.contains(&third));
  EXPECT(list.empty());
  EXPECT_EQ(list.oldest(), nullptr);
  EXPECT_EQ(list.newest(), nullptr);
}

TEST(ExpiryOrderedList, RelaxesAlarmToHeadWithoutOverwritingEarlierDeadline) {
  auto now = td::Timestamp::now();
  auto existing = now + 30.0;
  auto alarm = existing;
  ExpiryOrderedList<Entry> empty;
  empty.relax_alarm(alarm);
  EXPECT_EQ(alarm, existing);

  Entry first{1, now + 10.0};
  Entry second{2, now + 20.0};
  ExpiryOrderedList<Entry> list;
  list.append(&first);
  list.append(&second);

  alarm = td::Timestamp::never();
  list.relax_alarm(alarm);
  EXPECT_EQ(alarm, first.delete_at);

  auto earlier = now + 1.0;
  alarm = earlier;
  list.relax_alarm(alarm);
  EXPECT_EQ(alarm, earlier);
}

TEST(ExpiryOrderedList, LargeUnexpiredListDoesNoRemovalWork) {
  constexpr size_t entry_count = 100000;
  auto now = td::Timestamp::now();
  std::deque<Entry> entries;
  ExpiryOrderedList<Entry> list;
  for (size_t i = 0; i < entry_count; ++i) {
    entries.push_back(Entry{static_cast<int>(i), now + 60.0 + static_cast<double>(i)});
    list.append(&entries.back());
  }

  size_t erase_calls = 0;
  auto removed = list.pop_expired(now, [&](Entry *) {
    ++erase_calls;
    return false;
  });

  EXPECT_EQ(removed, 0u);
  EXPECT_EQ(erase_calls, 0u);
  EXPECT_EQ(list.oldest(), &entries.front());
  EXPECT_EQ(list.newest(), &entries.back());
}

}  // namespace
}  // namespace ton::validator::detail
