/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <memory>
#include <optional>
#include <utility>

#include "td/utils/Random.h"

namespace td {

// A persistent (functional) treap — a randomized BST with structural sharing.
//
// All mutation operations return a new treap, leaving old versions intact.
// Snapshot is O(1) (just copy the handle). Insert/erase/find/at are O(log n).
//
// All mutations are expressed as split + merge.
// Merge uses fresh random bits on each comparison to maintain expected O(log n) height.
template <typename K, typename V>
class PersistentTreap {
  struct Node {
    K key;
    V value;
    std::shared_ptr<const Node> left;
    std::shared_ptr<const Node> right;
    size_t size;

    Node(K k, V v, std::shared_ptr<const Node> l, std::shared_ptr<const Node> r)
        : key(std::move(k)), value(std::move(v)), left(std::move(l)), right(std::move(r)) {
      size = 1 + node_size(left) + node_size(right);
    }
  };

  using NodePtr = std::shared_ptr<const Node>;
  NodePtr root_;

  explicit PersistentTreap(NodePtr root) : root_(std::move(root)) {
  }

  static size_t node_size(const NodePtr& n) {
    return n ? n->size : 0;
  }

  static NodePtr make_node(K key, V value, NodePtr left, NodePtr right) {
    return std::make_shared<const Node>(std::move(key), std::move(value), std::move(left), std::move(right));
  }

  // Split by key: everything < key goes left, everything >= key goes right.
  static std::pair<NodePtr, NodePtr> split(const NodePtr& node, const K& key) {
    if (!node) {
      return {nullptr, nullptr};
    }
    if (node->key < key) {
      auto [rl, rr] = split(node->right, key);
      return {make_node(node->key, node->value, node->left, std::move(rl)), std::move(rr)};
    } else {
      auto [ll, lr] = split(node->left, key);
      return {std::move(ll), make_node(node->key, node->value, std::move(lr), node->right)};
    }
  }

  // Split by rank: [0, idx) goes left, [idx, n) goes right.
  static std::pair<NodePtr, NodePtr> split_by_rank(const NodePtr& node, size_t idx) {
    if (!node) {
      return {nullptr, nullptr};
    }
    size_t left_size = node_size(node->left);
    if (idx <= left_size) {
      auto [ll, lr] = split_by_rank(node->left, idx);
      return {std::move(ll), make_node(node->key, node->value, std::move(lr), node->right)};
    } else {
      auto [rl, rr] = split_by_rank(node->right, idx - left_size - 1);
      return {make_node(node->key, node->value, node->left, std::move(rl)), std::move(rr)};
    }
  }

  // Merge two treaps where all keys in left < all keys in right.
  static NodePtr merge(NodePtr left, NodePtr right) {
    if (!left) {
      return right;
    }
    if (!right) {
      return left;
    }
    if (Random::fast_uint32() % (left->size + right->size) < left->size) {
      return make_node(left->key, left->value, left->left, merge(left->right, std::move(right)));
    } else {
      return make_node(right->key, right->value, merge(std::move(left), right->left), right->right);
    }
  }

  static const K& leftmost_key(const NodePtr& node) {
    auto n = node;
    while (n->left) {
      n = n->left;
    }
    return n->key;
  }

 public:
  PersistentTreap() = default;

  // O(log n). Split into elements < key and elements >= key.
  std::pair<PersistentTreap, PersistentTreap> split(const K& key) const {
    auto [l, r] = split(root_, key);
    return {PersistentTreap(std::move(l)), PersistentTreap(std::move(r))};
  }

  // O(log n). Split into [lo, hi) range and the rest.
  // Returns {below_lo, in_range, at_or_above_hi}.
  std::tuple<PersistentTreap, PersistentTreap, PersistentTreap> split_range(const K& lo, const K& hi) const {
    auto [below, ge_lo] = split(root_, lo);
    auto [in_range, ge_hi] = split(ge_lo, hi);
    return {PersistentTreap(std::move(below)), PersistentTreap(std::move(in_range)), PersistentTreap(std::move(ge_hi))};
  }

  // O(log n). Insert or update.
  PersistentTreap insert(K key, V value) const {
    auto [left, ge] = split(root_, key);
    // The leftmost element in ge has key == key (if it exists).
    // Extract it via split_by_rank to discard, then insert new node.
    auto [mid, right] = split_by_rank(ge, ge ? (leftmost_key(ge) < key || key < leftmost_key(ge) ? 0 : 1) : 0);
    auto single = make_node(std::move(key), std::move(value), nullptr, nullptr);
    return PersistentTreap(merge(merge(std::move(left), std::move(single)), std::move(right)));
  }

  // O(log n). Remove by key. No-op if not found.
  PersistentTreap erase(const K& key) const {
    auto [left, ge] = split(root_, key);
    if (!ge) {
      return PersistentTreap(std::move(left));
    }
    auto& lk = leftmost_key(ge);
    if (key < lk || lk < key) {
      // Key not found
      return PersistentTreap(merge(std::move(left), std::move(ge)));
    }
    // Leftmost of ge is the key — remove it
    auto [_mid, right] = split_by_rank(ge, 1);
    return PersistentTreap(merge(std::move(left), std::move(right)));
  }

  // O(log n). Remove element at rank index.
  PersistentTreap erase_at(size_t index) const {
    CHECK(index < size());
    auto [left, rest] = split_by_rank(root_, index);
    auto [_mid, right] = split_by_rank(rest, 1);
    return PersistentTreap(merge(std::move(left), std::move(right)));
  }

  // O(log n). Lookup. Returns copy of value, or nullopt.
  std::optional<V> find(const K& key) const {
    auto node = root_;
    while (node) {
      if (key < node->key) {
        node = node->left;
      } else if (node->key < key) {
        node = node->right;
      } else {
        return node->value;
      }
    }
    return std::nullopt;
  }

  // O(log n). Get key-value by rank (0-based, sorted order). Returns copies.
  std::pair<K, V> at(size_t index) const {
    CHECK(index < size());
    auto node = root_;
    while (true) {
      size_t left_size = node_size(node->left);
      if (index < left_size) {
        node = node->left;
      } else if (index == left_size) {
        return {node->key, node->value};
      } else {
        index -= left_size + 1;
        node = node->right;
      }
    }
  }

  size_t size() const {
    return node_size(root_);
  }

  bool empty() const {
    return !root_;
  }
};

}  // namespace td
