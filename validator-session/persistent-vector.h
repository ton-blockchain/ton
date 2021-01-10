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

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include <functional>

#include "td/utils/int_types.h"
#include "td/utils/buffer.h"

#include "adnl/utils.hpp"

#include "validator-session-description.h"

namespace ton {

namespace validatorsession {

using HashType = ValidatorSessionDescription::HashType;

template <class T>
inline HashType get_vs_hash(ValidatorSessionDescription& desc, const T& value) {
  return value.get_hash(desc);
}

template <class T>
inline HashType get_vs_hash(ValidatorSessionDescription& desc, const T* value) {
  return value ? value->get_hash(desc) : desc.zero_hash();
}

HashType get_vector_hash(ValidatorSessionDescription& desc, std::vector<HashType>&& value);
HashType get_pair_hash(ValidatorSessionDescription& desc, const HashType& left, const HashType& right);

HashType get_vs_hash(ValidatorSessionDescription& desc, const bool& value);
HashType get_vs_hash(ValidatorSessionDescription& desc, const td::uint32& value);
HashType get_vs_hash(ValidatorSessionDescription& desc, const td::uint64& value);
HashType get_vs_hash(ValidatorSessionDescription& desc, const td::Bits256& value);
HashType get_vs_hash(ValidatorSessionDescription& desc, const td::BufferSlice& value);

template <typename T1, typename T2>
inline HashType get_vs_hash(ValidatorSessionDescription& desc, const std::pair<T1, T2>& value) {
  return get_pair_hash(desc, get_vs_hash(value.first), get_vs_hash(value.second));
}

template <typename T>
inline HashType get_vs_hash(ValidatorSessionDescription& desc, const std::vector<T>& value) {
  std::vector<HashType> v;
  v.resize(value.size());
  for (size_t i = 0; i < value.size(); i++) {
    v[i] = get_vs_hash(desc, value[i]);
  }
  return get_vector_hash(desc, std::move(v));
}
inline HashType get_vs_hash(ValidatorSessionDescription& desc, const std::vector<bool>& value) {
  std::vector<HashType> v;
  v.resize(value.size());
  for (size_t i = 0; i < value.size(); i++) {
    bool b = value[i];
    v[i] = get_vs_hash(desc, b);
  }
  return get_vector_hash(desc, std::move(v));
}

template <typename T>
inline HashType get_vs_hash(ValidatorSessionDescription& desc, td::uint32 size, const T* value) {
  std::vector<HashType> v;
  v.resize(size);
  for (size_t i = 0; i < size; i++) {
    v[i] = get_vs_hash(desc, value[i]);
  }
  return get_vector_hash(desc, std::move(v));
}

inline bool move_to_persistent(ValidatorSessionDescription& desc, bool v) {
  return v;
}

inline td::uint32 move_to_persistent(ValidatorSessionDescription& desc, td::uint32 v) {
  return v;
}

inline td::uint64 move_to_persistent(ValidatorSessionDescription& desc, td::uint64 v) {
  return v;
}

template <typename T>
inline const T* move_to_persistent(ValidatorSessionDescription& desc, const T* v) {
  return T::move_to_persistent(desc, v);
}

template <typename T>
class CntVector : public ValidatorSessionDescription::RootObject {
 public:
  static HashType create_hash(ValidatorSessionDescription& desc, std::vector<T>& value) {
    auto obj = create_tl_object<ton_api::hashable_cntVector>(get_vs_hash(desc, value));
    return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
  }
  static HashType create_hash(ValidatorSessionDescription& desc, td::uint32 size, const T* value) {
    auto obj = create_tl_object<ton_api::hashable_cntVector>(get_vs_hash(desc, size, value));
    return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
  }
  static bool compare(const RootObject* r, td::uint32 size, const T* data, HashType hash) {
    if (!r || r->get_size() < sizeof(CntVector)) {
      return false;
    }
    auto R = static_cast<const CntVector*>(r);
    if (R->data_size_ != size * sizeof(T) || R->hash_ != hash) {
      return false;
    }
    for (td::uint32 i = 0; i < size; i++) {
      if (R->data_[i] != data[i]) {
        return false;
      }
    }
    return true;
  }
  static bool compare(const RootObject* r, const std::vector<T>& data, HashType hash) {
    if (!r || r->get_size() < sizeof(CntVector)) {
      return false;
    }
    auto R = static_cast<const CntVector*>(r);
    if (R->data_size_ != sizeof(T) * data.size() || R->hash_ != hash) {
      return false;
    }
    for (td::uint32 i = 0; i < data.size(); i++) {
      if (R->data_[i] != data[i]) {
        return false;
      }
    }
    return true;
  }
  static const CntVector* lookup(ValidatorSessionDescription& desc, std::vector<T>& value, HashType hash, bool temp) {
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, value, hash)) {
      desc.on_reuse();
      return static_cast<const CntVector*>(r);
    }
    return nullptr;
  }
  static const CntVector* lookup(ValidatorSessionDescription& desc, td::uint32 size, const T* data, HashType hash,
                                 bool temp) {
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, size, data, hash)) {
      desc.on_reuse();
      return static_cast<const CntVector*>(r);
    }
    return nullptr;
  }
  static const CntVector* create(ValidatorSessionDescription& desc, std::vector<T> value) {
    if (value.size() == 0) {
      return nullptr;
    }
    auto hash = create_hash(desc, value);
    auto r = lookup(desc, value, hash, true);
    if (r) {
      return r;
    }

    auto size = static_cast<td::uint32>(value.size());
    auto data = static_cast<T*>(desc.alloc(sizeof(T) * size, 8, true));
    for (td::uint32 i = 0; i < size; i++) {
      data[i] = value[i];
    }

    return new (desc, true) CntVector{desc, size, data, hash};
  }
  static const CntVector* create(ValidatorSessionDescription& desc, td::uint32 size, const T* value) {
    if (!size) {
      return nullptr;
    }
    auto hash = create_hash(desc, size, value);
    auto r = lookup(desc, size, value, hash, true);
    if (r) {
      return r;
    }

    return new (desc, true) CntVector{desc, size, value, hash};
  }
  static const CntVector* move_to_persistent(ValidatorSessionDescription& desc, const CntVector* b) {
    if (desc.is_persistent(b)) {
      return b;
    }
    std::vector<T> v;
    v.resize(b->size());
    for (td::uint32 i = 0; i < b->size(); i++) {
      v[i] = ton::validatorsession::move_to_persistent(desc, b->data_[i]);
    }
    auto r = lookup(desc, v, b->hash_, false);
    if (r) {
      return r;
    }
    auto data = static_cast<T*>(desc.alloc(sizeof(T) * b->size(), 8, false));
    for (td::uint32 i = 0; i < b->size(); i++) {
      data[i] = v[i];
    }

    return new (desc, false) CntVector{desc, b->size(), data, b->hash_};
  }
  static const CntVector* merge(ValidatorSessionDescription& desc, const CntVector* l, const CntVector* r,
                                std::function<T(T, T)> merge_f, bool merge_all = false) {
    if (!merge_all) {
      if (!l) {
        return r;
      }
      if (!r) {
        return l;
      }
      if (l == r) {
        return l;
      }
    }
    auto sz = std::max(l->size(), r->size());
    bool ret_left = true;
    bool ret_right = true;
    for (td::uint32 i = 0; i < sz; i++) {
      if (i >= l->size()) {
        ret_left = false;
        break;
      } else if (i >= r->size()) {
        ret_right = false;
        break;
      } else if (l->at(i) != r->at(i)) {
        if (l->at(i)) {
          ret_right = false;
        }
        if (r->at(i)) {
          ret_left = false;
        }
      }
    }
    if (!merge_all && ret_left) {
      return l;
    }
    if (!merge_all && ret_right) {
      return r;
    }

    auto v = static_cast<T*>(desc.alloc(sizeof(T) * sz, 8, true));
    for (td::uint32 i = 0; i < sz; i++) {
      if (i >= l->size()) {
        if (!merge_all) {
          v[i] = r->at(i);
        } else {
          v[i] = merge_f(r->at(i), r->at(i));
        }
      } else if (i >= r->size()) {
        if (!merge_all) {
          v[i] = l->at(i);
        } else {
          v[i] = merge_f(l->at(i), l->at(i));
        }
      } else {
        v[i] = merge_f(l->at(i), r->at(i));
      }
    }

    return create(desc, sz, v);
  }
  static const CntVector* modify(ValidatorSessionDescription& desc, const CntVector* l, std::function<T(T)> mod_f) {
    if (!l) {
      return l;
    }
    auto sz = l->size();

    auto v = static_cast<T*>(desc.alloc(sizeof(T) * sz, 8, true));
    for (td::uint32 i = 0; i < sz; i++) {
      v[i] = mod_f(l->at(i));
    }

    return create(desc, sz, v);
  }
  static const CntVector* change(ValidatorSessionDescription& desc, const CntVector* l, td::uint32 idx, T value) {
    auto sz = l->size();
    auto v = static_cast<T*>(desc.alloc(sizeof(T) * sz, 8, true));
    std::memcpy(v, l->data_, sizeof(T) * sz);
    v[idx] = std::move(value);
    return create(desc, sz, v);
  }
  static const CntVector* push(ValidatorSessionDescription& desc, const CntVector* l, td::uint32 idx, T value) {
    td::uint32 sz = l ? l->size() : 0;
    CHECK(idx == sz);
    sz++;
    auto v = static_cast<T*>(desc.alloc(sizeof(T) * sz, 8, true));
    if (l) {
      std::memcpy(v, l->data_, sizeof(T) * (sz - 1));
    }
    v[idx] = std::move(value);
    return create(desc, sz, v);
  }
  CntVector(ValidatorSessionDescription& desc, td::uint32 data_size, const T* data, HashType hash)
      : RootObject{sizeof(CntVector)}
      , data_size_(static_cast<td::uint32>(data_size * sizeof(T)))
      , data_(data)
      , hash_(std::move(hash)) {
    desc.update_hash(this, hash_);
  }
  td::uint32 size() const {
    return static_cast<td::uint32>(data_size_ / sizeof(T));
  }
  auto data() const {
    return data_;
  }
  auto get_hash(ValidatorSessionDescription& desc) const {
    return hash_;
  }
  T at(td::uint32 idx) const {
    CHECK(idx < size());
    return data_[idx];
  }
  //const T& at(size_t idx) const;

 private:
  const td::uint32 data_size_;
  const T* data_;
  const HashType hash_;
};

template <>
class CntVector<bool> : public ValidatorSessionDescription::RootObject {
 private:
  static bool get_bit(const td::uint32* value, td::uint32 idx) {
    return (value[idx / 32] & (1u << (idx % 32))) != 0;
  }
  static void set_bit(td::uint32* value, td::uint32 idx, bool v) {
    if (v) {
      value[idx / 32] |= (1u << (idx % 32));
    } else {
      value[idx / 32] &= ~static_cast<td::uint32>((1u << (idx % 32)));
    }
  }

 public:
  static HashType create_hash(ValidatorSessionDescription& desc, std::vector<bool>& value) {
    CHECK(value.size() % 32 == 0);
    auto b = new td::uint32[value.size() / 32];
    for (td::uint32 i = 0; i < value.size(); i++) {
      set_bit(b, i, value[i]);
    }
    auto hash = create_hash(desc, static_cast<td::uint32>(value.size()), b);
    delete[] b;
    return hash;
  }
  static HashType create_hash(ValidatorSessionDescription& desc, td::uint32 size, const td::uint32* value) {
    return desc.compute_hash(td::Slice(reinterpret_cast<const td::uint8*>(value), size / 8));
  }
  static bool compare(const RootObject* r, td::uint32 size, const td::uint32* data, HashType hash) {
    CHECK(size % 32 == 0);
    if (!r || r->get_size() < sizeof(CntVector)) {
      return false;
    }
    auto R = static_cast<const CntVector*>(r);
    if (R->data_size_ != size / 8 || R->hash_ != hash) {
      return false;
    }
    return std::memcmp(R->data_, data, size / 8) == 0;
  }
  static bool compare(const RootObject* r, const std::vector<bool>& data, HashType hash) {
    CHECK(data.size() % 32 == 0);
    if (!r || r->get_size() < sizeof(CntVector)) {
      return false;
    }
    auto R = static_cast<const CntVector*>(r);
    if (R->data_size_ != data.size() / 8 || R->hash_ != hash) {
      return false;
    }
    for (td::uint32 i = 0; i < data.size(); i++) {
      if (get_bit(R->data_, i) != data[i]) {
        return false;
      }
    }
    return true;
  }
  static const CntVector* lookup(ValidatorSessionDescription& desc, std::vector<bool>& value, HashType hash,
                                 bool temp) {
    CHECK(value.size() % 32 == 0);
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, value, hash)) {
      desc.on_reuse();
      return static_cast<const CntVector*>(r);
    }
    return nullptr;
  }
  static const CntVector* lookup(ValidatorSessionDescription& desc, td::uint32 size, const td::uint32* data,
                                 HashType hash, bool temp) {
    CHECK(size % 32 == 0);
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, size, data, hash)) {
      desc.on_reuse();
      return static_cast<const CntVector*>(r);
    }
    return nullptr;
  }
  static const CntVector* create(ValidatorSessionDescription& desc, std::vector<bool> value) {
    if (value.size() == 0) {
      return nullptr;
    }
    if (value.size() % 32) {
      auto new_size = value.size() - value.size() % 32 + 32;
      value.resize(new_size, false);
    }
    auto hash = create_hash(desc, value);
    auto r = lookup(desc, value, hash, true);
    if (r) {
      return r;
    }

    auto size = static_cast<td::uint32>(value.size());
    auto data = static_cast<td::uint32*>(desc.alloc(sizeof(td::uint32) * size / 32, 8, true));
    for (td::uint32 i = 0; i < size; i++) {
      set_bit(data, i, value[i]);
    }

    return new (desc, true) CntVector{desc, size, data, hash};
  }
  static const CntVector* create(ValidatorSessionDescription& desc, td::uint32 size, const td::uint32* value) {
    if (!size) {
      return nullptr;
    }
    CHECK(size % 32 == 0);
    auto hash = create_hash(desc, size, value);
    auto r = lookup(desc, size, value, hash, true);
    if (r) {
      return r;
    }

    return new (desc, true) CntVector{desc, size, value, hash};
  }
  static const CntVector* move_to_persistent(ValidatorSessionDescription& desc, const CntVector* b) {
    if (desc.is_persistent(b)) {
      return b;
    }
    auto r = lookup(desc, b->max_size(), b->data_, b->hash_, false);
    if (r) {
      return r;
    }
    auto data = static_cast<td::uint32*>(desc.alloc(b->data_size_, 8, false));
    std::memcpy(data, b->data_, b->data_size_);

    return new (desc, false) CntVector{desc, b->max_size(), data, b->hash_};
  }
  static const CntVector* merge(ValidatorSessionDescription& desc, const CntVector* l, const CntVector* r) {
    if (!l) {
      return r;
    }
    if (!r) {
      return l;
    }
    if (l == r) {
      return l;
    }
    CHECK(l->max_size() == r->max_size());
    auto sz = l->max_size() / 32;
    bool ret_left = true;
    bool ret_right = true;
    for (td::uint32 i = 0; i < sz; i++) {
      if (l->data_[i] & ~r->data_[i]) {
        ret_right = false;
      }
      if (r->data_[i] & ~l->data_[i]) {
        ret_left = false;
      }
    }
    if (ret_left) {
      return l;
    }
    if (ret_right) {
      return r;
    }
    auto v = static_cast<td::uint32*>(desc.alloc(sz * 4, 8, true));
    for (td::uint32 i = 0; i < sz; i++) {
      v[i] = l->data_[i] | r->data_[i];
    }

    return create(desc, sz * 32, v);
  }
  static const CntVector* merge(ValidatorSessionDescription& desc, const CntVector* l, const CntVector* r,
                                std::function<bool(bool, bool)> merge_f) {
    if (!l) {
      return r;
    }
    if (!r) {
      return l;
    }
    if (l == r) {
      return l;
    }
    auto sz = std::max(l->max_size(), r->max_size());

    auto v = static_cast<td::uint32*>(desc.alloc(sz / 8, 8, true));
    std::memset(v, 0, sz / 8);
    for (td::uint32 i = 0; i < sz; i++) {
      if (i >= l->max_size()) {
        set_bit(v, i, r->at(i));
      } else if (i >= r->max_size()) {
        set_bit(v, i, l->at(i));
      } else {
        set_bit(v, i, merge_f(l->at(i), r->at(i)));
      }
    }

    return create(desc, sz, v);
  }
  static const CntVector* change(ValidatorSessionDescription& desc, const CntVector* l, td::uint32 idx, bool value) {
    if (l->at(idx) == value) {
      return l;
    }
    auto sz = l->max_size();
    auto v = static_cast<td::uint32*>(desc.alloc(sz / 8, 8, true));
    std::memcpy(v, l->data_, l->data_size_);
    set_bit(v, idx, value);
    return create(desc, sz, v);
  }
  CntVector(ValidatorSessionDescription& desc, td::uint32 data_size, const td::uint32* data, HashType hash)
      : RootObject{sizeof(CntVector)}
      , data_size_(static_cast<td::uint32>(data_size / 8))
      , data_(data)
      , hash_(std::move(hash)) {
    desc.update_hash(this, hash_);
    CHECK(data_size % 32 == 0);
  }
  td::uint32 max_size() const {
    return data_size_ * 8;
  }
  auto data() const {
    return data_;
  }
  auto get_hash(ValidatorSessionDescription& desc) const {
    return hash_;
  }
  bool at(td::uint32 idx) const {
    CHECK(idx < max_size());
    return get_bit(data_, idx);
  }
  //const T& at(size_t idx) const;

 private:
  const td::uint32 data_size_;
  const td::uint32* data_;
  const HashType hash_;
};

template <typename T, typename Compare = std::less<T>>
class CntSortedVector : public ValidatorSessionDescription::RootObject {
 public:
  static HashType create_hash(ValidatorSessionDescription& desc, std::vector<T>& value) {
    auto obj = create_tl_object<ton_api::hashable_cntSortedVector>(get_vs_hash(desc, value));
    return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
  }
  static HashType create_hash(ValidatorSessionDescription& desc, td::uint32 size, const T* value) {
    auto obj = create_tl_object<ton_api::hashable_cntSortedVector>(get_vs_hash(desc, size, value));
    return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
  }
  static bool compare(const RootObject* r, td::uint32 size, const T* data, HashType hash) {
    if (!r || r->get_size() < sizeof(CntSortedVector)) {
      return false;
    }
    auto R = static_cast<const CntSortedVector*>(r);
    if (R->data_size_ != size * sizeof(T) || R->hash_ != hash) {
      return false;
    }
    for (td::uint32 i = 0; i < size; i++) {
      if (R->data_[i] != data[i]) {
        return false;
      }
    }
    return true;
  }
  static bool compare(const RootObject* r, const std::vector<T>& data, HashType hash) {
    if (!r || r->get_size() < sizeof(CntSortedVector)) {
      return false;
    }
    auto R = static_cast<const CntSortedVector*>(r);
    if (R->data_size_ != data.size() * sizeof(T) || R->hash_ != hash) {
      return false;
    }
    for (td::uint32 i = 0; i < data.size(); i++) {
      if (R->data_[i] != data[i]) {
        return false;
      }
    }
    return true;
  }
  static const CntSortedVector* lookup(ValidatorSessionDescription& desc, std::vector<T>& value, HashType hash,
                                       bool temp) {
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, value, hash)) {
      desc.on_reuse();
      return static_cast<const CntSortedVector*>(r);
    }
    return nullptr;
  }
  static const CntSortedVector* lookup(ValidatorSessionDescription& desc, td::uint32 size, const T* data, HashType hash,
                                       bool temp) {
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, size, data, hash)) {
      desc.on_reuse();
      return static_cast<const CntSortedVector*>(r);
    }
    return nullptr;
  }
  static const CntSortedVector* create(ValidatorSessionDescription& desc, std::vector<T> value) {
    if (value.size() == 0) {
      return nullptr;
    }
    auto hash = create_hash(desc, value);
    auto r = lookup(desc, value, hash, true);
    if (r) {
      return r;
    }

    auto data_size = static_cast<td::uint32>(value.size());
    auto data = static_cast<T*>(desc.alloc(sizeof(T) * data_size, 8, true));
    for (td::uint32 i = 0; i < data_size; i++) {
      data[i] = value[i];
    }

    return new (desc, true) CntSortedVector{desc, data_size, data, hash};
  }
  static const CntSortedVector* create(ValidatorSessionDescription& desc, td::uint32 size, const T* value) {
    if (size == 0) {
      return nullptr;
    }
    auto hash = create_hash(desc, size, value);
    auto r = lookup(desc, size, value, hash, true);
    if (r) {
      return r;
    }

    return new (desc, true) CntSortedVector{desc, size, value, hash};
  }
  static const CntSortedVector* move_to_persistent(ValidatorSessionDescription& desc, const CntSortedVector* b) {
    if (desc.is_persistent(b)) {
      return b;
    }
    std::vector<T> v;
    v.resize(b->size());
    for (td::uint32 i = 0; i < v.size(); i++) {
      v[i] = ton::validatorsession::move_to_persistent(desc, b->data_[i]);
    }
    auto r = lookup(desc, v, b->hash_, false);
    if (r) {
      return r;
    }
    auto data = static_cast<T*>(desc.alloc(sizeof(T) * v.size(), 8, false));
    for (td::uint32 i = 0; i < v.size(); i++) {
      data[i] = v[i];
    }

    return new (desc, false) CntSortedVector{desc, b->size(), data, b->hash_};
  }
  static const CntSortedVector* merge(ValidatorSessionDescription& desc, const CntSortedVector* l,
                                      const CntSortedVector* r, std::function<T(T, T)> merge_f) {
    if (!l) {
      return r;
    }
    if (!r) {
      return l;
    }
    if (l == r) {
      return l;
    }

    bool ret_left = true;
    bool ret_right = true;

    const T* li = l->data_;
    const T* ri = r->data_;
    td::uint32 lp = 0;
    td::uint32 rp = 0;
    while (lp < l->size() || rp < r->size()) {
      if (lp == l->size()) {
        ret_left = false;
        break;
      } else if (rp == r->size()) {
        ret_right = false;
        break;
      } else {
        if (Compare()(li[lp], ri[rp])) {
          ret_right = false;
          lp++;
        } else if (Compare()(ri[rp], li[lp])) {
          ret_left = false;
          rp++;
        } else {
          if (li[lp++] != ri[rp++]) {
            ret_left = false;
            ret_right = false;
            break;
          }
        }
      }
    }
    if (ret_left) {
      return l;
    }
    if (ret_right) {
      return r;
    }

    std::vector<T> v;

    lp = 0;
    rp = 0;
    while (lp < l->size() || rp < r->size()) {
      if (lp == l->size()) {
        v.push_back(ri[rp++]);
      } else if (rp == r->size()) {
        v.push_back(li[lp++]);
      } else {
        if (Compare()(li[lp], ri[rp])) {
          v.push_back(li[lp++]);
        } else if (Compare()(ri[rp], li[lp])) {
          v.push_back(ri[rp++]);
        } else {
          v.push_back(merge_f(li[lp++], ri[rp++]));
        }
      }
    }

    return CntSortedVector::create(desc, std::move(v));
  }
  /*static const CntSortedVector* merge(ValidatorSessionDescription& desc, const CntSortedVector* l,
                                      const CntSortedVector* r) {
    return merge(desc, l, r, [](T l, T r) { return l; });
  }*/
  static const CntSortedVector* push(ValidatorSessionDescription& desc, const CntSortedVector* v, T value) {
    if (!v) {
      return create(desc, std::vector<T>{value});
    }
    T* res = nullptr;
    td::uint32 res_size = 0;

    td::int32 l = -1;
    td::int32 r = v->size();
    bool found = false;
    while (r - l > 1) {
      auto x = (r + l) / 2;
      if (Compare()(v->at(x), value)) {
        l = x;
      } else if (Compare()(value, v->at(x))) {
        r = x;
      } else {
        if (v->at(x) == value) {
          return v;
        }
        res = static_cast<T*>(desc.alloc(sizeof(T) * v->size(), 8, true));
        std::memcpy(res, v->data(), sizeof(T) * v->size());
        res[x] = value;
        res_size = v->size();
        found = true;
        break;
      }
    }
    if (!found) {
      res = static_cast<T*>(desc.alloc(sizeof(T) * (v->size() + 1), 8, true));
      res_size = v->size() + 1;
      std::memcpy(res, v->data(), sizeof(T) * r);
      res[r] = value;
      std::memcpy(res + r + 1, v->data() + r, sizeof(T) * (v->size() - r));
    }
    return CntSortedVector::create(desc, res_size, res);
  }
  CntSortedVector(ValidatorSessionDescription& desc, td::uint32 data_size, const T* data, HashType hash)
      : RootObject{sizeof(CntSortedVector)}
      , data_size_(static_cast<td::uint32>(data_size * sizeof(T)))
      , data_(data)
      , hash_(std::move(hash)) {
    desc.update_hash(this, hash_);
  }
  td::uint32 size() const {
    return static_cast<td::int32>(data_size_ / sizeof(T));
  }
  auto data() const {
    return data_;
  }
  auto get_hash(ValidatorSessionDescription& desc) const {
    return hash_;
  }
  T at(td::uint32 idx) const {
    CHECK(idx < size());
    return data_[idx];
  }
  //const T& at(size_t idx) const;

 private:
  const td::uint32 data_size_;
  const T* data_;
  const HashType hash_;
};

}  // namespace validatorsession

}  // namespace ton
