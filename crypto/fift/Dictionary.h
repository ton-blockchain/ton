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
#include "Continuation.h"
#include "HashMap.h"
#include "vm/box.hpp"

namespace fift {
using td::Ref;

struct IntCtx;

/*
 *
 *    WORD CLASSES
 *
 */

class DictEntry {
  Ref<FiftCont> def;
  bool active{false};

 public:
  DictEntry() = default;
  DictEntry(const DictEntry& ref) = default;
  DictEntry(DictEntry&& ref) = default;
  DictEntry(Ref<FiftCont> _def, bool _act = false) : def(std::move(_def)), active(_act) {
  }
  DictEntry(StackWordFunc func);
  DictEntry(CtxWordFunc func, bool _act = false);
  DictEntry(CtxTailWordFunc func, bool _act = false);
  //DictEntry(const std::vector<Ref<FiftCont>>& word_list);
  //DictEntry(std::vector<Ref<FiftCont>>&& word_list);
  DictEntry& operator=(const DictEntry&) = default;
  DictEntry& operator=(DictEntry&&) = default;
  static DictEntry create_from(vm::StackEntry se);
  explicit operator vm::StackEntry() const&;
  explicit operator vm::StackEntry() &&;
  Ref<FiftCont> get_def() const& {
    return def;
  }
  Ref<FiftCont> get_def() && {
    return std::move(def);
  }
  bool is_active() const {
    return active;
  }
  bool empty() const {
    return def.is_null();
  }
  explicit operator bool() const {
    return def.not_null();
  }
  bool operator!() const {
    return def.is_null();
  }
};

/*
 *
 *    DICTIONARIES
 *
 */

class Dictionary {
 public:
  Dictionary() : box_(true) {
  }
  Dictionary(Ref<vm::Box> box) : box_(std::move(box)) {
  }
  Dictionary(Ref<Hashmap> hmap) : box_(true, vm::from_object, std::move(hmap)) {
  }

  DictEntry lookup(std::string name) const;
  void def_ctx_word(std::string name, CtxWordFunc func);
  void def_ctx_tail_word(std::string name, CtxTailWordFunc func);
  void def_active_word(std::string name, CtxWordFunc func);
  void def_stack_word(std::string name, StackWordFunc func);
  void def_word(std::string name, DictEntry word);
  void undef_word(std::string name);
  bool lookup_def(const FiftCont* cont, std::string* word_ptr = nullptr) const;
  bool lookup_def(Ref<FiftCont> cont, std::string* word_ptr = nullptr) const {
    return lookup_def(cont.get(), word_ptr);
  }
  auto begin() const {
    return words().begin();
  }
  auto end() const {
    return words().end();
  }
  HashmapKeeper words() const {
    if (box_->empty()) {
      return {};
    } else {
      return box_->get().as_object<Hashmap>();
    }
  }
  Ref<vm::Box> get_box() const {
    return box_;
  }
  void set_words(Ref<Hashmap> new_words) {
    box_->set(vm::StackEntry{vm::from_object, std::move(new_words)});
  }
  bool operator==(const Dictionary& other) const {
    return box_ == other.box_;
  }
  bool operator!=(const Dictionary& other) const {
    return box_ != other.box_;
  }

 private:
  Ref<vm::Box> box_;
};

}  // namespace fift
