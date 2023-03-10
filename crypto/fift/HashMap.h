/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once
#include "common/refcnt.hpp"
#include "vm/stack.hpp"
#include "vm/atom.h"

namespace fift {

using td::Ref;
using td::RefAny;

class DictKey {
 public:
  typedef vm::StackEntry::Type Type;
  typedef unsigned long long keyhash_t;

 private:
  RefAny ref_;
  Type tp_ = Type::t_null;
  keyhash_t hash_ = 0;

  static constexpr keyhash_t IntHash0 = 0xce6ab89d724409ed, MixConst1 = 0xcd5c126501510979,
                             MixConst2 = 0xb8f44d7fd6274ad1, MixConst3 = 0xd08726ea2422e405,
                             MixConst4 = 0x6407d2aeb5039dfb, StrHash = 0x93ff128344add06d;
  keyhash_t compute_hash();
  static keyhash_t compute_str_hash(DictKey::keyhash_t h, const char* str, std::size_t len);
  static keyhash_t compute_int_hash(td::AnyIntView<> x);
  int cmp_internal(const DictKey& other) const;
  template <typename T>
  Ref<T> value() const {
    return Ref<T>{td::static_cast_ref(), ref_};
  }
  template <typename T>
  Ref<T> move_value() {
    return Ref<T>{td::static_cast_ref(), std::move(ref_)};
  }

 public:
  DictKey() : ref_(), tp_(Type::t_null) {
  }
  DictKey(const DictKey& other) = default;
  DictKey(DictKey&& other) = default;
  DictKey& operator=(const DictKey& other) = default;
  DictKey& operator=(DictKey&& other) = default;
  DictKey(Ref<vm::Atom> atom_ref) : ref_(std::move(atom_ref)), tp_(Type::t_atom) {
    compute_hash();
  }
  DictKey(td::RefInt256 int_ref) : ref_(std::move(int_ref)), tp_(Type::t_int) {
    compute_hash();
  }
  explicit DictKey(vm::StackEntry se);
  DictKey(std::string str, bool bytes = false) : ref_(), tp_(bytes ? Type::t_bytes : Type::t_string) {
    ref_ = Ref<td::Cnt<std::string>>{true, std::move(str)};
    compute_hash();
  }
  Type type() const {
    return tp_;
  }
  void swap(DictKey& other) {
    ref_.swap(other.ref_);
    std::swap(tp_, other.tp_);
  }

  operator vm::StackEntry() const&;
  operator vm::StackEntry() &&;
  int cmp(const DictKey& other) const;
  bool operator==(const DictKey& other) const {
    return hash_ == other.hash_ && !cmp_internal(other);
  }
  bool operator!=(const DictKey& other) const {
    return hash_ != other.hash_ || cmp_internal(other);
  }
  bool operator<(const DictKey& other) const {
    return hash_ < other.hash_ || (hash_ == other.hash_ && cmp_internal(other) < 0);
  }
  bool is_null() const {
    return tp_ == Type::t_null;
  }
  bool is_string() const {
    return tp_ == Type::t_string;
  }
};

std::ostream& operator<<(std::ostream& os, const DictKey& dkey);

class Hashmap : public td::CntObject {
  DictKey key_;
  vm::StackEntry value_;
  Ref<Hashmap> left_;
  Ref<Hashmap> right_;
  long long y_;

 public:
  Hashmap(DictKey key, vm::StackEntry value, Ref<Hashmap> left, Ref<Hashmap> right, long long y)
      : key_(std::move(key)), value_(std::move(value)), left_(std::move(left)), right_(std::move(right)), y_(y) {
  }
  Hashmap(const Hashmap& other) = default;
  Hashmap(Hashmap&& other) = default;
  virtual ~Hashmap() {
  }
  Hashmap* make_copy() const override {
    return new Hashmap(*this);
  }
  const DictKey& key() const& {
    return key_;
  }
  DictKey key() && {
    return std::move(key_);
  }
  const vm::StackEntry& value() const& {
    return value_;
  }
  vm::StackEntry value() && {
    return std::move(value_);
  }
  Ref<Hashmap> left() const {
    return left_;
  }
  Ref<Hashmap> right() const {
    return right_;
  }
  Ref<Hashmap> lr(bool branch) const {
    return branch ? right_ : left_;
  }
  Ref<Hashmap> rl(bool branch) const {
    return branch ? left_ : right_;
  }
  static Ref<Hashmap> lookup_key(Ref<Hashmap> root, const DictKey& key);
  template <typename... Args>
  static Ref<Hashmap> lookup(Ref<Hashmap> root, Args&&... args) {
    return lookup_key(std::move(root), DictKey{std::forward<Args>(args)...});
  }
  static vm::StackEntry get_key(Ref<Hashmap> root, const DictKey& key);
  template <typename... Args>
  static vm::StackEntry get(Ref<Hashmap> root, Args&&... args) {
    return get_key(std::move(root), DictKey{std::forward<Args>(args)...});
  }
  static Ref<Hashmap> remove_key(Ref<Hashmap> root, const DictKey& key);
  template <typename... Args>
  static Ref<Hashmap> remove(Ref<Hashmap> root, Args&&... args) {
    return remove_key(std::move(root), DictKey{std::forward<Args>(args)...});
  }
  static std::pair<Ref<Hashmap>, vm::StackEntry> get_remove_key(Ref<Hashmap> root, const DictKey& key);
  template <typename... Args>
  static std::pair<Ref<Hashmap>, vm::StackEntry> get_remove(Ref<Hashmap> root, Args&&... args) {
    return get_remove_key(std::move(root), DictKey{std::forward<Args>(args)...});
  }
  static Ref<Hashmap> set(Ref<Hashmap> root, const DictKey& key, vm::StackEntry value);
  static bool replace(Ref<Hashmap>& root, const DictKey& key, vm::StackEntry value);
  static std::pair<Ref<Hashmap>, Ref<Hashmap>> split(Ref<Hashmap> root, const DictKey& key, bool eq_left = false);
  static Ref<Hashmap> empty() {
    return {};
  }

 private:
  static Ref<Hashmap> merge(Ref<Hashmap> a, Ref<Hashmap> b);
  static const Hashmap* lookup_key_aux(const Hashmap* root, const DictKey& key);
  Ref<Hashmap> get_remove_internal(const DictKey& key, vm::StackEntry& val) const;
  Ref<Hashmap> replace_internal(const DictKey& key, const vm::StackEntry& value, bool& found) const;
  static void insert(Ref<Hashmap>& root, const DictKey& key, vm::StackEntry value, long long y);
  static long long new_y();
};

struct HashmapIdx {
  Ref<Hashmap>& root_;
  DictKey idx_;
  template <typename... Args>
  HashmapIdx(Ref<Hashmap>& root, Args&&... args) : root_(root), idx_(std::forward<Args>(args)...) {
  }
  operator vm::StackEntry() const {
    return Hashmap::get(root_, idx_);
  }
  template <typename T>
  HashmapIdx& operator=(T&& value) {
    root_ = Hashmap::set(root_, idx_, vm::StackEntry(std::forward<T>(value)));
    return *this;
  }
};

class HashmapIterator {
  std::vector<Ref<Hashmap>> stack_;
  Ref<Hashmap> cur_;
  const bool down_{false};
  bool unwind(Ref<Hashmap> root);

 public:
  HashmapIterator() = default;
  HashmapIterator(Ref<Hashmap> root, bool down = false) : down_(down) {
    unwind(std::move(root));
  }
  const Hashmap& operator*() const {
    return *cur_;
  }
  const Hashmap* operator->() const {
    return cur_.get();
  }
  bool eof() {
    return cur_.is_null();
  }
  bool next();
  bool operator<(const HashmapIterator& other) const {
    if (other.cur_.is_null()) {
      return cur_.not_null();
    } else if (cur_.is_null()) {
      return false;
    } else {
      return cur_->key().cmp(other.cur_->key()) * (down_ ? -1 : 1) < 0;
    }
  }
  bool operator==(const HashmapIterator& other) const {
    return other.cur_.is_null() ? cur_.is_null() : (cur_.not_null() && cur_->key() == other.cur_->key());
  }
  bool operator!=(const HashmapIterator& other) const {
    return other.cur_.is_null() ? cur_.not_null() : (cur_.is_null() || cur_->key() != other.cur_->key());
  }
  HashmapIterator& operator++() {
    next();
    return *this;
  }
};

struct HashmapKeeper {
  Ref<Hashmap> root;
  HashmapKeeper() = default;
  HashmapKeeper(Ref<Hashmap> _root) : root(std::move(_root)) {
  }
  Ref<Hashmap> extract() {
    return std::move(root);
  }
  operator Ref<Hashmap>() const& {
    return root;
  }
  operator Ref<Hashmap>() && {
    return std::move(root);
  }
  template <typename... Args>
  HashmapIdx operator[](Args&&... args) {
    return HashmapIdx{root, DictKey{std::forward<Args>(args)...}};
  }
  template <typename... Args>
  vm::StackEntry operator[](Args&&... args) const {
    return Hashmap::get(root, DictKey{std::forward<Args>(args)...});
  }
  vm::StackEntry get_key(const DictKey& key) const {
    return Hashmap::get(root, key);
  }
  template <typename... Args>
  vm::StackEntry get(Args&&... args) const {
    return Hashmap::get(root, DictKey{std::forward<Args>(args)...});
  }
  vm::StackEntry get_remove_key(const DictKey& key) {
    auto res = Hashmap::get_remove_key(root, key);
    root = std::move(res.first);
    return std::move(res.second);
  }
  template <typename... Args>
  vm::StackEntry get_remove(Args&&... args) {
    return get_remove_key(DictKey{std::forward<Args>(args)...});
  }
  bool remove_key(const DictKey& key) {
    auto res = Hashmap::get_remove(root, key);
    root = std::move(res.first);
    return !res.second.is_null();
  }
  template <typename... Args>
  bool remove(Args&&... args) {
    return remove_key(DictKey{std::forward<Args>(args)...});
  }
  template <typename T>
  void set(T key, vm::StackEntry value) {
    root = Hashmap::set(root, DictKey(key), std::move(value));
  }
  template <typename T>
  bool replace(T key, vm::StackEntry value) {
    return Hashmap::replace(root, DictKey(key), std::move(value));
  }
  HashmapIterator begin(bool reverse = false) const {
    return HashmapIterator{root, reverse};
  }
  HashmapIterator end() const {
    return HashmapIterator{};
  }
  HashmapIterator rbegin() const {
    return HashmapIterator{root, true};
  }
  HashmapIterator rend() const {
    return HashmapIterator{};
  }
};

}  // namespace fift
