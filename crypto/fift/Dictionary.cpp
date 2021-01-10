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

namespace fift {

//
// StackWord
//
Ref<FiftCont> StackWord::run_tail(IntCtx& ctx) const {
  f(ctx.stack);
  return {};
}

//
// CtxWord
//
Ref<FiftCont> CtxWord::run_tail(IntCtx& ctx) const {
  f(ctx);
  return {};
}

//
// CtxTailWord
//
Ref<FiftCont> CtxTailWord::run_tail(IntCtx& ctx) const {
  return f(ctx);
}

//
// WordList
//
WordList::WordList(std::vector<Ref<FiftCont>>&& _list) : list(std::move(_list)) {
}

WordList::WordList(const std::vector<Ref<FiftCont>>& _list) : list(_list) {
}

WordList& WordList::push_back(Ref<FiftCont> word_def) {
  list.push_back(std::move(word_def));
  return *this;
}

WordList& WordList::push_back(FiftCont& wd) {
  list.emplace_back(&wd);
  return *this;
}

Ref<FiftCont> WordList::run_tail(IntCtx& ctx) const {
  if (list.empty()) {
    return {};
  }
  if (list.size() > 1) {
    ctx.next = td::make_ref<ListCont>(std::move(ctx.next), Ref<WordList>(this), 1);
  }
  return list[0];
}

void WordList::close() {
  list.shrink_to_fit();
}

WordList& WordList::append(const std::vector<Ref<FiftCont>>& other) {
  list.insert(list.end(), other.begin(), other.end());
  return *this;
}

WordList& WordList::append(const Ref<FiftCont>* begin, const Ref<FiftCont>* end) {
  list.insert(list.end(), begin, end);
  return *this;
}

bool WordList::dump(std::ostream& os, const IntCtx& ctx) const {
  os << "{";
  for (auto entry : list) {
    os << ' ';
    entry->print_name(os, ctx);
  }
  os << " }" << std::endl;
  return true;
}

//
// ListCont
//

Ref<FiftCont> ListCont::run_tail(IntCtx& ctx) const {
  auto sz = list->size();
  if (pos >= sz) {
    return std::move(ctx.next);
  } else if (ctx.next.not_null()) {
    ctx.next = td::make_ref<ListCont>(SeqCont::seq(next, std::move(ctx.next)), list, pos + 1);
  } else if (pos + 1 == sz) {
    ctx.next = next;
  } else {
    ctx.next = td::make_ref<ListCont>(next, list, pos + 1);
  }
  return list->at(pos);
}

Ref<FiftCont> ListCont::run_modify(IntCtx& ctx) {
  auto sz = list->size();
  if (pos >= sz) {
    return std::move(ctx.next);
  }
  auto cur = list->at(pos++);
  if (ctx.next.not_null()) {
    next = SeqCont::seq(next, std::move(ctx.next));
  }
  if (pos == sz) {
    ctx.next = std::move(next);
  } else {
    ctx.next = self();
  }
  return cur;
}

bool ListCont::dump(std::ostream& os, const IntCtx& ctx) const {
  std::string dict_name = list->get_dict_name(ctx);
  if (!dict_name.empty()) {
    os << "[in " << dict_name << ":] ";
  }
  std::size_t sz = list->size(), i, a = (pos >= 16 ? pos - 16 : 0), b = std::min(pos + 16, sz);
  if (a > 0) {
    os << "... ";
  }
  for (i = a; i < b; i++) {
    if (i == pos) {
      os << "**HERE** ";
    }
    list->at(i)->print_name(os, ctx);
    os << ' ';
  }
  if (b < sz) {
    os << "...";
  }
  os << std::endl;
  return true;
}

//
// DictEntry
//

DictEntry::DictEntry(StackWordFunc func) : def(Ref<StackWord>{true, std::move(func)}), active(false) {
}

DictEntry::DictEntry(CtxWordFunc func, bool _act) : def(Ref<CtxWord>{true, std::move(func)}), active(_act) {
}

DictEntry::DictEntry(CtxTailWordFunc func, bool _act) : def(Ref<CtxTailWord>{true, std::move(func)}), active(_act) {
}

//
// Dictionary
//
DictEntry* Dictionary::lookup(td::Slice name) {
  auto it = words_.find(name);
  if (it == words_.end()) {
    return nullptr;
  }
  return &it->second;
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
  auto res = words_.emplace(name, std::move(word));
  LOG_IF(FATAL, !res.second) << "Cannot redefine word: " << name;
}

void Dictionary::undef_word(td::Slice name) {
  auto it = words_.find(name);
  if (it == words_.end()) {
    return;
  }
  words_.erase(it);
}

bool Dictionary::lookup_def(const FiftCont* cont, std::string* word_ptr) const {
  if (!cont) {
    return false;
  }
  for (const auto& entry : words_) {
    if (entry.second.get_def().get() == cont) {
      if (word_ptr) {
        *word_ptr = entry.first;
      }
      return true;
    }
  }
  return false;
}

void interpret_nop(vm::Stack& stack) {
}

Ref<FiftCont> Dictionary::nop_word_def = Ref<StackWord>{true, interpret_nop};

//
// functions for wordef
//
Ref<FiftCont> pop_exec_token(vm::Stack& stack) {
  stack.check_underflow(1);
  auto wd_ref = stack.pop().as_object<FiftCont>();
  if (wd_ref.is_null()) {
    throw IntError{"execution token expected"};
  }
  return wd_ref;
}

Ref<WordList> pop_word_list(vm::Stack& stack) {
  stack.check_underflow(1);
  auto wl_ref = stack.pop().as_object<WordList>();
  if (wl_ref.is_null()) {
    throw IntError{"word list expected"};
  }
  return wl_ref;
}

void push_argcount(vm::Stack& stack, int args) {
  stack.push_smallint(args);
  stack.push({vm::from_object, Dictionary::nop_word_def});
}

}  // namespace fift
