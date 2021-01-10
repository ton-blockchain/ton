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
#include <map>

#include "IntCtx.h"
#include "Continuation.h"

namespace fift {
using td::Ref;

/*
 *
 *    WORD CLASSES
 *
 */

typedef std::function<void(vm::Stack&)> StackWordFunc;
typedef std::function<void(IntCtx&)> CtxWordFunc;

class StackWord : public FiftCont {
  StackWordFunc f;

 public:
  StackWord(StackWordFunc _f) : f(std::move(_f)) {
  }
  ~StackWord() override = default;
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
};

class CtxWord : public FiftCont {
  CtxWordFunc f;

 public:
  CtxWord(CtxWordFunc _f) : f(std::move(_f)) {
  }
  ~CtxWord() override = default;
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
};

typedef std::function<Ref<FiftCont>(IntCtx&)> CtxTailWordFunc;

class CtxTailWord : public FiftCont {
  CtxTailWordFunc f;

 public:
  CtxTailWord(CtxTailWordFunc _f) : f(std::move(_f)) {
  }
  ~CtxTailWord() override = default;
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
};

class WordList : public FiftCont {
  std::vector<Ref<FiftCont>> list;

 public:
  ~WordList() override = default;
  WordList() = default;
  WordList(std::vector<Ref<FiftCont>>&& _list);
  WordList(const std::vector<Ref<FiftCont>>& _list);
  WordList& push_back(Ref<FiftCont> word_def);
  WordList& push_back(FiftCont& wd);
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
  void close();
  bool is_list() const override {
    return true;
  }
  long long list_size() const override {
    return (long long)list.size();
  }
  std::size_t size() const {
    return list.size();
  }
  const Ref<FiftCont>& at(std::size_t idx) const {
    return list.at(idx);
  }
  const Ref<FiftCont>* get_list() const override {
    return list.data();
  }
  WordList& append(const std::vector<Ref<FiftCont>>& other);
  WordList& append(const Ref<FiftCont>* begin, const Ref<FiftCont>* end);
  WordList* make_copy() const override {
    return new WordList(list);
  }
  bool dump(std::ostream& os, const IntCtx& ctx) const override;
};

class ListCont : public FiftCont {
  Ref<FiftCont> next;
  Ref<WordList> list;
  std::size_t pos;

 public:
  ListCont(Ref<FiftCont> nxt, Ref<WordList> wl, std::size_t p = 0) : next(std::move(nxt)), list(std::move(wl)), pos(p) {
  }
  ~ListCont() override = default;
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
  Ref<FiftCont> run_modify(IntCtx& ctx) override;
  Ref<FiftCont> up() const override {
    return next;
  }
  bool dump(std::ostream& os, const IntCtx& ctx) const override;
};

class DictEntry {
  Ref<FiftCont> def;
  bool active;

 public:
  DictEntry() = delete;
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
  Ref<FiftCont> get_def() const& {
    return def;
  }
  Ref<FiftCont> get_def() && {
    return std::move(def);
  }
  bool is_active() const {
    return active;
  }
};

/*
DictEntry::DictEntry(const std::vector<Ref<FiftCont>>& word_list) : def(Ref<WordList>{true, word_list}) {
}

DictEntry::DictEntry(std::vector<Ref<FiftCont>>&& word_list) : def(Ref<WordList>{true, std::move(word_list)}) {
}
*/

/*
 *
 *    DICTIONARIES
 *
 */

class Dictionary {
 public:
  DictEntry* lookup(td::Slice name);
  void def_ctx_word(std::string name, CtxWordFunc func);
  void def_ctx_tail_word(std::string name, CtxTailWordFunc func);
  void def_active_word(std::string name, CtxWordFunc func);
  void def_stack_word(std::string name, StackWordFunc func);
  void def_word(std::string name, DictEntry word);
  void undef_word(td::Slice name);
  bool lookup_def(const FiftCont* cont, std::string* word_ptr = nullptr) const;
  bool lookup_def(Ref<FiftCont> cont, std::string* word_ptr = nullptr) const {
    return lookup_def(cont.get(), word_ptr);
  }
  auto begin() const {
    return words_.begin();
  }
  auto end() const {
    return words_.end();
  }

  static Ref<FiftCont> nop_word_def;

 private:
  std::map<std::string, DictEntry, std::less<>> words_;
};

/*
 *
 *      AUX FUNCTIONS FOR WORD DEFS
 *
 */

Ref<FiftCont> pop_exec_token(vm::Stack& stack);
Ref<WordList> pop_word_list(vm::Stack& stack);
void push_argcount(vm::Stack& stack, int args);
}  // namespace fift
