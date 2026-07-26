#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "td/utils/PersistentTreap.h"
#include "td/utils/Random.h"
#include "td/utils/tests.h"

namespace td {
namespace {

TEST(PersistentTreap, Empty) {
  PersistentTreap<int, int> t;
  EXPECT_EQ(0u, t.size());
  EXPECT(t.empty());
  EXPECT(!t.find(42).has_value());
}

TEST(PersistentTreap, InsertAndFind) {
  PersistentTreap<int, std::string> t;
  auto t1 = t.insert(10, "ten");
  auto t2 = t1.insert(20, "twenty");
  auto t3 = t2.insert(5, "five");

  EXPECT_EQ(0u, t.size());
  EXPECT_EQ(1u, t1.size());
  EXPECT_EQ(2u, t2.size());
  EXPECT_EQ(3u, t3.size());

  EXPECT(t3.find(10).has_value());
  EXPECT_EQ("ten", *t3.find(10));
  EXPECT_EQ("twenty", *t3.find(20));
  EXPECT_EQ("five", *t3.find(5));
  EXPECT(!t3.find(99).has_value());

  // Old versions untouched
  EXPECT(!t1.find(20).has_value());
  EXPECT(!t1.find(5).has_value());
}

TEST(PersistentTreap, InsertOverwrite) {
  PersistentTreap<int, int> t;
  auto t1 = t.insert(1, 100);
  auto t2 = t1.insert(1, 200);

  EXPECT_EQ(1u, t1.size());
  EXPECT_EQ(1u, t2.size());
  EXPECT_EQ(100, *t1.find(1));
  EXPECT_EQ(200, *t2.find(1));
}

TEST(PersistentTreap, Erase) {
  PersistentTreap<int, int> t;
  auto t1 = t.insert(1, 10).insert(2, 20).insert(3, 30);
  EXPECT_EQ(3u, t1.size());

  auto t2 = t1.erase(2);
  EXPECT_EQ(2u, t2.size());
  EXPECT(t2.find(1).has_value());
  EXPECT(!t2.find(2).has_value());
  EXPECT(t2.find(3).has_value());

  // Original untouched
  EXPECT_EQ(3u, t1.size());
  EXPECT(t1.find(2).has_value());

  // Erase non-existent key
  auto t3 = t2.erase(99);
  EXPECT_EQ(2u, t3.size());
}

TEST(PersistentTreap, EraseAt) {
  PersistentTreap<int, int> t;
  auto t1 = t.insert(10, 1).insert(20, 2).insert(30, 3).insert(40, 4).insert(50, 5);
  EXPECT_EQ(5u, t1.size());

  // Elements in sorted order: 10, 20, 30, 40, 50
  // Erase index 2 (key=30)
  auto t2 = t1.erase_at(2);
  EXPECT_EQ(4u, t2.size());
  EXPECT(!t2.find(30).has_value());
  EXPECT(t2.find(10).has_value());
  EXPECT(t2.find(20).has_value());
  EXPECT(t2.find(40).has_value());
  EXPECT(t2.find(50).has_value());

  // Original untouched
  EXPECT_EQ(5u, t1.size());
}

TEST(PersistentTreap, AtRandomAccess) {
  PersistentTreap<int, int> t;
  auto t1 = t.insert(30, 3).insert(10, 1).insert(50, 5).insert(20, 2).insert(40, 4);
  EXPECT_EQ(5u, t1.size());

  // at() returns elements in sorted key order
  auto [k0, v0] = t1.at(0);
  EXPECT_EQ(10, k0);
  EXPECT_EQ(1, v0);

  auto [k1, v1] = t1.at(1);
  EXPECT_EQ(20, k1);

  auto [k2, v2] = t1.at(2);
  EXPECT_EQ(30, k2);

  auto [k3, v3] = t1.at(3);
  EXPECT_EQ(40, k3);

  auto [k4, v4] = t1.at(4);
  EXPECT_EQ(50, k4);
}

TEST(PersistentTreap, SnapshotIsolation) {
  PersistentTreap<int, int> t;
  auto v1 = t.insert(1, 10).insert(2, 20).insert(3, 30);
  auto snapshot = v1;  // O(1) copy

  // Mutate v1
  auto v2 = v1.insert(4, 40);
  auto v3 = v2.erase(1);

  // Snapshot unchanged
  EXPECT_EQ(3u, snapshot.size());
  EXPECT(snapshot.find(1).has_value());
  EXPECT(!snapshot.find(4).has_value());

  // v3 has the mutations
  EXPECT_EQ(3u, v3.size());
  EXPECT(!v3.find(1).has_value());
  EXPECT(v3.find(4).has_value());
}

TEST(PersistentTreap, LargeInsertErase) {
  PersistentTreap<int, int> t;
  constexpr int N = 10000;

  // Insert N elements
  for (int i = 0; i < N; i++) {
    t = t.insert(i, i * 10);
  }
  EXPECT_EQ(static_cast<size_t>(N), t.size());

  // Verify all present
  for (int i = 0; i < N; i++) {
    auto v = t.find(i);
    EXPECT(v.has_value());
    EXPECT_EQ(i * 10, *v);
  }

  // Verify at() order
  for (int i = 0; i < N; i++) {
    auto [k, v] = t.at(i);
    EXPECT_EQ(i, k);
    EXPECT_EQ(i * 10, v);
  }

  // Erase half
  for (int i = 0; i < N; i += 2) {
    t = t.erase(i);
  }
  EXPECT_EQ(static_cast<size_t>(N / 2), t.size());

  // Verify remaining
  for (int i = 0; i < N; i++) {
    auto v = t.find(i);
    if (i % 2 == 0) {
      EXPECT(!v.has_value());
    } else {
      EXPECT(v.has_value());
      EXPECT_EQ(i * 10, *v);
    }
  }
}

TEST(PersistentTreap, EraseAtSmall) {
  PersistentTreap<int, int> t;
  for (int i = 0; i < 5; i++) {
    t = t.insert(i, i);
  }
  EXPECT_EQ(5u, t.size());

  // Erase each index and verify size
  for (int idx = 0; idx < 5; idx++) {
    auto t2 = t.erase_at(idx);
    LOG_CHECK(t2.size() == 4u) << "erase_at(" << idx << ") gave size " << t2.size() << " expected 4";
  }

  // Sequential erase_at(0)
  auto t2 = t;
  for (int i = 0; i < 5; i++) {
    LOG_CHECK(t2.size() == static_cast<size_t>(5 - i)) << "step " << i << " size " << t2.size();
    t2 = t2.erase_at(0);
  }
  EXPECT_EQ(0u, t2.size());
}

TEST(PersistentTreap, RandomInsertEraseAt) {
  PersistentTreap<int, int> t;
  std::set<int> reference;

  for (int i = 0; i < 1000; i++) {
    t = t.insert(i, i);
    reference.insert(i);
  }
  EXPECT_EQ(1000u, t.size());

  td::Random::Xorshift128plus rnd(123);
  while (!t.empty()) {
    size_t idx = rnd() % t.size();
    auto [key, value] = t.at(idx);

    EXPECT(reference.count(key) == 1);
    reference.erase(key);

    // Erase by key (not erase_at) to avoid any rank/ordering issues
    t = t.erase(key);

    EXPECT_EQ(reference.size(), t.size());
  }
  EXPECT(reference.empty());
}

TEST(PersistentTreap, SnapshotRandomDrain) {
  // The actual use case: take snapshot, drain randomly while live tree is modified
  PersistentTreap<int, int> live;
  for (int i = 0; i < 500; i++) {
    live = live.insert(i, i);
  }

  auto snapshot = live;

  // Modify live tree while draining snapshot
  for (int i = 500; i < 1000; i++) {
    live = live.insert(i, i);
  }
  for (int i = 0; i < 250; i++) {
    live = live.erase(i);
  }

  // Snapshot still has original 500 elements
  EXPECT_EQ(500u, snapshot.size());

  // Drain snapshot randomly
  std::vector<int> drained;
  td::Random::Xorshift128plus rnd(456);
  while (!snapshot.empty()) {
    size_t idx = rnd() % snapshot.size();
    auto [key, value] = snapshot.at(idx);
    drained.push_back(key);
    snapshot = snapshot.erase_at(idx);
  }

  // All 500 original keys should appear exactly once
  std::sort(drained.begin(), drained.end());
  EXPECT_EQ(500u, drained.size());
  for (int i = 0; i < 500; i++) {
    EXPECT_EQ(i, drained[i]);
  }

  // Live tree has 750 elements (500 added + 500 original - 250 erased)
  EXPECT_EQ(750u, live.size());
}

TEST(PersistentTreap, StringKeys) {
  PersistentTreap<std::string, int> t;
  t = t.insert("banana", 2);
  t = t.insert("apple", 1);
  t = t.insert("cherry", 3);

  EXPECT_EQ(3u, t.size());
  auto [k0, _] = t.at(0);
  EXPECT_EQ("apple", k0);

  EXPECT_EQ(2, *t.find("banana"));
  EXPECT(!t.find("durian").has_value());
}

// Reference implementation using std::map for stress testing
template <typename K, typename V>
class ReferenceTreap {
  std::map<K, V> data_;

 public:
  ReferenceTreap insert(K key, V value) const {
    auto copy = *this;
    copy.data_[std::move(key)] = std::move(value);
    return copy;
  }
  ReferenceTreap erase(const K& key) const {
    auto copy = *this;
    copy.data_.erase(key);
    return copy;
  }
  ReferenceTreap erase_at(size_t idx) const {
    auto copy = *this;
    auto it = copy.data_.begin();
    std::advance(it, idx);
    copy.data_.erase(it);
    return copy;
  }
  const V* find(const K& key) const {
    auto it = data_.find(key);
    return it != data_.end() ? &it->second : nullptr;
  }
  std::pair<K, V> at(size_t idx) const {
    auto it = data_.begin();
    std::advance(it, idx);
    return {it->first, it->second};
  }
  size_t size() const {
    return data_.size();
  }
  bool empty() const {
    return data_.empty();
  }
};

TEST(PersistentTreap, StressRandomOps) {
  PersistentTreap<int, int> treap;
  ReferenceTreap<int, int> ref;

  td::Random::Xorshift128plus rnd(42);
  constexpr int OPS = 50000;
  constexpr int KEY_RANGE = 1000;

  for (int op = 0; op < OPS; op++) {
    int action = rnd() % 4;
    if (action == 0 || ref.empty()) {
      // Insert
      int key = rnd() % KEY_RANGE;
      int val = rnd() % 10000;
      treap = treap.insert(key, val);
      ref = ref.insert(key, val);
    } else if (action == 1) {
      // Erase by key
      int key = rnd() % KEY_RANGE;
      treap = treap.erase(key);
      ref = ref.erase(key);
    } else if (action == 2) {
      // Erase at random index
      size_t idx = rnd() % ref.size();
      auto [rk, rv] = ref.at(idx);
      auto [tk, tv] = treap.at(idx);
      LOG_CHECK(rk == tk) << "at(" << idx << "): ref key=" << rk << " treap key=" << tk;
      LOG_CHECK(rv == tv) << "at(" << idx << "): ref val=" << rv << " treap val=" << tv;
      treap = treap.erase_at(idx);
      ref = ref.erase_at(idx);
    } else {
      // Find random key
      int key = rnd() % KEY_RANGE;
      auto tv = treap.find(key);
      auto rv = ref.find(key);
      if (!rv) {
        LOG_CHECK(!tv) << "find(" << key << "): ref=null treap=" << *tv;
      } else {
        LOG_CHECK(tv.has_value()) << "find(" << key << "): ref=" << *rv << " treap=null";
        LOG_CHECK(*tv == *rv) << "find(" << key << "): ref=" << *rv << " treap=" << *tv;
      }
    }
    LOG_CHECK(treap.size() == ref.size())
        << "op " << op << " action " << action << ": treap size=" << treap.size() << " ref size=" << ref.size();
  }
}

TEST(PersistentTreap, StressSnapshotIsolation) {
  PersistentTreap<int, int> treap;
  ReferenceTreap<int, int> ref;

  td::Random::Xorshift128plus rnd(99);
  constexpr int KEY_RANGE = 500;

  // Build up some state
  for (int i = 0; i < 500; i++) {
    int key = rnd() % KEY_RANGE;
    treap = treap.insert(key, i);
    ref = ref.insert(key, i);
  }

  // Take snapshots
  auto snap_treap = treap;
  auto snap_ref = ref;

  // Mutate live versions
  for (int i = 0; i < 1000; i++) {
    int action = rnd() % 3;
    if (action == 0 || ref.empty()) {
      int key = rnd() % KEY_RANGE;
      treap = treap.insert(key, i + 1000);
      ref = ref.insert(key, i + 1000);
    } else {
      int key = rnd() % KEY_RANGE;
      treap = treap.erase(key);
      ref = ref.erase(key);
    }
  }

  // Verify snapshots are untouched
  EXPECT_EQ(snap_treap.size(), snap_ref.size());
  for (size_t i = 0; i < snap_treap.size(); i++) {
    auto [tk, tv] = snap_treap.at(i);
    auto [rk, rv] = snap_ref.at(i);
    LOG_CHECK(tk == rk) << "snapshot at(" << i << "): treap key=" << tk << " ref key=" << rk;
    LOG_CHECK(tv == rv) << "snapshot at(" << i << "): treap val=" << tv << " ref val=" << rv;
  }

  // Verify live versions match
  EXPECT_EQ(treap.size(), ref.size());
  for (size_t i = 0; i < treap.size(); i++) {
    auto [tk, tv] = treap.at(i);
    auto [rk, rv] = ref.at(i);
    LOG_CHECK(tk == rk) << "live at(" << i << "): treap key=" << tk << " ref key=" << rk;
    LOG_CHECK(tv == rv) << "live at(" << i << "): treap val=" << tv << " ref val=" << rv;
  }
}

TEST(PersistentTreap, StressRandomDrain) {
  // Simulate the collator pattern: build, snapshot, drain randomly
  td::Random::Xorshift128plus rnd(777);
  constexpr int N = 5000;

  PersistentTreap<int, int> treap;
  ReferenceTreap<int, int> ref;
  for (int i = 0; i < N; i++) {
    treap = treap.insert(i, i);
    ref = ref.insert(i, i);
  }

  // Drain both randomly, comparing at each step
  while (!treap.empty()) {
    size_t idx = rnd() % treap.size();
    auto [tk, tv] = treap.at(idx);
    auto [rk, rv] = ref.at(idx);
    LOG_CHECK(tk == rk) << "drain at(" << idx << "): treap=" << tk << " ref=" << rk;
    treap = treap.erase_at(idx);
    ref = ref.erase_at(idx);
    LOG_CHECK(treap.size() == ref.size())
        << "after erase_at(" << idx << "): treap=" << treap.size() << " ref=" << ref.size();
  }
}

}  // namespace
}  // namespace td
