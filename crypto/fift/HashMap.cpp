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
#include "HashMap.h"
#include "td/utils/Random.h"
#include "IntCtx.h"

namespace fift {
using td::Ref;

DictKey::DictKey(vm::StackEntry se) {
  auto tp = tp_ = se.type();
  switch (tp) {
    case Type::t_int:
      ref_ = se.as_int();
      break;
    case Type::t_atom:
      ref_ = se.as_atom();
      break;
    case Type::t_string:
      ref_ = se.as_string_ref();
      break;
    case Type::t_bytes:
      ref_ = se.as_bytes_ref();
      break;
    case Type::t_null:
      break;
    default:
      throw IntError{"unsupported key type"};
  }
  compute_hash();
}

DictKey::operator vm::StackEntry() const& {
  switch (tp_) {
    case Type::t_int:
      return value<td::CntInt256>();
    case Type::t_atom:
      return value<vm::Atom>();
    case Type::t_string:
    case Type::t_bytes:
      return {value<td::Cnt<std::string>>(), tp_ == Type::t_bytes};
    default:
      return {};
  }
}

DictKey::operator vm::StackEntry() && {
  switch (tp_) {
    case Type::t_int:
      return move_value<td::CntInt256>();
    case Type::t_atom:
      return move_value<vm::Atom>();
    case Type::t_string:
    case Type::t_bytes:
      return {move_value<td::Cnt<std::string>>(), tp_ == Type::t_bytes};
    default:
      return {};
  }
}

std::ostream& operator<<(std::ostream& os, const DictKey& dkey) {
  return os << vm::StackEntry(dkey).to_string();
}

int DictKey::cmp_internal(const DictKey& other) const {
  if (tp_ != other.tp_) {
    return tp_ < other.tp_ ? -1 : 1;
  }
  switch (tp_) {
    case Type::t_int:
      return td::cmp(value<td::CntInt256>(), other.value<td::CntInt256>());
    case Type::t_atom: {
      int u = value<vm::Atom>()->index(), v = other.value<vm::Atom>()->index();
      return u == v ? 0 : (u < v ? -1 : 1);
    }
    case Type::t_string:
    case Type::t_bytes:
      return value<td::Cnt<std::string>>()->compare(*other.value<td::Cnt<std::string>>());
    default:
      return 0;
  }
}

int DictKey::cmp(const DictKey& other) const {
  if (hash_ < other.hash_) {
    return -1;
  } else if (hash_ > other.hash_) {
    return 1;
  } else {
    return cmp_internal(other);
  }
}

DictKey::keyhash_t DictKey::compute_str_hash(DictKey::keyhash_t h, const char* str, std::size_t len) {
  const char* end = str + len;
  while (str < end) {
    h = h * StrHash + (unsigned char)*str++;
  }
  return h;
}

DictKey::keyhash_t DictKey::compute_int_hash(td::AnyIntView<> x) {
  keyhash_t h = IntHash0;
  for (int i = 0; i < x.size(); i++) {
    h = h * MixConst3 + x.digits[i];
  }
  return h * MixConst4;
}

DictKey::keyhash_t DictKey::compute_hash() {
  switch (tp_) {
    case Type::t_int:
      return hash_ = compute_int_hash(value<td::CntInt256>()->as_any_int());
    case Type::t_atom:
      return hash_ = value<vm::Atom>()->index() * MixConst1 + MixConst2;
    case Type::t_string:
    case Type::t_bytes: {
      auto ref = value<td::Cnt<std::string>>();
      return hash_ = compute_str_hash(tp_, ref->data(), ref->size());
    }
    default:
      return hash_ = 0;
  }
}

const Hashmap* Hashmap::lookup_key_aux(const Hashmap* root, const DictKey& key) {
  if (key.is_null()) {
    return nullptr;
  }
  while (root) {
    int r = key.cmp(root->key_);
    if (!r) {
      break;
    }
    root = (r < 0 ? root->left_.get() : root->right_.get());
  }
  return root;
}

Ref<Hashmap> Hashmap::lookup_key(Ref<Hashmap> root, const DictKey& key) {
  return Ref<Hashmap>(lookup_key_aux(root.get(), key));
}

vm::StackEntry Hashmap::get_key(Ref<Hashmap> root, const DictKey& key) {
  auto node = lookup_key_aux(root.get(), key);
  if (node) {
    return node->value_;
  } else {
    return {};
  }
}

std::pair<Ref<Hashmap>, vm::StackEntry> Hashmap::get_remove_key(Ref<Hashmap> root, const DictKey& key) {
  if (root.is_null() || key.is_null()) {
    return {std::move(root), {}};
  }
  vm::StackEntry val;
  auto res = root->get_remove_internal(key, val);
  if (val.is_null()) {
    return {std::move(root), {}};
  } else {
    return {std::move(res), std::move(val)};
  }
}

Ref<Hashmap> Hashmap::remove_key(Ref<Hashmap> root, const DictKey& key) {
  if (root.is_null() || key.is_null()) {
    return root;
  }
  vm::StackEntry val;
  auto res = root->get_remove_internal(key, val);
  if (val.is_null()) {
    return root;
  } else {
    return res;
  }
}

Ref<Hashmap> Hashmap::get_remove_internal(const DictKey& key, vm::StackEntry& val) const {
  int r = key.cmp(key_);
  if (!r) {
    val = value_;
    return merge(left_, right_);
  } else if (r < 0) {
    if (left_.is_null()) {
      return {};
    } else {
      auto res = left_->get_remove_internal(key, val);
      if (val.is_null()) {
        return res;
      } else {
        return td::make_ref<Hashmap>(key_, value_, std::move(res), right_, y_);
      }
    }
  } else if (right_.is_null()) {
    return {};
  } else {
    auto res = right_->get_remove_internal(key, val);
    if (val.is_null()) {
      return res;
    } else {
      return td::make_ref<Hashmap>(key_, value_, left_, std::move(res), y_);
    }
  }
}

Ref<Hashmap> Hashmap::merge(Ref<Hashmap> a, Ref<Hashmap> b) {
  if (a.is_null()) {
    return b;
  } else if (b.is_null()) {
    return a;
  } else if (a->y_ > b->y_) {
    auto& aa = a.write();
    aa.right_ = merge(std::move(aa.right_), std::move(b));
    return a;
  } else {
    auto& bb = b.write();
    bb.left_ = merge(std::move(a), std::move(bb.left_));
    return b;
  }
}

Ref<Hashmap> Hashmap::set(Ref<Hashmap> root, const DictKey& key, vm::StackEntry value) {
  if (!key.is_null() && !replace(root, key, value) && !value.is_null()) {
    insert(root, key, value, new_y());
  }
  return root;
}

bool Hashmap::replace(Ref<Hashmap>& root, const DictKey& key, vm::StackEntry value) {
  if (root.is_null() || key.is_null()) {
    return false;
  }
  if (value.is_null()) {
    auto res = root->get_remove_internal(key, value);
    if (value.is_null()) {
      return false;
    } else {
      root = std::move(res);
      return true;
    }
  }
  bool found = false;
  auto res = root->replace_internal(key, std::move(value), found);
  if (found) {
    root = std::move(res);
  }
  return found;
}

Ref<Hashmap> Hashmap::replace_internal(const DictKey& key, const vm::StackEntry& value, bool& found) const {
  int r = key.cmp(key_);
  if (!r) {
    found = true;
    return td::make_ref<Hashmap>(key_, value, left_, right_, y_);
  } else if (r < 0) {
    if (left_.is_null()) {
      found = false;
      return {};
    }
    auto res = left_->replace_internal(key, value, found);
    if (!found) {
      return {};
    }
    return td::make_ref<Hashmap>(key_, value_, std::move(res), right_, y_);
  } else {
    if (right_.is_null()) {
      found = false;
      return {};
    }
    auto res = right_->replace_internal(key, value, found);
    if (!found) {
      return {};
    }
    return td::make_ref<Hashmap>(key_, value_, left_, std::move(res), y_);
  }
}

void Hashmap::insert(Ref<Hashmap>& root, const DictKey& key, vm::StackEntry value, long long y) {
  if (root.is_null()) {
    root = td::make_ref<Hashmap>(key, std::move(value), empty(), empty(), y);
    return;
  }
  if (root->y_ <= y) {
    auto res = split(std::move(root), key);
    root = td::make_ref<Hashmap>(key, std::move(value), std::move(res.first), std::move(res.second), y);
    return;
  }
  int r = key.cmp(root->key_);
  CHECK(r);
  insert(r < 0 ? root.write().left_ : root.write().right_, key, std::move(value), y);
}

std::pair<Ref<Hashmap>, Ref<Hashmap>> Hashmap::split(Ref<Hashmap> root, const DictKey& key, bool cmpv) {
  if (root.is_null()) {
    return {{}, {}};
  }
  int r = key.cmp(root->key_);
  if (r < (int)cmpv) {
    if (root->left_.is_null()) {
      return {{}, std::move(root)};
    }
    auto res = split(root->left_, key, cmpv);
    return {std::move(res.first),
            td::make_ref<Hashmap>(root->key_, root->value_, std::move(res.second), root->right_, root->y_)};
  } else {
    if (root->right_.is_null()) {
      return {std::move(root), {}};
    }
    auto res = split(root->right_, key, cmpv);
    return {td::make_ref<Hashmap>(root->key_, root->value_, root->left_, std::move(res.first), root->y_),
            std::move(res.second)};
  }
}

long long Hashmap::new_y() {
  return td::Random::fast_uint64();
}

bool HashmapIterator::unwind(Ref<Hashmap> root) {
  if (root.is_null()) {
    return false;
  }
  while (true) {
    auto left = root->lr(down_);
    if (left.is_null()) {
      cur_ = std::move(root);
      return true;
    }
    stack_.push_back(std::move(root));
    root = std::move(left);
  }
}

bool HashmapIterator::next() {
  if (cur_.not_null()) {
    cur_ = cur_->rl(down_);
    if (cur_.not_null()) {
      while (true) {
        auto left = cur_->lr(down_);
        if (left.is_null()) {
          return true;
        }
        stack_.push_back(std::move(cur_));
        cur_ = std::move(left);
      }
    }
  }
  if (stack_.empty()) {
    return false;
  }
  cur_ = std::move(stack_.back());
  stack_.pop_back();
  return true;
}

}  // namespace fift
