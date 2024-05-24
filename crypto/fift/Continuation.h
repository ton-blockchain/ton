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

    Copyright 2020 Telegram Systems LLP
*/
#pragma once
#include <functional>
#include "common/refcnt.hpp"
#include "common/refint.h"
#include "vm/stack.hpp"

namespace fift {
using td::Ref;
struct IntCtx;

/*
 * 
 *    FIFT CONTINUATIONS
 * 
 */

class FiftCont : public td::CntObject {
 public:
  FiftCont() = default;
  virtual ~FiftCont() override = default;
  virtual Ref<FiftCont> run_tail(IntCtx& ctx) const = 0;
  virtual Ref<FiftCont> run_modify(IntCtx& ctx) {
    return run_tail(ctx);
  }
  virtual Ref<FiftCont> handle_tail(IntCtx& ctx) const {
    return {};
  }
  virtual Ref<FiftCont> handle_modify(IntCtx& ctx) {
    return handle_tail(ctx);
  }
  virtual Ref<FiftCont> up() const {
    return {};
  }
  virtual bool is_list() const {
    return false;
  }
  virtual long long list_size() const {
    return -1;
  }
  virtual const Ref<FiftCont>* get_list() const {
    return nullptr;
  }
  virtual bool is_literal() const {
    return false;
  }
  virtual int literal_count() const {
    return -1;
  }
  virtual std::vector<vm::StackEntry> get_literals() const {
    return {};
  }
  std::string get_dict_name(const IntCtx& ctx) const;
  bool print_dict_name(std::ostream& os, const IntCtx& ctx) const;
  bool print_dummy_name(std::ostream& os, const IntCtx& ctx) const;
  virtual bool print_name(std::ostream& os, const IntCtx& ctx) const;
  virtual bool dump(std::ostream& os, const IntCtx& ctx) const;
  Ref<FiftCont> self() const {
    return Ref<FiftCont>{this};
  }
};

typedef std::function<void(vm::Stack&)> StackWordFunc;
typedef std::function<void(IntCtx&)> CtxWordFunc;
typedef std::function<Ref<FiftCont>(IntCtx&)> CtxTailWordFunc;

class NopWord : public FiftCont {
 public:
  NopWord() = default;
  ~NopWord() override = default;
  Ref<FiftCont> run_tail(IntCtx& ctx) const override {
    return {};
  }
};

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

class QuitCont : public FiftCont {
  int exit_code;

 public:
  QuitCont(int _exit_code = -1) : exit_code(_exit_code) {
  }
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
  bool print_name(std::ostream& os, const IntCtx& ctx) const override;
};

class SeqCont : public FiftCont {
  Ref<FiftCont> first, second;

 public:
  SeqCont(Ref<FiftCont> _first, Ref<FiftCont> _second) : first(std::move(_first)), second(std::move(_second)) {
  }
  static Ref<FiftCont> seq(Ref<FiftCont> _first, Ref<FiftCont> _second) {
    return _second.is_null() ? std::move(_first) : td::make_ref<SeqCont>(std::move(_first), std::move(_second));
  }
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
  Ref<FiftCont> run_modify(IntCtx& ctx) override;
  Ref<FiftCont> up() const override {
    return second;
  }
  bool print_name(std::ostream& os, const IntCtx& ctx) const override;
  bool dump(std::ostream& os, const IntCtx& ctx) const override;
};

class TimesCont : public FiftCont {
  Ref<FiftCont> body, after;
  int count;

 public:
  TimesCont(Ref<FiftCont> _body, Ref<FiftCont> _after, int _count)
      : body(std::move(_body)), after(std::move(_after)), count(_count) {
  }
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
  Ref<FiftCont> run_modify(IntCtx& ctx) override;
  Ref<FiftCont> up() const override {
    return after;
  }
  bool print_name(std::ostream& os, const IntCtx& ctx) const override;
  bool dump(std::ostream& os, const IntCtx& ctx) const override;
};

class UntilCont : public FiftCont {
  Ref<FiftCont> body, after;

 public:
  UntilCont(Ref<FiftCont> _body, Ref<FiftCont> _after) : body(std::move(_body)), after(std::move(_after)) {
  }
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
  Ref<FiftCont> run_modify(IntCtx& ctx) override;
  bool print_name(std::ostream& os, const IntCtx& ctx) const override;
  bool dump(std::ostream& os, const IntCtx& ctx) const override;
};

class WhileCont : public FiftCont {
  Ref<FiftCont> cond, body, after;
  bool stage;

 public:
  WhileCont(Ref<FiftCont> _cond, Ref<FiftCont> _body, Ref<FiftCont> _after, bool _stage = false)
      : cond(std::move(_cond)), body(std::move(_body)), after(std::move(_after)), stage(_stage) {
  }
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
  Ref<FiftCont> run_modify(IntCtx& ctx) override;
  Ref<FiftCont> up() const override {
    return after;
  }
  bool print_name(std::ostream& os, const IntCtx& ctx) const override;
  bool dump(std::ostream& os, const IntCtx& ctx) const override;
};

class LoopCont : public FiftCont {
  Ref<FiftCont> func, after;
  int state;

 public:
  LoopCont(Ref<FiftCont> _func, Ref<FiftCont> _after, int _state = 0)
      : func(std::move(_func)), after(std::move(_after)), state(_state) {
  }
  LoopCont(const LoopCont&) = default;
  virtual bool init(IntCtx& ctx) {
    return true;
  }
  virtual bool pre_exec(IntCtx& ctx) {
    return true;
  }
  virtual bool post_exec(IntCtx& ctx) {
    return true;
  }
  virtual bool finalize(IntCtx& ctx) {
    return true;
  }
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
  Ref<FiftCont> run_modify(IntCtx& ctx) override;
  Ref<FiftCont> up() const override {
    return after;
  }
  bool print_name(std::ostream& os, const IntCtx& ctx) const override;
};

class GenericLitCont : public FiftCont {
 public:
  bool is_literal() const override {
    return true;
  }
  std::vector<vm::StackEntry> get_literals() const override = 0;
  bool print_name(std::ostream& os, const IntCtx& ctx) const override;
};

class SmallIntLitCont : public GenericLitCont {
  long long value_;

 public:
  SmallIntLitCont(long long value) : value_(value) {
  }
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
  std::vector<vm::StackEntry> get_literals() const override;
  static Ref<FiftCont> literal(long long int_value) {
    return td::make_ref<SmallIntLitCont>(int_value);
  }
};

class IntLitCont : public GenericLitCont {
  td::RefInt256 value_;

 public:
  IntLitCont(td::RefInt256 value) : value_(std::move(value)) {
  }
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
  Ref<FiftCont> run_modify(IntCtx& ctx) override;
  std::vector<vm::StackEntry> get_literals() const override {
    return {vm::StackEntry(value_)};
  }
  static Ref<FiftCont> literal(td::RefInt256 int_value);
  static Ref<FiftCont> literal(long long int_value) {
    return SmallIntLitCont::literal(int_value);
  }
};

class LitCont : public GenericLitCont {
  vm::StackEntry value_;

 public:
  LitCont(const vm::StackEntry& value) : value_(value) {
  }
  LitCont(vm::StackEntry&& value) : value_(std::move(value)) {
  }
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
  Ref<FiftCont> run_modify(IntCtx& ctx) override;
  std::vector<vm::StackEntry> get_literals() const override {
    return {value_};
  }
  static Ref<FiftCont> literal(vm::StackEntry value);
  static Ref<FiftCont> literal(td::RefInt256 int_value) {
    return IntLitCont::literal(std::move(int_value));
  }
  static Ref<FiftCont> literal(long long int_value) {
    return SmallIntLitCont::literal(int_value);
  }
};

class MultiLitCont : public GenericLitCont {
  std::vector<vm::StackEntry> values_;

 public:
  MultiLitCont(const std::vector<vm::StackEntry>& values) : values_(values) {
  }
  MultiLitCont(std::vector<vm::StackEntry>&& values) : values_(std::move(values)) {
  }
  MultiLitCont(std::initializer_list<vm::StackEntry> value_list) : values_(value_list) {
  }
  Ref<FiftCont> run_tail(IntCtx& ctx) const override;
  Ref<FiftCont> run_modify(IntCtx& ctx) override;
  std::vector<vm::StackEntry> get_literals() const override {
    return values_;
  }
  MultiLitCont& push_back(vm::StackEntry new_literal);
  vm::StackEntry at(int idx) const;
};

class InterpretCont : public FiftCont {
 public:
  InterpretCont() = default;
  Ref<FiftCont> run_tail(IntCtx&) const override;  // text interpreter, defined in words.cpp
  bool print_name(std::ostream& os, const IntCtx& ctx) const override {
    os << "<text interpreter continuation>";
    return true;
  }
};

}  // namespace fift
