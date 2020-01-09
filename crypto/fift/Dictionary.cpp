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

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "Dictionary.h"

namespace fift {

//
// WordDef
//
void WordDef::run(IntCtx& ctx) const {
  bool loop_provoked = false;
  auto stkcpy = ctx.tracing_stack() ? 
        vm::Stack(ctx.stack, ctx.stack.depth(), 0) : std::move(ctx.stack) ;
  try {
    auto next = run_tail(ctx);
    while (next.not_null()) {
      auto curr = next;
      auto stkcpy_ = ctx.tracing_stack() ? 
        vm::Stack(ctx.stack, ctx.stack.depth(), 0) : std::move(ctx.stack) ;
      try {
        next = next->run_tail(ctx);
      } catch (...) {
        if (ctx.tracing_errors()) {
          std::ostringstream stko;
          if (ctx.tracing_stack()) {
            stko << "\n\tStack dump: ";
            for (int i = stkcpy_.depth(); i > 0; i--) {
              stkcpy_[i - 1].dump(stko); stko << ' ';
            }
          }
          LOG(INFO) << "Backtrace: "
            << ctx.dictionary->reverse_lookup(curr.get()) 
            << "(word execution)" << stko.str();
          loop_provoked = true;
        }
        throw;
      }
    }
  } catch (...) {
    if (ctx.tracing_errors() && !loop_provoked) {
      std::ostringstream stko;
      if (ctx.tracing_stack()) {
        stko << "\n\tStack dump: ";
        for (int i = stkcpy.depth(); i > 0; i--) {
          stkcpy[i - 1].dump(stko); stko << ' ';
        }
      }
      LOG(INFO) << "Backtrace: "
        << ctx.dictionary->reverse_lookup(this) 
        << "(word execution)" << stko.str();
    }
    throw;
  }
}

//
// StackWord
//
Ref<WordDef> StackWord::run_tail(IntCtx& ctx) const {
  f(ctx.stack);
  return {};
}

//
// CtxWord
//
Ref<WordDef> CtxWord::run_tail(IntCtx& ctx) const {
  f(ctx);
  return {};
}

//
// CtxTailWord
//
Ref<WordDef> CtxTailWord::run_tail(IntCtx& ctx) const {
  return f(ctx);
}

//
// WordList
//
WordList::WordList(std::vector<Ref<WordDef>>&& _list) : list(std::move(_list)) {
}

WordList::WordList(const std::vector<Ref<WordDef>>& _list) : list(_list) {
}

WordList& WordList::push_back(Ref<WordDef> word_def) {
  list.push_back(std::move(word_def));
  return *this;
}

WordList& WordList::push_back(WordDef& wd) {
  list.emplace_back(&wd);
  return *this;
}

Ref<WordDef> WordList::run_tail(IntCtx& ctx) const {
  if (list.empty()) {
    return {};
  }
  auto it = list.cbegin(), it2 = list.cend() - 1;
  while (it < it2) {
    auto stkcpy = ctx.tracing_stack() ? 
        vm::Stack(ctx.stack, ctx.stack.depth(), 0) : std::move(ctx.stack) ;
    try {
      (*it)->run(ctx);
    } catch (...) {
        if (ctx.tracing_errors()) {
          std::ostringstream wl;
          std::streampos offset = 0;
          std::size_t offlen = 0;
          auto iit = list.cbegin();
          while (iit != list.cend()) {
            if (iit == it) offset = wl.tellp();
            std::string name = ctx.dictionary->reverse_lookup(iit->get());
            wl << name;
            if (iit == it) offlen = name.length() - 1;
            ++iit;
          }
          std::ostringstream stko;
          if (ctx.tracing_stack()) {
            stko << "\n\tStack dump: ";
            for (int i = stkcpy.depth(); i > 0; i--) {
              stkcpy[i - 1].dump(stko); stko << ' ';
            }
          }
          auto current = ctx.dictionary->reverse_lookup(this);
          auto spaces = std::string(current.size() + 4 + offset, ' ');
          LOG(WARNING) << "Backtrace (word list execution):"
            << "\n\t" << current << ": { " << wl.str() << "}"
            << "\n\t" << spaces + std::string(std::max(offlen, 1ul), '^') << stko.str();
        }
        throw;
    }
    ++it;
  }
  return *it;
}

void WordList::close() {
  list.shrink_to_fit();
}

WordList& WordList::append(const std::vector<Ref<WordDef>>& other) {
  list.insert(list.end(), other.begin(), other.end());
  return *this;
}

//
// WordRef
//

WordRef::WordRef(Ref<WordDef> _def, bool _act) : def(std::move(_def)), active(_act) {
}

WordRef::WordRef(StackWordFunc func) : def(Ref<StackWord>{true, std::move(func)}), active(false) {
}

WordRef::WordRef(CtxWordFunc func, bool _act) : def(Ref<CtxWord>{true, std::move(func)}), active(_act) {
}

WordRef::WordRef(CtxTailWordFunc func, bool _act) : def(Ref<CtxTailWord>{true, std::move(func)}), active(_act) {
}

Ref<WordDef> WordRef::get_def() const & {
  return def;
}

Ref<WordDef> WordRef::get_def() && {
  return std::move(def);
}

void WordRef::operator()(IntCtx& ctx) const {
  def->run(ctx);
}

bool WordRef::is_active() const {
  return active;
}

//
// Dictionary
//
WordRef* Dictionary::lookup(td::Slice name) {
  auto it = words_.find(name);
  if (it == words_.end()) {
    return nullptr;
  }
  return &it->second;
}

std::string Dictionary::reverse_lookup(const WordDef* ref) {
  auto it = words_.crbegin();
  while (it != words_.crend()) {
    if (it->second.get_def().get() == ref)
      return it->first;
    ++it;
  }
  auto base_addr = (long long)(words_.crbegin()->second.get_def().get());
  std::ostringstream os;
  auto addr = (long long) ref;
  //if (ref->inner_addr())
  //  addr = ref->inner_addr();
  os << "{" << std::hex 
     << ((addr - base_addr) / 0x10 % 0x10000) 
     << "} ";
  std::string str = os.str();
  return str;
}

void Dictionary::def_ctx_word(std::string name, CtxWordFunc func) {
  def_word(std::move(name), std::move(func));
}

void Dictionary::def_active_word(std::string name, CtxWordFunc func) {
  Ref<WordDef> wdef = Ref<CtxWord>{true, std::move(func)};
  def_word(std::move(name), {std::move(wdef), true});
}

void Dictionary::def_stack_word(std::string name, StackWordFunc func) {
  def_word(std::move(name), std::move(func));
}

void Dictionary::def_ctx_tail_word(std::string name, CtxTailWordFunc func) {
  def_word(std::move(name), std::move(func));
}

void Dictionary::def_word(std::string name, WordRef word) {
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

void interpret_nop(vm::Stack& stack) {
}

Ref<WordDef> Dictionary::nop_word_def = Ref<StackWord>{true, interpret_nop};

//
// functions for wordef
//
Ref<WordDef> pop_exec_token(vm::Stack& stack) {
  stack.check_underflow(1);
  auto wd_ref = stack.pop().as_object<WordDef>();
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
