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
#include "Dictionary.h"
#include "IntCtx.h"

namespace fift {

//
// DictEntry
//

DictEntry::DictEntry(StackWordFunc func) : def(Ref<StackWord>{true, std::move(func)}), active(false) {
}

DictEntry::DictEntry(CtxWordFunc func, bool _act) : def(Ref<CtxWord>{true, std::move(func)}), active(_act) {
}

DictEntry::DictEntry(CtxTailWordFunc func, bool _act) : def(Ref<CtxTailWord>{true, std::move(func)}), active(_act) {
}

DictEntry DictEntry::create_from(vm::StackEntry se) {
  if (se.is_tuple()) {
    auto& tuple = *se.as_tuple();
    if (tuple.size() == 1) {
      auto def = tuple[0].as_object<FiftCont>();
      if (def.not_null()) {
        return DictEntry{std::move(def), true};
      }
    }
  } else {
    auto def = std::move(se).as_object<FiftCont>();
    if (def.not_null()) {
      return DictEntry{std::move(def)};
    }
  }
  return {};
}

DictEntry::operator vm::StackEntry() const& {
  if (def.is_null()) {
    return {};
  } else if (active) {
    return vm::make_tuple_ref(vm::StackEntry{vm::from_object, def});
  } else {
    return {vm::from_object, def};
  }
}

DictEntry::operator vm::StackEntry() && {
  if (def.is_null()) {
    return {};
  } else if (active) {
    return vm::make_tuple_ref(vm::StackEntry{vm::from_object, std::move(def)});
  } else {
    return {vm::from_object, std::move(def)};
  }
}

//
// Dictionary
//
DictEntry Dictionary::lookup(std::string name) const {
  return DictEntry::create_from(words().get(name));
}

void Dictionary::def_ctx_word(std::string name, CtxWordFunc func) {
  def_word(std::move(name), std::move(func));
}

void Dictionary::def_active_word(std::string name, CtxWordFunc func) {
  Ref<FiftCont> wdef = Ref<CtxWord>{true, std::move(func)};
  def_word(std::move(name), {std::move(wdef), true});
}

void Dictionary::def_stack_word(std::string name, StackWordFunc func) {
  def_word(std::move(name), std::move(func));
}

void Dictionary::def_ctx_tail_word(std::string name, CtxTailWordFunc func) {
  def_word(std::move(name), std::move(func));
}

void Dictionary::def_word(std::string name, DictEntry word) {
  auto dict = words();
  dict.set(std::move(name), vm::StackEntry(std::move(word)));
  set_words(dict);
}

void Dictionary::undef_word(std::string name) {
  auto dict = words();
  if (dict.remove(name)) {
    set_words(dict);
  }
}

bool Dictionary::lookup_def(const FiftCont* cont, std::string* word_ptr) const {
  if (!cont) {
    return false;
  }
  for (auto entry : words()) {
    auto val = DictEntry::create_from(entry.value());
    if (val.get_def().get() == cont && entry.key().is_string()) {
      if (word_ptr) {
        *word_ptr = vm::StackEntry(entry.key()).as_string();
      }
      return true;
    }
  }
  return false;
}

}  // namespace fift
