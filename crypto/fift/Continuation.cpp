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
#include "Continuation.h"
#include "IntCtx.h"
#include "Dictionary.h"

namespace fift {

//
// FiftCont
//
bool FiftCont::print_dict_name(std::ostream& os, const IntCtx& ctx) const {
  std::string word_name;
  if (ctx.dictionary.lookup_def(this, &word_name)) {
    if (word_name.size() && word_name.back() == ' ') {
      word_name.pop_back();
    }
    os << word_name;
    return true;
  }
  return false;
}

std::string FiftCont::get_dict_name(const IntCtx& ctx) const {
  std::string word_name;
  if (ctx.dictionary.lookup_def(this, &word_name)) {
    if (word_name.size() && word_name.back() == ' ') {
      word_name.pop_back();
    }
    return word_name;
  }
  return {};
}

bool FiftCont::print_dummy_name(std::ostream& os, const IntCtx& ctx) const {
  os << "<continuation " << (const void*)this << ">";
  return false;
}

bool FiftCont::print_name(std::ostream& os, const IntCtx& ctx) const {
  return print_dict_name(os, ctx) || print_dummy_name(os, ctx);
}

bool FiftCont::dump(std::ostream& os, const IntCtx& ctx) const {
  bool ok = print_name(os, ctx);
  os << std::endl;
  return ok;
}

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
// QuitCont
//
Ref<FiftCont> QuitCont::run_tail(IntCtx& ctx) const {
  ctx.set_exit_code(exit_code);
  ctx.next.clear();
  return {};
}

bool QuitCont::print_name(std::ostream& os, const IntCtx& ctx) const {
  os << "<quit " << exit_code << ">";
  return true;
}

//
// SeqCont
//
Ref<FiftCont> SeqCont::run_tail(IntCtx& ctx) const {
  ctx.next = seq(second, std::move(ctx.next));
  return first;
}

Ref<FiftCont> SeqCont::run_modify(IntCtx& ctx) {
  if (ctx.next.is_null()) {
    ctx.next = std::move(second);
    return std::move(first);
  } else {
    auto res = std::move(first);
    first = std::move(second);
    second = std::move(ctx.next);
    ctx.next = self();
    return res;
  }
}

bool SeqCont::print_name(std::ostream& os, const IntCtx& ctx) const {
  if (first.not_null()) {
    return first->print_name(os, ctx);
  } else {
    return true;
  }
}

bool SeqCont::dump(std::ostream& os, const IntCtx& ctx) const {
  if (first.not_null()) {
    return first->dump(os, ctx);
  } else {
    return true;
  }
}

//
// TimesCont
//
Ref<FiftCont> TimesCont::run_tail(IntCtx& ctx) const {
  if (count > 1) {
    ctx.next = td::make_ref<TimesCont>(body, SeqCont::seq(after, std::move(ctx.next)), count - 1);
  } else {
    ctx.next = SeqCont::seq(after, std::move(ctx.next));
  }
  return body;
}

Ref<FiftCont> TimesCont::run_modify(IntCtx& ctx) {
  if (ctx.next.not_null()) {
    after = SeqCont::seq(std::move(after), std::move(ctx.next));
  }
  if (count > 1) {
    --count;
    ctx.next = self();
    return body;
  } else {
    ctx.next = std::move(after);
    return std::move(body);
  }
}

bool TimesCont::print_name(std::ostream& os, const IntCtx& ctx) const {
  os << "<repeat " << count << " times>";
  return true;
}

bool TimesCont::dump(std::ostream& os, const IntCtx& ctx) const {
  os << "<repeat " << count << " times:> ";
  return body->dump(os, ctx);
}

//
// UntilCont
//
Ref<FiftCont> UntilCont::run_tail(IntCtx& ctx) const {
  if (ctx.stack.pop_bool()) {
    return after;
  } else if (ctx.next.not_null()) {
    ctx.next = td::make_ref<UntilCont>(body, SeqCont::seq(after, std::move(ctx.next)));
    return body;
  } else {
    ctx.next = self();
    return body;
  }
}

Ref<FiftCont> UntilCont::run_modify(IntCtx& ctx) {
  if (ctx.stack.pop_bool()) {
    return std::move(after);
  } else {
    if (ctx.next.not_null()) {
      after = SeqCont::seq(after, std::move(ctx.next));
    }
    ctx.next = self();
    return body;
  }
}

bool UntilCont::print_name(std::ostream& os, const IntCtx& ctx) const {
  os << "<until loop continuation>";
  return true;
}

bool UntilCont::dump(std::ostream& os, const IntCtx& ctx) const {
  os << "<until loop continuation:> ";
  return body->dump(os, ctx);
}

//
// WhileCont
//
Ref<FiftCont> WhileCont::run_tail(IntCtx& ctx) const {
  if (!stage) {
    ctx.next = td::make_ref<WhileCont>(cond, body, SeqCont::seq(after, std::move(ctx.next)), true);
    return cond;
  }
  if (!ctx.stack.pop_bool()) {
    return after;
  } else {
    ctx.next = td::make_ref<WhileCont>(cond, body, SeqCont::seq(after, std::move(ctx.next)));
    return body;
  }
}

Ref<FiftCont> WhileCont::run_modify(IntCtx& ctx) {
  if (!stage) {
    if (ctx.next.not_null()) {
      after = SeqCont::seq(std::move(after), std::move(ctx.next));
    }
    stage = true;
    ctx.next = self();
    return cond;
  }
  if (!ctx.stack.pop_bool()) {
    return std::move(after);
  } else {
    if (ctx.next.not_null()) {
      after = SeqCont::seq(std::move(after), std::move(ctx.next));
    }
    stage = false;
    ctx.next = self();
    return body;
  }
}

bool WhileCont::print_name(std::ostream& os, const IntCtx& ctx) const {
  os << "<while loop " << (stage ? "body" : "condition") << ">";
  return true;
}

bool WhileCont::dump(std::ostream& os, const IntCtx& ctx) const {
  os << "<while loop " << (stage ? "body" : "condition") << ":> ";
  return (stage ? body : cond)->dump(os, ctx);
}

//
// LoopCont
//
Ref<FiftCont> LoopCont::run_tail(IntCtx& ctx) const {
  return Ref<FiftCont>(clone());
}

Ref<FiftCont> LoopCont::run_modify(IntCtx& ctx) {
  if (ctx.next.not_null()) {
    after = SeqCont::seq(std::move(after), std::move(ctx.next));
  }
  switch (state) {
    case 0:
      if (!init(ctx)) {
        return std::move(after);
      }
      state = 1;
      // fallthrough
    case 1:
      if (!pre_exec(ctx)) {
        state = 3;
        if (finalize(ctx)) {
          return std::move(after);
        } else {
          return {};
        }
      }
      state = 2;
      ctx.next = self();
      return func;
    case 2:
      if (post_exec(ctx)) {
        state = 1;
        return self();
      }
      state = 3;
      // fallthrough
    case 3:
      if (finalize(ctx)) {
        return std::move(after);
      } else {
        return {};
      }
    default:
      throw IntError{"invalid LoopCont state"};
  }
}

bool LoopCont::print_name(std::ostream& os, const IntCtx& ctx) const {
  os << "<generic loop continuation state " << state << ">";
  return true;
}

//
// GenericLitCont
//
bool GenericLitCont::print_name(std::ostream& os, const IntCtx& ctx) const {
  auto list = get_literals();
  bool sp = false;
  for (auto entry : list) {
    if (sp) {
      os << ' ';
    }
    sp = true;
    int tp = entry.type();
    if (entry.is_int() || entry.is(vm::StackEntry::t_string) || entry.is(vm::StackEntry::t_bytes)) {
      entry.dump(os);
    } else if (entry.is_atom()) {
      os << '`';
      entry.dump(os);
    } else {
      auto cont_lit = entry.as_object<FiftCont>();
      if (cont_lit.not_null()) {
        os << "{ ";
        cont_lit->print_name(os, ctx);
        os << " }";
      } else {
        os << "<literal of type " << tp << ">";
      }
    }
  }
  return true;
}

//
// SmallIntLitCont
//
Ref<FiftCont> SmallIntLitCont::run_tail(IntCtx& ctx) const {
  ctx.stack.push_smallint(value_);
  return {};
}

std::vector<vm::StackEntry> SmallIntLitCont::get_literals() const {
  return {td::make_refint(value_)};
}

//
// IntLitCont
//
Ref<FiftCont> IntLitCont::run_tail(IntCtx& ctx) const {
  ctx.stack.push_int(value_);
  return {};
}

Ref<FiftCont> IntLitCont::run_modify(IntCtx& ctx) {
  ctx.stack.push_int(std::move(value_));
  return {};
}

Ref<FiftCont> IntLitCont::literal(td::RefInt256 int_value) {
  if (int_value->signed_fits_bits(64)) {
    return literal(int_value->to_long());
  } else {
    return td::make_ref<IntLitCont>(std::move(int_value));
  }
}

//
// LitCont
//
Ref<FiftCont> LitCont::run_tail(IntCtx& ctx) const {
  ctx.stack.push(value_);
  return {};
}

Ref<FiftCont> LitCont::run_modify(IntCtx& ctx) {
  ctx.stack.push(std::move(value_));
  return {};
}

Ref<FiftCont> LitCont::literal(vm::StackEntry value) {
  if (value.is_int()) {
    return literal(std::move(value).as_int());
  } else {
    return td::make_ref<LitCont>(std::move(value));
  }
}

//
// MultiLitCont
//
Ref<FiftCont> MultiLitCont::run_tail(IntCtx& ctx) const {
  for (auto& value : values_) {
    ctx.stack.push(value);
  }
  return {};
}

Ref<FiftCont> MultiLitCont::run_modify(IntCtx& ctx) {
  for (auto& value : values_) {
    ctx.stack.push(std::move(value));
  }
  return {};
}

MultiLitCont& MultiLitCont::push_back(vm::StackEntry new_literal) {
  values_.push_back(std::move(new_literal));
  return *this;
}

vm::StackEntry MultiLitCont::at(int idx) const {
  return values_.at(idx);
}

}  // namespace fift
