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
#include "words.h"

#include "Dictionary.h"
#include "IntCtx.h"
#include "SourceLookup.h"
#include "HashMap.h"

#include "common/refcnt.hpp"
#include "common/bigint.hpp"
#include "common/refint.h"
#include "common/bitstring.h"
#include "common/util.h"

#include "openssl/digest.hpp"

#include "Ed25519.h"

#include "vm/cells.h"
#include "vm/cellslice.h"
#include "vm/vm.h"
#include "vm/cp0.h"
#include "vm/dict.h"
#include "vm/boc.h"

#include "vm/box.hpp"
#include "vm/atom.h"

#include "block/block.h"
#include "common/global-version.h"

#include "td/utils/filesystem.h"
#include "td/utils/misc.h"
#include "td/utils/optional.h"
#include "td/utils/PathView.h"
#include "td/utils/port/thread.h"
#include "td/utils/port/Stat.h"
#include "td/utils/Timer.h"
#include "td/utils/tl_helpers.h"
#include "td/utils/crypto.h"

#include <ctime>

namespace fift {

const Ref<FiftCont> nop_word_def = Ref<NopWord>{true};

//
// functions for wordef
//
Ref<FiftCont> pop_exec_token(vm::Stack& stack) {
  auto wd_ref = stack.pop_chk().as_object<FiftCont>();
  if (wd_ref.is_null()) {
    throw IntError{"execution token expected"};
  }
  return wd_ref;
}

Ref<WordList> pop_word_list(vm::Stack& stack) {
  auto wl_ref = stack.pop_chk().as_object<WordList>();
  if (wl_ref.is_null()) {
    throw IntError{"word list expected"};
  }
  return wl_ref;
}

void push_argcount(vm::Stack& stack, int args) {
  stack.push_smallint(args);
  stack.push_object(nop_word_def);
}

void interpret_dot(IntCtx& ctx, bool space_after) {
  *ctx.output_stream << dec_string2(ctx.stack.pop_int()) << (space_after ? " " : "");
}

void interpret_dothex(IntCtx& ctx, bool upcase, bool space_after) {
  *ctx.output_stream << hex_string(ctx.stack.pop_int(), upcase) << (space_after ? " " : "");
}

void interpret_dotbinary(IntCtx& ctx, bool space_after) {
  *ctx.output_stream << binary_string(ctx.stack.pop_int()) << (space_after ? " " : "");
}

void interpret_dot_cellslice_rec(IntCtx& ctx) {
  auto cs = ctx.stack.pop_cellslice();
  cs->print_rec(*ctx.output_stream);
}

void interpret_dotstack(IntCtx& ctx) {
  for (int i = ctx.stack.depth(); i > 0; i--) {
    ctx.stack[i - 1].dump(*ctx.output_stream);
    *ctx.output_stream << ' ';
  }
  *ctx.output_stream << std::endl;
}

void interpret_dotstack_list(IntCtx& ctx) {
  for (int i = ctx.stack.depth(); i > 0; i--) {
    ctx.stack[i - 1].print_list(*ctx.output_stream);
    *ctx.output_stream << ' ';
  }
  *ctx.output_stream << std::endl;
}

void interpret_dotstack_list_dump(IntCtx& ctx) {
  ctx.stack.dump(*ctx.output_stream, 3);
}

void interpret_dump(IntCtx& ctx) {
  ctx.stack.pop_chk().dump(*ctx.output_stream);
  *ctx.output_stream << ' ';
}

void interpret_dump_internal(vm::Stack& stack) {
  stack.push_string(stack.pop_chk().to_string());
}

void interpret_list_dump_internal(vm::Stack& stack) {
  stack.push_string(stack.pop_chk().to_lisp_string());
}

void interpret_print_list(IntCtx& ctx) {
  ctx.stack.pop_chk().print_list(*ctx.output_stream);
  *ctx.output_stream << ' ';
}

void interpret_dottc(IntCtx& ctx) {
  *ctx.output_stream << "total cells = " << vm::DataCell::get_total_data_cells() << std::endl;
}

void interpret_dot_internal(vm::Stack& stack) {
  stack.push_string(dec_string2(stack.pop_int()));
}

void interpret_dothex_internal(vm::Stack& stack, bool upcase) {
  stack.push_string(hex_string(stack.pop_int(), upcase));
}

void interpret_dotbinary_internal(vm::Stack& stack) {
  stack.push_string(binary_string(stack.pop_int()));
}

void interpret_plus(vm::Stack& stack) {
  stack.push_int(stack.pop_int() + stack.pop_int());
}

void interpret_cond_dup(vm::Stack& stack) {
  auto x = stack.pop_int();
  if (x->sgn()) {
    stack.push_int(x);
  }
  stack.push_int(std::move(x));
}

void interpret_plus_tiny(vm::Stack& stack, long long y) {
  stack.push_int(stack.pop_int() + y);
}

void interpret_minus(vm::Stack& stack) {
  auto y = stack.pop_int();
  stack.push_int(stack.pop_int() - y);
}

void interpret_times(vm::Stack& stack) {
  stack.push_int(stack.pop_int() * stack.pop_int());
}

void interpret_div(vm::Stack& stack, int round_mode) {
  auto y = stack.pop_int();
  stack.push_int(td::div(stack.pop_int(), y, round_mode));
}

void interpret_mod(vm::Stack& stack, int round_mode) {
  auto y = stack.pop_int();
  stack.push_int(td::mod(stack.pop_int(), y, round_mode));
}

void interpret_divmod(vm::Stack& stack, int round_mode) {
  auto y = stack.pop_int();
  auto dm = td::divmod(stack.pop_int(), std::move(y), round_mode);
  stack.push_int(std::move(dm.first));
  stack.push_int(std::move(dm.second));
}

void interpret_times_div(vm::Stack& stack, int round_mode) {
  auto z = stack.pop_int(), y = stack.pop_int(), x = stack.pop_int();
  stack.push_int(muldiv(std::move(x), std::move(y), std::move(z), round_mode));
}

void interpret_times_divmod(vm::Stack& stack, int round_mode) {
  auto z = stack.pop_int(), y = stack.pop_int(), x = stack.pop_int();
  auto dm = muldivmod(std::move(x), std::move(y), std::move(z));
  stack.push_int(std::move(dm.first));
  stack.push_int(std::move(dm.second));
}

void interpret_times_mod(vm::Stack& stack, int round_mode) {
  auto z = stack.pop_int();
  auto y = stack.pop_int();
  auto x = stack.pop_int();
  typename td::BigInt256::DoubleInt tmp{0}, q;
  tmp.add_mul(*x, *y);
  tmp.mod_div(*z, q, round_mode);
  stack.push_int(td::make_refint(tmp));
}

void interpret_negate(vm::Stack& stack) {
  stack.push_int(-stack.pop_int());
}

void interpret_cmp(vm::Stack& stack, const char opt[3]) {
  auto y = stack.pop_int();
  auto x = stack.pop_int();
  int r = x->cmp(*y);
  assert((unsigned)(r + 1) <= 2);
  stack.push_smallint(((const signed char*)opt)[r + 1]);
}

void interpret_sgn(vm::Stack& stack, const char opt[3]) {
  auto x = stack.pop_int_finite();
  int r = x->sgn();
  assert((unsigned)(r + 1) <= 2);
  stack.push_smallint(((const signed char*)opt)[r + 1]);
}

void interpret_fits(vm::Stack& stack, bool sgnd) {
  int n = stack.pop_smallint_range(1023);
  auto x = stack.pop_int();
  stack.push_bool(x->fits_bits(n, sgnd));
}

void interpret_pow2(vm::Stack& stack) {
  int x = stack.pop_smallint_range(255);
  auto r = td::make_refint();
  r.unique_write().set_pow2(x);
  stack.push_int(r);
}

void interpret_neg_pow2(vm::Stack& stack) {
  int x = stack.pop_smallint_range(256);
  auto r = td::make_refint();
  r.unique_write().set_pow2(x).negate().normalize();
  stack.push_int(r);
}

void interpret_pow2_minus1(vm::Stack& stack) {
  int x = stack.pop_smallint_range(256);
  auto r = td::make_refint();
  r.unique_write().set_pow2(x).add_tiny(-1).normalize();
  stack.push_int(r);
}

void interpret_mod_pow2(vm::Stack& stack) {
  int y = stack.pop_smallint_range(256);
  auto x = stack.pop_int();
  x.write().mod_pow2(y).normalize();
  stack.push_int(x);
}

void interpret_lshift(vm::Stack& stack) {
  int y = stack.pop_smallint_range(256);
  stack.push_int(stack.pop_int() << y);
}

void interpret_rshift(vm::Stack& stack, int round_mode) {
  int y = stack.pop_smallint_range(256);
  stack.push_int(rshift(stack.pop_int(), y, round_mode));
}

void interpret_lshift_const(vm::Stack& stack, int y) {
  stack.push_int(stack.pop_int() << y);
}

void interpret_rshift_const(vm::Stack& stack, int y) {
  stack.push_int(stack.pop_int() >> y);
}

void interpret_times_rshift(vm::Stack& stack, int round_mode) {
  int z = stack.pop_smallint_range(256);
  auto y = stack.pop_int();
  auto x = stack.pop_int();
  typename td::BigInt256::DoubleInt tmp{0};
  tmp.add_mul(*x, *y).rshift(z, round_mode).normalize();
  stack.push_int(td::make_refint(tmp));
}

void interpret_lshift_div(vm::Stack& stack, int round_mode) {
  int z = stack.pop_smallint_range(256);
  auto y = stack.pop_int();
  auto x = stack.pop_int();
  typename td::BigInt256::DoubleInt tmp{*x};
  tmp <<= z;
  auto q = td::make_refint();
  tmp.mod_div(*y, q.unique_write(), round_mode);
  q.unique_write().normalize();
  stack.push_int(std::move(q));
}

void interpret_not(vm::Stack& stack) {
  stack.push_int(~stack.pop_int());
}

void interpret_and(vm::Stack& stack) {
  stack.push_int(stack.pop_int() & stack.pop_int());
}

void interpret_or(vm::Stack& stack) {
  stack.push_int(stack.pop_int() | stack.pop_int());
}

void interpret_xor(vm::Stack& stack) {
  stack.push_int(stack.pop_int() ^ stack.pop_int());
}

void interpret_has_type(vm::Stack& stack, int t) {
  stack.push_bool(stack.pop_chk().type() == t);
}

void interpret_drop(vm::Stack& stack) {
  stack.check_underflow(1);
  stack.pop();
}

void interpret_2drop(vm::Stack& stack) {
  stack.check_underflow(2);
  stack.pop();
  stack.pop();
}

void interpret_dup(vm::Stack& stack) {
  stack.check_underflow(1);
  stack.push(stack.fetch(0));
}

void interpret_2dup(vm::Stack& stack) {
  stack.check_underflow(2);
  stack.push(stack.fetch(1));
  stack.push(stack.fetch(1));
}

void interpret_over(vm::Stack& stack) {
  stack.check_underflow(2);
  stack.push(stack.fetch(1));
}

void interpret_2over(vm::Stack& stack) {
  stack.check_underflow(4);
  stack.push(stack.fetch(3));
  stack.push(stack.fetch(3));
}

void interpret_swap(vm::Stack& stack) {
  stack.check_underflow(2);
  swap(stack[0], stack[1]);
}

void interpret_2swap(vm::Stack& stack) {
  stack.check_underflow(4);
  swap(stack[0], stack[2]);
  swap(stack[1], stack[3]);
}

void interpret_tuck(vm::Stack& stack) {
  stack.check_underflow(2);
  swap(stack[0], stack[1]);
  stack.push(stack.fetch(1));
}

void interpret_nip(vm::Stack& stack) {
  stack.check_underflow(2);
  stack.pop(stack[1]);
}

void interpret_rot(vm::Stack& stack) {
  stack.check_underflow(3);
  swap(stack[1], stack[2]);
  swap(stack[0], stack[1]);
}

void interpret_rot_rev(vm::Stack& stack) {
  stack.check_underflow(3);
  swap(stack[0], stack[1]);
  swap(stack[1], stack[2]);
}

void interpret_pick(vm::Stack& stack) {
  int n = stack.pop_smallint_range(255);
  stack.check_underflow(n + 1);
  stack.push(stack.fetch(n));
}

void interpret_roll(vm::Stack& stack) {
  int n = stack.pop_smallint_range(255);
  stack.check_underflow(n + 1);
  for (int i = n; i > 0; i--) {
    swap(stack[i], stack[i - 1]);
  }
}

void interpret_roll_rev(vm::Stack& stack) {
  int n = stack.pop_smallint_range(255);
  stack.check_underflow(n + 1);
  for (int i = 0; i < n; i++) {
    swap(stack[i], stack[i + 1]);
  }
}

void interpret_reverse(vm::Stack& stack) {
  int m = stack.pop_smallint_range(255);
  int n = stack.pop_smallint_range(255);
  stack.check_underflow(n + m);
  int s = 2 * m + n - 1;
  for (int i = ((s - 1) >> 1); i >= m; i--) {
    swap(stack[i], stack[s - i]);
  }
}

void interpret_exch(vm::Stack& stack) {
  int n = stack.pop_smallint_range(255);
  stack.check_underflow(n + 1);
  swap(stack[0], stack[n]);
}

void interpret_exch2(vm::Stack& stack) {
  int n = stack.pop_smallint_range(255);
  int m = stack.pop_smallint_range(255);
  stack.check_underflow(std::max(m, n) + 1);
  swap(stack[n], stack[m]);
}

void interpret_depth(vm::Stack& stack) {
  stack.push_smallint(stack.depth());
}

void interpret_xchg0(vm::Stack& stack, int x) {
  stack.check_underflow_p(x);
  std::swap(stack.tos(), stack.at(x));
}

void interpret_xchg(vm::Stack& stack, int x, int y) {
  stack.check_underflow_p(x, y);
  std::swap(stack.at(x), stack.at(y));
}

void interpret_push(vm::Stack& stack, int x) {
  stack.check_underflow_p(x);
  stack.push(stack.fetch(x));
}

void interpret_pop(vm::Stack& stack, int x) {
  stack.check_underflow_p(x);
  std::swap(stack.tos(), stack.at(x));
  stack.pop();
}

Ref<StackWord> dup_word_def{true, interpret_dup}, over_word_def{true, interpret_over},
    drop_word_def{true, interpret_drop}, nip_word_def{true, interpret_nip}, swap_word_def{true, interpret_swap};

void interpret_make_xchg(vm::Stack& stack) {
  using namespace std::placeholders;
  int y = stack.pop_smallint_range(255), x = stack.pop_smallint_range(255);
  if (x > y) {
    std::swap(x, y);
  }
  if (x) {
    stack.push_object(td::Ref<StackWord>{true, std::bind(interpret_xchg, _1, x, y)});
  } else if (y <= 1) {
    stack.push_object(y ? swap_word_def : nop_word_def);
  } else {
    stack.push_object(td::Ref<StackWord>{true, std::bind(interpret_xchg0, _1, y)});
  }
}

void interpret_make_push(vm::Stack& stack) {
  int x = stack.pop_smallint_range(255);
  if (x <= 1) {
    stack.push_object(x ? over_word_def : dup_word_def);
  } else {
    stack.push_object(td::Ref<StackWord>{true, std::bind(interpret_push, std::placeholders::_1, x)});
  }
}

void interpret_make_pop(vm::Stack& stack) {
  int x = stack.pop_smallint_range(255);
  if (x <= 1) {
    stack.push_object(x ? nip_word_def : drop_word_def);
  } else {
    stack.push_object(td::Ref<StackWord>{true, std::bind(interpret_pop, std::placeholders::_1, x)});
  }
}

void interpret_is_string(vm::Stack& stack) {
  stack.push_bool(stack.pop_chk().type() == vm::StackEntry::t_string);
}

int make_utf8_char(char buffer[4], int x) {
  if (x < -0x80) {
    return 0;
  } else if (x < 0x80) {
    buffer[0] = (char)x;
    return 1;
  } else if (x < 0x800) {
    buffer[0] = (char)(0xc0 + (x >> 6));
    buffer[1] = (char)(0x80 + (x & 0x3f));
    return 2;
  } else if (x < 0x10000) {
    buffer[0] = (char)(0xe0 + (x >> 12));
    buffer[1] = (char)(0x80 + ((x >> 6) & 0x3f));
    buffer[2] = (char)(0x80 + (x & 0x3f));
    return 3;
  } else if (x < 0x200000) {
    buffer[0] = (char)(0xf0 + (x >> 18));
    buffer[1] = (char)(0x80 + ((x >> 12) & 0x3f));
    buffer[2] = (char)(0x80 + ((x >> 6) & 0x3f));
    buffer[3] = (char)(0x80 + (x & 0x3f));
    return 4;
  } else {
    return 0;
  }
}

void interpret_chr(vm::Stack& stack) {
  char buffer[8];
  unsigned len = make_utf8_char(buffer, stack.pop_smallint_range(0x10ffff, -128));
  stack.push_string(std::string{buffer, len});
}

void interpret_hold(vm::Stack& stack) {
  stack.check_underflow(2);
  char buffer[8];
  unsigned len = make_utf8_char(buffer, stack.pop_smallint_range(0x10ffff, -128));
  std::string s = stack.pop_string();
  s.append(buffer, len);
  stack.push_string(std::move(s));
}

void interpret_emit(IntCtx& ctx) {
  char buffer[8];
  buffer[make_utf8_char(buffer, ctx.stack.pop_smallint_range(0x10ffff, -128))] = 0;
  *ctx.output_stream << buffer;
}

void interpret_emit_const(IntCtx& ctx, char c) {
  *ctx.output_stream << c;
}

void interpret_type(IntCtx& ctx) {
  std::string s = ctx.stack.pop_string();
  *ctx.output_stream << s;
}

void interpret_str_concat(vm::Stack& stack) {
  std::string t = stack.pop_string();
  stack.push_string(stack.pop_string() + t);
}

void interpret_str_equal(vm::Stack& stack) {
  stack.check_underflow(2);
  std::string t = stack.pop_string(), s = stack.pop_string();
  stack.push_bool(s == t);
}

void interpret_str_cmp(vm::Stack& stack) {
  stack.check_underflow(2);
  std::string t = stack.pop_string(), s = stack.pop_string();
  int res = s.compare(std::move(t));
  stack.push_smallint((res > 0) - (res < 0));
}

void interpret_str_len(vm::Stack& stack) {
  stack.push_smallint((long long)stack.pop_string().size());
}

void interpret_str_split(vm::Stack& stack) {
  stack.check_underflow(2);
  unsigned sz = stack.pop_smallint_range(0x7fffffff);
  std::string str = stack.pop_string();
  if (sz > str.size()) {
    throw IntError{"not enough bytes for cutting"};
  }
  stack.push_string(std::string{str, 0, sz});
  stack.push_string(std::string{str, sz});
}

void interpret_str_pos(vm::Stack& stack) {
  auto s2 = stack.pop_string(), s1 = stack.pop_string();
  auto pos = s1.find(s2);
  stack.push_smallint(pos == std::string::npos ? -1 : static_cast<long long>(pos));
}

void interpret_str_reverse(vm::Stack& stack) {
  std::string s = stack.pop_string();
  auto it = s.begin();
  while (it < s.end()) {
    if ((*it & 0xc0) != 0xc0) {
      ++it;
    } else {
      auto it0 = it++;
      while (it < s.end() && (*it & 0xc0) == 0x80) {
        ++it;
      }
      std::reverse(it0, it);
    }
  }
  std::reverse(s.begin(), s.end());
  stack.push_string(std::move(s));
}

void interpret_utf8_str_len(vm::Stack& stack) {
  std::string s = stack.pop_string();
  long long cnt = 0;
  for (char c : s) {
    if ((c & 0xc0) != 0x80) {
      cnt++;
    }
  }
  stack.push_smallint(cnt);
}

void interpret_utf8_str_split(vm::Stack& stack) {
  stack.check_underflow(2);
  unsigned c = stack.pop_smallint_range(0xffff);
  std::string s = stack.pop_string();
  if (c > s.size()) {
    throw IntError{"not enough utf8 characters for cutting"};
  }
  auto it = s.begin();
  for (; it < s.end(); ++it) {
    if ((*it & 0xc0) != 0x80) {
      if (!c) {
        stack.push_string(std::string{s.begin(), it});
        stack.push_string(std::string{it, s.end()});
        return;
      }
      --c;
    }
  }
  if (!c) {
    stack.push_string(std::move(s));
    stack.push_string(std::string{});
  } else {
    throw IntError{"not enough utf8 characters for cutting"};
  }
}

void interpret_utf8_str_pos(vm::Stack& stack) {
  auto s2 = stack.pop_string(), s1 = stack.pop_string();
  auto pos = s1.find(s2);
  if (pos == std::string::npos) {
    stack.push_smallint(-1);
    return;
  }
  int cnt = 0;
  for (std::size_t i = 0; i < pos; i++) {
    cnt += ((s1[i] & 0xc0) != 0x80);
  }
  stack.push_smallint(cnt);
}

void interpret_str_remove_trailing_int(vm::Stack& stack, int arg) {
  char x = (char)(arg ? arg : stack.pop_long_range(127));
  std::string s = stack.pop_string();
  s.resize(s.find_last_not_of(x) + 1);  // if not found, this expression will be 0
  stack.push_string(std::move(s));
}

void interpret_bytes_len(vm::Stack& stack) {
  stack.push_smallint((long long)stack.pop_bytes().size());
}

const char hex_digits[] = "0123456789abcdef";
const char HEX_digits[] = "0123456789ABCDEF";

static inline const char* hex_digits_table(bool upcase) {
  return upcase ? HEX_digits : hex_digits;
}

void interpret_bytes_hex_print_raw(IntCtx& ctx, bool upcase) {
  auto hex_digits = hex_digits_table(upcase);
  std::string str = ctx.stack.pop_bytes();
  for (unsigned c : str) {
    *ctx.output_stream << hex_digits[(c >> 4) & 15] << hex_digits[c & 15];
  }
}

void interpret_bytes_to_hex(vm::Stack& stack, bool upcase) {
  auto hex_digits = hex_digits_table(upcase);
  std::string str = stack.pop_bytes();
  std::string t(str.size() * 2, 0);
  for (std::size_t i = 0; i < str.size(); i++) {
    unsigned c = str[i];
    t[2 * i] = hex_digits[(c >> 4) & 15];
    t[2 * i + 1] = hex_digits[c & 15];
  }
  stack.push_string(std::move(t));
}

void interpret_hex_to_bytes(vm::Stack& stack, bool partial) {
  std::string str = stack.pop_string(), t;
  if (!partial) {
    if (str.size() & 1) {
      throw IntError{"not a hex string"};
    }
    t.reserve(str.size() >> 1);
  }
  std::size_t i;
  unsigned f = 0;
  for (i = 0; i < str.size(); i++) {
    int c = str[i];
    if (c >= '0' && c <= '9') {
      c -= '0';
    } else {
      c |= 0x20;
      if (c >= 'a' && c <= 'f') {
        c -= 'a' - 10;
      } else {
        if (!partial) {
          throw IntError{"not a hex string"};
        }
        break;
      }
    }
    f = (f << 4) + c;
    if (i & 1) {
      t += (char)(f & 0xff);
    }
  }
  stack.push_bytes(t);
  if (partial) {
    stack.push_smallint(i & -2);
  }
}

void interpret_bytes_split(vm::Stack& stack) {
  stack.check_underflow(2);
  unsigned sz = stack.pop_smallint_range(0x7fffffff);
  std::string str = stack.pop_bytes();
  if (sz > str.size()) {
    throw IntError{"not enough bytes for cutting"};
  }
  stack.push_bytes(std::string{str, 0, sz});
  stack.push_bytes(std::string{str, sz});
}

void interpret_bytes_concat(vm::Stack& stack) {
  std::string t = stack.pop_bytes();
  stack.push_bytes(stack.pop_bytes() + t);
}

void interpret_bytes_equal(vm::Stack& stack) {
  stack.check_underflow(2);
  std::string t = stack.pop_bytes(), s = stack.pop_bytes();
  stack.push_bool(s == t);
}

void interpret_bytes_cmp(vm::Stack& stack) {
  stack.check_underflow(2);
  std::string t = stack.pop_bytes(), s = stack.pop_bytes();
  int res = s.compare(std::move(t));
  stack.push_smallint((res > 0) - (res < 0));
}

void interpret_bytes_fetch_int(vm::Stack& stack, int mode) {
  stack.check_underflow(2);
  unsigned bits = (unsigned)stack.pop_smallint_range(256 + (mode & 1));
  std::string str = stack.pop_bytes();
  if ((bits & 7)) {
    throw IntError{"can load only an integer number of bytes"};
  }
  unsigned sz = bits >> 3;
  if (str.size() < sz) {
    throw IntError{"not enough bytes in the source"};
  }
  td::RefInt256 x{true};
  bool ok;
  const unsigned char* ptr = (const unsigned char*)(str.data());
  if (!(mode & 0x10)) {
    ok = x.write().import_bytes(ptr, sz, mode & 1);
  } else {
    ok = x.write().import_bytes_lsb(ptr, sz, mode & 1);
  }
  if (!ok) {
    throw IntError{"cannot load integer"};
  }
  if (mode & 2) {
    stack.push_bytes(std::string{str, sz});
  }
  stack.push_int(std::move(x));
}

void interpret_int_to_bytes(vm::Stack& stack, bool sgnd, bool lsb) {
  stack.check_underflow(2);
  unsigned bits = (unsigned)stack.pop_smallint_range(sgnd ? 264 : 256, 1);
  td::RefInt256 x = stack.pop_int();
  if ((bits & 7)) {
    throw IntError{"can store only an integer number of bytes"};
  }
  unsigned sz = bits >> 3;
  unsigned char buffer[33];
  if (!(lsb ? x->export_bytes_lsb(buffer, sz, sgnd) : x->export_bytes(buffer, sz, sgnd))) {
    throw IntError{"cannot store integer"};
  }
  stack.push_bytes(std::string{(char*)buffer, sz});
}

void interpret_string_to_bytes(vm::Stack& stack) {
  stack.push_bytes(stack.pop_string());
}

void interpret_bytes_to_string(vm::Stack& stack) {
  stack.push_string(stack.pop_bytes());
}

void interpret_bytes_hash(vm::Stack& stack, bool as_uint) {
  std::string str = stack.pop_bytes();
  unsigned char buffer[32];
  digest::hash_str<digest::SHA256>(buffer, str.c_str(), str.size());
  if (as_uint) {
    td::RefInt256 x{true};
    x.write().import_bytes(buffer, 32, false);
    stack.push_int(std::move(x));
  } else {
    stack.push_bytes(std::string{(char*)buffer, 32});
  }
}

void interpret_empty(vm::Stack& stack) {
  stack.push(td::Ref<vm::CellBuilder>{true});
}

void interpret_store(vm::Stack& stack, bool sgnd) {
  stack.check_underflow(3);
  int bits = stack.pop_smallint_range(1023);
  auto x = stack.pop_int();
  auto cell = stack.pop_builder();
  if (!cell.write().store_int256_bool(*x, bits, sgnd)) {
    throw IntError{"integer does not fit into cell"};
  }
  stack.push(cell);
}

void interpret_store_str(vm::Stack& stack) {
  stack.check_underflow(2);
  auto str = stack.pop_string();
  auto cell = stack.pop_builder();
  if (!cell.write().store_bytes_bool(str)) {
    throw IntError{"string does not fit into cell"};
  }
  stack.push(cell);
}

void interpret_store_bytes(vm::Stack& stack) {
  stack.check_underflow(2);
  auto str = stack.pop_bytes();
  auto cell = stack.pop_builder();
  if (!cell.write().store_bytes_bool(str)) {
    throw IntError{"byte string does not fit into cell"};
  }
  stack.push(cell);
}

void interpret_string_to_cellslice(vm::Stack& stack) {
  auto str = stack.pop_string();
  vm::CellBuilder cb;
  if (!cb.store_bytes_bool(str)) {
    throw IntError{"string does not fit into cell"};
  }
  stack.push_cellslice(td::Ref<vm::CellSlice>{true, cb.finalize()});
}

void interpret_store_cellslice(vm::Stack& stack) {
  stack.check_underflow(2);
  auto cs = stack.pop_cellslice();
  auto cb = stack.pop_builder();
  if (!vm::cell_builder_add_slice_bool(cb.write(), *cs)) {
    throw IntError{"slice does not fit into cell"};
  }
  stack.push(std::move(cb));
}

void interpret_store_cellslice_ref(vm::Stack& stack) {
  stack.check_underflow(2);
  auto cs = stack.pop_cellslice();
  vm::CellBuilder cs_cell_builder;
  vm::cell_builder_add_slice(cs_cell_builder, *cs);
  auto cb = stack.pop_builder();
  if (!cb.write().store_ref_bool(cs_cell_builder.finalize())) {
    throw IntError{"cell reference list overflow"};
  }
  stack.push(std::move(cb));
}

void interpret_concat_cellslice(vm::Stack& stack) {
  stack.check_underflow(2);
  auto cs2 = stack.pop_cellslice();
  auto cs1 = stack.pop_cellslice();
  vm::CellBuilder cb;
  if (vm::cell_builder_add_slice_bool(cb, *cs1) && vm::cell_builder_add_slice_bool(cb, *cs2)) {
    stack.push_cellslice(td::Ref<vm::CellSlice>{true, cb.finalize()});
  } else {
    throw IntError{"concatenation of two slices does not fit into a cell"};
  }
}

void interpret_concat_cellslice_ref(vm::Stack& stack) {
  stack.check_underflow(2);
  auto cs2 = stack.pop_cellslice();
  auto cs1 = stack.pop_cellslice();
  vm::CellBuilder builder1, builder2;
  vm::cell_builder_add_slice(builder1, *cs1);
  vm::cell_builder_add_slice(builder2, *cs2);
  if (!builder1.store_ref_bool(builder2.finalize())) {
    throw IntError{"cell reference list overflow"};
  }
  stack.push_cellslice(td::Ref<vm::CellSlice>{true, builder1.finalize()});
}

void interpret_concat_builders(vm::Stack& stack) {
  stack.check_underflow(2);
  auto cb2 = stack.pop_builder();
  auto cb1 = stack.pop_builder();
  if (!cb1.write().append_builder_bool(std::move(cb2))) {
    throw IntError{"cannot concatenate two builders"};
  }
  stack.push_builder(std::move(cb1));
}

void interpret_cell_datasize(vm::Stack& stack, int mode) {
  auto bound = (mode & 4 ? stack.pop_int() : td::make_refint(1 << 22));
  Ref<vm::Cell> cell;
  Ref<vm::CellSlice> cs;
  if (mode & 2) {
    cs = stack.pop_cellslice();
  } else {
    cell = stack.pop_maybe_cell();
  }
  if (!bound->is_valid() || bound->sgn() < 0) {
    throw IntError{"finite non-negative integer expected"};
  }
  vm::VmStorageStat stat{bound->unsigned_fits_bits(63) ? bound->to_long() : (1ULL << 63) - 1};
  bool ok = (mode & 2 ? stat.add_storage(cs.write()) : stat.add_storage(std::move(cell)));
  if (ok) {
    stack.push_smallint(stat.cells);
    stack.push_smallint(stat.bits);
    stack.push_smallint(stat.refs);
  } else if (!(mode & 1)) {
    throw IntError{"scanned too many cells"};
  }
  if (mode & 1) {
    stack.push_bool(ok);
  }
}

void interpret_slice_bitrefs(vm::Stack& stack, int mode) {
  auto cs = stack.pop_cellslice();
  if (mode & 1) {
    stack.push_smallint(cs->size());
  }
  if (mode & 2) {
    stack.push_smallint(cs->size_refs());
  }
}

void interpret_builder_bitrefs(vm::Stack& stack, int mode) {
  auto cb = stack.pop_builder();
  if (mode & 1) {
    stack.push_smallint(cb->size());
  }
  if (mode & 2) {
    stack.push_smallint(cb->size_refs());
  }
}

void interpret_builder_remaining_bitrefs(vm::Stack& stack, int mode) {
  auto cb = stack.pop_builder();
  if (mode & 1) {
    stack.push_smallint(cb->remaining_bits());
  }
  if (mode & 2) {
    stack.push_smallint(cb->remaining_refs());
  }
}

void interpret_cell_hash(vm::Stack& stack, bool as_uint) {
  auto cell = stack.pop_cell();
  if (as_uint) {
    td::RefInt256 hash{true};
    hash.write().import_bytes(cell->get_hash().as_slice().ubegin(), 32, false);
    stack.push_int(std::move(hash));
  } else {
    stack.push_bytes(cell->get_hash().as_slice().str());
  }
}

void interpret_store_ref(vm::Stack& stack) {
  auto ref = stack.pop_cell();
  auto cb = stack.pop_builder();
  if (!cb.write().store_ref_bool(ref)) {
    throw IntError{"cell reference list overflow"};
  }
  stack.push(std::move(cb));
}

void interpret_store_end(vm::Stack& stack, bool special) {
  auto cell = stack.pop_builder()->finalize_copy(special);
  if (cell.is_null()) {
    throw IntError{"invalid special cell constructed"};
  }
  stack.push_cell(std::move(cell));
}

void interpret_from_cell(vm::Stack& stack) {
  auto cell = stack.pop_cell();
  Ref<vm::CellSlice> cs{true, vm::NoVmOrd(), std::move(cell)};
  if (!cs->is_valid()) {
    throw IntError{"deserializing a special cell as ordinary"};
  }
  stack.push(cs);
}

// cs n -- cs' x
// cs n -- cs' x -1 OR cs' 0
// mode & 1 : signed
// mode & 2 : advance position
// mode & 4 : return error on stack
void interpret_fetch(vm::Stack& stack, int mode) {
  auto n = stack.pop_smallint_range(256 + (mode & 1));
  auto cs = stack.pop_cellslice();
  if (!cs->have(n)) {
    if (mode & 2) {
      stack.push(std::move(cs));
    }
    stack.push_bool(false);
    if (!(mode & 4)) {
      throw IntError{"end of data while reading integer from cell"};
    }
  } else {
    if (mode & 2) {
      stack.push_int(cs.write().fetch_int256(n, mode & 1));
      stack.push(std::move(cs));
    } else {
      stack.push_int(cs->prefetch_int256(n, mode & 1));
    }
    if (mode & 4) {
      stack.push_bool(true);
    }
  }
}

// mode & 1 : return result as bytes (instead of string)
// mode & 2 : advance position
// mode & 4 : return error on stack
void interpret_fetch_bytes(vm::Stack& stack, int mode) {
  unsigned n = stack.pop_smallint_range(127);
  auto cs = stack.pop_cellslice();
  if (!cs->have(n * 8)) {
    if (mode & 2) {
      stack.push(std::move(cs));
    }
    stack.push_bool(false);
    if (!(mode & 4)) {
      throw IntError{"end of data while reading byte string from cell"};
    }
  } else {
    // unfortunately, std::string's data() is writeable only in C++17
    unsigned char tmp[128];
    if (mode & 2) {
      cs.write().fetch_bytes(tmp, n);
    } else {
      cs->prefetch_bytes(tmp, n);
    }
    std::string s{tmp, tmp + n};
    if (mode & 1) {
      stack.push_bytes(std::move(s));
    } else {
      stack.push_string(std::move(s));
    }
    if (mode & 2) {
      stack.push(std::move(cs));
    }
    if (mode & 4) {
      stack.push_bool(true);
    }
  }
}

void interpret_fetch_slice(vm::Stack& stack, int mode) {
  unsigned refs = ((mode & 1) ? stack.pop_smallint_range(4) : 0);
  unsigned bits = stack.pop_smallint_range(1023);
  auto cs = stack.pop_cellslice();
  if (!cs->have(bits, refs)) {
    if (mode & 2) {
      stack.push(std::move(cs));
    }
    stack.push_bool(false);
    if (!(mode & 4)) {
      throw IntError{"end of data while fetching subslice from cell"};
    }
  } else {
    if (mode & 2) {
      stack.push(cs.write().fetch_subslice(bits, refs));
      stack.push(std::move(cs));
    } else {
      stack.push(cs->prefetch_subslice(bits, refs));
    }
    if (mode & 4) {
      stack.push_bool(true);
    }
  }
}

void interpret_cell_empty(vm::Stack& stack) {
  auto cs = stack.pop_cellslice();
  stack.push_bool(cs->empty_ext());
}

void interpret_cell_check_empty(vm::Stack& stack) {
  auto cs = stack.pop_cellslice();
  if (!cs->empty_ext()) {
    throw IntError{"cell slice not empty"};
  }
}

void interpret_cell_remaining(vm::Stack& stack) {
  auto cs = stack.pop_cellslice();
  stack.push_smallint(cs->size());
  stack.push_smallint(cs->size_refs());
}

// mode & 1 : return result as slice (instead of cell)
// mode & 2 : advance position
// mode & 4 : return error on stack
void interpret_fetch_ref(vm::Stack& stack, int mode) {
  auto cs = stack.pop_cellslice();
  if (!cs->have_refs(1)) {
    if (mode & 2) {
      stack.push(std::move(cs));
    }
    stack.push_bool(false);
    if (!(mode & 4)) {
      throw IntError{"end of data while reading reference from cell"};
    }
  } else {
    auto cell = (mode & 2) ? cs.write().fetch_ref() : cs->prefetch_ref();
    if (mode & 2) {
      stack.push(std::move(cs));
    }
    if (mode & 1) {
      Ref<vm::CellSlice> new_cs{true, vm::NoVmOrd(), std::move(cell)};
      if (!new_cs->is_valid()) {
        throw IntError{"cannot load ordinary cell"};
      }
      stack.push(std::move(new_cs));
    } else {
      stack.push_cell(std::move(cell));
    }
    if (mode & 4) {
      stack.push_bool(true);
    }
  }
}

// Box create/fetch/store operations

void interpret_hole(vm::Stack& stack) {
  stack.push_box(Ref<vm::Box>{true});
}

void interpret_box(vm::Stack& stack) {
  stack.push_box(Ref<vm::Box>{true, stack.pop_chk()});
}

void interpret_box_fetch(vm::Stack& stack) {
  stack.push(stack.pop_box()->get());
}

void interpret_box_store(vm::Stack& stack) {
  stack.check_underflow(2);
  auto box = stack.pop_box();
  box->set(stack.pop());
}

void interpret_push_null(vm::Stack& stack) {
  stack.push({});
}

void interpret_is_null(vm::Stack& stack) {
  stack.push_bool(stack.pop_chk().empty());
}

// Tuple/array operations

void interpret_empty_tuple(vm::Stack& stack) {
  stack.push_tuple(Ref<vm::Tuple>{true});
}

void interpret_is_tuple(vm::Stack& stack) {
  stack.push_bool(stack.pop_chk().type() == vm::StackEntry::t_tuple);
}

void interpret_tuple_push(vm::Stack& stack) {
  stack.check_underflow(2);
  auto val = stack.pop();
  auto tuple = stack.pop_tuple();
  tuple.write().emplace_back(std::move(val));
  stack.push_tuple(std::move(tuple));
}

void interpret_tuple_pop(vm::Stack& stack) {
  auto tuple = stack.pop_tuple();
  if (tuple->empty()) {
    throw IntError{"empty tuple"};
  }
  auto val = tuple->back();
  tuple.write().pop_back();
  stack.push_tuple(std::move(tuple));
  stack.push(std::move(val));
}

void interpret_tuple_len(vm::Stack& stack) {
  stack.push_smallint(stack.pop_tuple()->size());
}

void interpret_tuple_index(vm::Stack& stack) {
  auto idx = stack.pop_long_range(std::numeric_limits<long long>::max());
  auto tuple = stack.pop_tuple();
  if ((td::uint64)idx >= tuple->size()) {
    throw vm::VmError{vm::Excno::range_chk, "array index out of range"};
  }
  stack.push((*tuple)[td::narrow_cast<size_t>(idx)]);
}

void interpret_tuple_set(vm::Stack& stack) {
  auto idx = stack.pop_long_range(std::numeric_limits<long long>::max());
  auto val = stack.pop_chk();
  auto tuple = stack.pop_tuple();
  if ((td::uint64)idx >= tuple->size()) {
    throw vm::VmError{vm::Excno::range_chk, "array index out of range"};
  }
  tuple.write()[td::narrow_cast<size_t>(idx)] = std::move(val);
  stack.push_tuple(std::move(tuple));
}

void interpret_make_tuple(vm::Stack& stack) {
  int n = stack.pop_smallint_range(255);
  stack.check_underflow(n);
  Ref<vm::Tuple> ref{true};
  auto& tuple = ref.unique_write();
  tuple.reserve(n);
  for (int i = n - 1; i >= 0; i--) {
    tuple.push_back(std::move(stack[i]));
  }
  stack.pop_many(n);
  stack.push_tuple(std::move(ref));
}

void interpret_tuple_explode(vm::Stack& stack, bool pop_count) {
  std::size_t n = pop_count ? (unsigned)stack.pop_smallint_range(255) : 0;
  auto ref = stack.pop_tuple();
  const auto& tuple = *ref;
  if (!pop_count) {
    n = tuple.size();
    if (n > 255) {
      throw IntError{"tuple too large to be exploded"};
    }
  } else if (tuple.size() != n) {
    throw IntError{"tuple size mismatch"};
  }
  if (ref.is_unique()) {
    auto& tuplew = ref.unique_write();
    for (auto& entry : tuplew) {
      stack.push(std::move(entry));
    }
  } else {
    for (const auto& entry : tuple) {
      stack.push(entry);
    }
  }
  if (!pop_count) {
    stack.push_smallint((td::int32)n);
  }
}

void interpret_allot(vm::Stack& stack) {
  auto n = stack.pop_long_range(0xffffffff);
  Ref<vm::Tuple> ref{true};
  auto& tuple = ref.unique_write();
  tuple.reserve(td::narrow_cast<size_t>(n));
  while (n-- > 0) {
    tuple.emplace_back(Ref<vm::Box>{true});
  }
  stack.push(std::move(ref));
}

// Atoms

void interpret_atom(vm::Stack& stack) {
  bool create = stack.pop_bool();
  auto atom = vm::Atom::find(stack.pop_string(), create);
  if (atom.is_null()) {
    stack.push_bool(false);
  } else {
    stack.push_atom(std::move(atom));
    stack.push_bool(true);
  }
}

void interpret_atom_name(vm::Stack& stack) {
  stack.push_string(stack.pop_atom()->name_ext());
}

void interpret_atom_anon(vm::Stack& stack) {
  stack.push_atom(vm::Atom::anon());
}

void interpret_is_atom(vm::Stack& stack) {
  stack.push_bool(stack.pop_chk().is_atom());
}

bool are_eqv(vm::StackEntry x, vm::StackEntry y) {
  if (x.type() != y.type()) {
    return false;
  }
  switch (x.type()) {
    case vm::StackEntry::t_null:
      return true;
    case vm::StackEntry::t_atom:
      return std::move(x).as_atom() == std::move(y).as_atom();
    case vm::StackEntry::t_int:
      return !td::cmp(std::move(x).as_int(), std::move(y).as_int());
    case vm::StackEntry::t_string:
      return std::move(x).as_string() == std::move(y).as_string();
    default:
      return false;
  }
}

void interpret_is_eqv(vm::Stack& stack) {
  stack.check_underflow(2);
  auto y = stack.pop(), x = stack.pop();
  stack.push_bool(are_eqv(std::move(x), std::move(y)));
}

void interpret_is_eq(vm::Stack& stack) {
  stack.check_underflow(2);
  auto y = stack.pop(), x = stack.pop();
  stack.push_bool(x == y);
}

// BoC (de)serialization

void interpret_boc_serialize(vm::Stack& stack) {
  vm::BagOfCells boc;
  boc.add_root(stack.pop_cell());
  auto res = boc.import_cells();
  if (res.is_error()) {
    throw IntError{(PSLICE() << "cannot serialize bag-of-cells " << res.error()).c_str()};
  }
  stack.push_bytes(boc.serialize_to_string());
}

void interpret_boc_serialize_ext(vm::Stack& stack) {
  int mode = stack.pop_smallint_range(vm::BagOfCells::Mode::max);
  vm::BagOfCells boc;
  boc.add_root(stack.pop_cell());
  auto res = boc.import_cells();
  if (res.is_error()) {
    throw IntError{(PSLICE() << "cannot serialize bag-of-cells " << res.error()).c_str()};
  }
  stack.push_bytes(boc.serialize_to_string(mode));
}

void interpret_boc_deserialize(vm::Stack& stack) {
  std::string bytes = stack.pop_bytes();
  vm::BagOfCells boc;
  auto res = boc.deserialize(td::Slice{bytes});
  if (res.is_error()) {
    throw IntError{(PSLICE() << "cannot deserialize bag-of-cells " << res.error()).c_str()};
  }
  if (res.ok() <= 0 || boc.get_root_cell().is_null()) {
    throw IntError{"cannot deserialize bag-of-cells "};
  }
  stack.push_cell(boc.get_root_cell());
}

void interpret_read_file(IntCtx& ctx) {
  std::string filename = ctx.stack.pop_string();
  auto r_data = ctx.source_lookup->read_file(filename);
  if (r_data.is_error()) {
    throw IntError{PSTRING() << "error reading file `" << filename << "`: " << r_data.error()};
  }
  ctx.stack.push_bytes(r_data.move_as_ok().data);
}

void interpret_read_file_part(IntCtx& ctx) {
  auto size = ctx.stack.pop_long_range(std::numeric_limits<long long>::max());
  auto offset = ctx.stack.pop_long_range(std::numeric_limits<long long>::max());
  std::string filename = ctx.stack.pop_string();
  auto r_data = ctx.source_lookup->read_file_part(filename, size, offset);
  if (r_data.is_error()) {
    throw IntError{PSTRING() << "error reading file `" << filename << "`: " << r_data.error()};
  }
  ctx.stack.push_bytes(r_data.move_as_ok().data);
}

void interpret_write_file(IntCtx& ctx) {
  std::string filename = ctx.stack.pop_string();
  std::string str = ctx.stack.pop_bytes();
  auto status = ctx.source_lookup->write_file(filename, str);
  if (status.is_error()) {
    throw IntError{PSTRING() << "error writing file `" << filename << "`: " << status.error()};
  }
}

void interpret_file_exists(IntCtx& ctx) {
  std::string filename = ctx.stack.pop_string();
  auto res = ctx.source_lookup->is_file_exists(filename);
  ctx.stack.push_bool(res);
}

// custom and crypto

void interpret_now(IntCtx& ctx) {
  ctx.stack.push_smallint(ctx.source_lookup->now());
}

void interpret_new_keypair(vm::Stack& stack) {
  auto priv_key = td::Ed25519::generate_private_key();
  if (!priv_key.is_ok()) {
    throw fift::IntError{priv_key.error().to_string()};
  }
  auto pub_key = priv_key.ok().get_public_key();
  if (!pub_key.is_ok()) {
    throw fift::IntError{pub_key.error().to_string()};
  }
  stack.push_bytes(priv_key.ok().as_octet_string());
  stack.push_bytes(pub_key.ok().as_octet_string());
}

void interpret_priv_key_to_pub(vm::Stack& stack) {
  std::string str = stack.pop_bytes();
  if (str.size() != 32) {
    throw IntError{"Ed25519 private key must be exactly 32 bytes long"};
  }
  td::Ed25519::PrivateKey priv_key{td::SecureString{str}};
  auto pub_key = priv_key.get_public_key();
  if (!pub_key.is_ok()) {
    throw fift::IntError{pub_key.error().to_string()};
  }
  stack.push_bytes(pub_key.ok().as_octet_string());
}

void interpret_ed25519_sign(vm::Stack& stack) {
  stack.check_underflow(2);
  std::string key = stack.pop_bytes(), data = stack.pop_bytes();
  if (key.size() != 32) {
    throw IntError{"Ed25519 private key must be exactly 32 bytes long"};
  }
  td::Ed25519::PrivateKey priv_key{td::SecureString{key}};
  auto signature = priv_key.sign(td::Slice{data});
  if (!signature.is_ok()) {
    throw fift::IntError{signature.error().to_string()};
  }
  stack.push_bytes(signature.move_as_ok());
}

void interpret_ed25519_sign_uint(vm::Stack& stack) {
  stack.check_underflow(2);
  std::string key = stack.pop_bytes();
  td::RefInt256 data_int = stack.pop_int();
  if (key.size() != 32) {
    throw IntError{"Ed25519 private key must be exactly 32 bytes long"};
  }
  unsigned char data[32];
  if (!data_int->export_bytes(data, 32, false)) {
    throw IntError{"Ed25519 data to be signed must fit into 256 bits"};
  }
  td::Ed25519::PrivateKey priv_key{td::SecureString{key}};
  auto signature = priv_key.sign(td::Slice{data, 32});
  if (!signature.is_ok()) {
    throw fift::IntError{signature.error().to_string()};
  }
  stack.push_bytes(signature.move_as_ok());
}

void interpret_ed25519_chksign(vm::Stack& stack) {
  stack.check_underflow(3);
  std::string key = stack.pop_bytes(), signature = stack.pop_bytes(), data = stack.pop_bytes();
  if (key.size() != 32) {
    throw IntError{"Ed25519 public key must be exactly 32 bytes long"};
  }
  if (signature.size() != 64) {
    throw IntError{"Ed25519 signature must be exactly 64 bytes long"};
  }
  td::Ed25519::PublicKey pub_key{td::SecureString{key}};
  auto res = pub_key.verify_signature(td::Slice{data}, td::Slice{signature});
  stack.push_bool(res.is_ok());
}

void interpret_crc16(vm::Stack& stack) {
  std::string str = stack.pop_bytes();
  stack.push_smallint(td::crc16(td::Slice{str}));
}

void interpret_crc32(vm::Stack& stack) {
  std::string str = stack.pop_bytes();
  stack.push_smallint(td::crc32(td::Slice{str}));
}

void interpret_crc32c(vm::Stack& stack) {
  std::string str = stack.pop_bytes();
  stack.push_smallint(td::crc32c(td::Slice{str}));
}

// Fift hashmaps

void push_hmap(vm::Stack& stack, Ref<Hashmap> hmap) {
  if (hmap.not_null()) {
    stack.push_object(std::move(hmap));
  } else {
    stack.push({});
  }
}

void push_hmap(vm::Stack& stack, HashmapKeeper hmap_keep) {
  push_hmap(stack, hmap_keep.extract());
}

Ref<Hashmap> pop_hmap(vm::Stack& stack) {
  stack.check_underflow(1);
  auto se = stack.pop();
  if (se.is_null()) {
    return {};
  }
  auto hmap_ref = std::move(se).as_object<Hashmap>();
  if (hmap_ref.is_null()) {
    throw IntError{"hashmap expected"};
  }
  return hmap_ref;
}

HashmapKeeper pop_hmap_keeper(vm::Stack& stack) {
  return HashmapKeeper{pop_hmap(stack)};
}

void interpret_hmap_new(vm::Stack& stack) {
  stack.push({});
}

void interpret_hmap_fetch(vm::Stack& stack, int mode) {
  auto hmap = pop_hmap(stack);
  auto value = Hashmap::get(std::move(hmap), stack.pop_chk());
  bool found = !value.is_null();
  if ((mode & 8) && !found) {
    throw IntError{"hashmap key not found"};
  }
  if (mode & (2 << (int)found)) {
    stack.push(std::move(value));
  }
  if (mode & 1) {
    stack.push_bool(found);
  }
}

void interpret_hmap_delete(vm::Stack& stack, int mode) {
  auto hmap = pop_hmap(stack);
  auto res = Hashmap::get_remove(std::move(hmap), stack.pop_chk());
  push_hmap(stack, std::move(res.first));
  bool found = !res.second.is_null();
  if ((mode & 8) && !found) {
    throw IntError{"hashmap key not found"};
  }
  if (mode & (2 << (int)found)) {
    stack.push(std::move(res.second));
  }
  if (mode & 1) {
    stack.push_bool(found);
  }
}

void interpret_hmap_store(vm::Stack& stack, int mode) {
  stack.check_underflow(3);
  auto hmap = pop_hmap_keeper(stack);
  auto key = stack.pop(), value = stack.pop();
  bool ok = true;
  if (mode & 1) {
    hmap.set(std::move(key), std::move(value));
  } else {
    ok = hmap.replace(std::move(key), std::move(value));
  }
  push_hmap(stack, std::move(hmap));
  if (mode & 2) {
    stack.push_bool(ok);
  }
}

void interpret_hmap_is_empty(vm::Stack& stack) {
  stack.push_bool(pop_hmap(stack).is_null());
}

void interpret_hmap_decompose(vm::Stack& stack, int mode) {
  auto hmap = pop_hmap(stack);
  if (hmap.is_null()) {
    if (mode & 1) {
      stack.push_bool(false);
    } else {
      throw IntError{"empty hmap"};
    }
    return;
  }
  stack.push(hmap->key());
  stack.push(hmap->value());
  push_hmap(stack, hmap->left());
  push_hmap(stack, hmap->right());
  if (mode & 1) {
    stack.push_bool(true);
  }
}

class HmapIterCont : public LoopCont {
  HashmapIterator it;
  bool ok;

 public:
  HmapIterCont(Ref<FiftCont> _func, Ref<FiftCont> _after, HashmapIterator _it)
      : LoopCont(std::move(_func), std::move(_after)), it(std::move(_it)), ok(true) {
  }
  HmapIterCont(const HmapIterCont&) = default;
  HmapIterCont* make_copy() const override {
    return new HmapIterCont(*this);
  }
  bool init(IntCtx& ctx) override {
    return true;
  }
  bool pre_exec(IntCtx& ctx) override {
    if (it.eof()) {
      return false;
    } else {
      ctx.stack.push(it->key());
      ctx.stack.push(it->value());
      return true;
    }
  }
  bool post_exec(IntCtx& ctx) override {
    ok = ctx.stack.pop_bool();
    return ok && it.next();
  }
  bool finalize(IntCtx& ctx) override {
    ctx.stack.push_bool(ok);
    return true;
  }
};

Ref<FiftCont> interpret_hmap_foreach(IntCtx& ctx, int mode) {
  auto func = pop_exec_token(ctx);
  return td::make_ref<HmapIterCont>(std::move(func), std::move(ctx.next), pop_hmap_keeper(ctx).begin(mode & 1));
}

// vm dictionaries
void interpret_dict_new(vm::Stack& stack) {
  stack.push({});
}

void interpret_dict_to_slice(vm::Stack& stack) {
  vm::CellBuilder cb;
  cb.store_maybe_ref(stack.pop_maybe_cell());
  stack.push_cellslice(vm::load_cell_slice_ref(cb.finalize()));
}

void interpret_load_dict(vm::Stack& stack, bool fetch) {
  auto cs = stack.pop_cellslice();
  Ref<vm::Cell> dict;
  bool non_empty;
  if (!(cs.write().fetch_bool_to(non_empty) && (!non_empty || cs.write().fetch_ref_to(dict)))) {
    throw IntError{"cell underflow"};
  }
  stack.push_maybe_cell(std::move(dict));
  if (fetch) {
    stack.push_cellslice(std::move(cs));
  }
}

void interpret_store_dict(vm::Stack& stack) {
  auto cell = stack.pop_maybe_cell();
  auto cb = stack.pop_builder();
  if (!cb.write().store_maybe_ref(std::move(cell))) {
    throw IntError{"cell overflow"};
  }
  stack.push_builder(std::move(cb));
}

// val key dict keylen -- dict' ?
void interpret_dict_add(vm::Stack& stack, vm::Dictionary::SetMode mode, bool add_builder, int sgnd) {
  int n = stack.pop_smallint_range(vm::Dictionary::max_key_bits);
  vm::Dictionary dict{stack.pop_maybe_cell(), n};
  unsigned char buffer[vm::Dictionary::max_key_bytes];
  vm::BitSlice key =
      (sgnd >= 0) ? dict.integer_key(stack.pop_int(), n, sgnd, buffer) : stack.pop_cellslice()->prefetch_bits(n);
  if (!key.is_valid()) {
    throw IntError{"not enough bits for a dictionary key"};
  }
  bool res;
  if (add_builder) {
    res = dict.set_builder(std::move(key), stack.pop_builder(), mode);
  } else {
    res = dict.set(std::move(key), stack.pop_cellslice(), mode);
  }
  stack.push_maybe_cell(std::move(dict).extract_root_cell());
  stack.push_bool(res);
}

void interpret_dict_get(vm::Stack& stack, int sgnd, int mode) {
  int n = stack.pop_smallint_range(vm::Dictionary::max_key_bits);
  vm::Dictionary dict{stack.pop_maybe_cell(), n};
  unsigned char buffer[vm::Dictionary::max_key_bytes];
  vm::BitSlice key =
      (sgnd >= 0) ? dict.integer_key(stack.pop_int(), n, sgnd, buffer) : stack.pop_cellslice()->prefetch_bits(n);
  if (!key.is_valid()) {
    throw IntError{"not enough bits for a dictionary key"};
  }
  auto res = (mode & 4 ? dict.lookup_delete(std::move(key)) : dict.lookup(std::move(key)));
  if (mode & 4) {
    stack.push_maybe_cell(std::move(dict).extract_root_cell());
  }
  bool found = res.not_null();
  if (found && (mode & 2)) {
    stack.push_cellslice(std::move(res));
  }
  if (mode & 1) {
    stack.push_bool(found);
  }
}

class DictMapCont : public LoopCont {
  int n;
  bool ext;
  bool sgnd;
  vm::Dictionary dict, dict2;
  vm::DictIterator it;

 public:
  DictMapCont(Ref<FiftCont> _func, Ref<FiftCont> _after, int _n, Ref<vm::Cell> dict_root, bool _ext, bool _sgnd)
      : LoopCont(std::move(_func), std::move(_after))
      , n(_n)
      , ext(_ext)
      , sgnd(_sgnd)
      , dict(std::move(dict_root), n)
      , dict2(n) {
  }
  DictMapCont(const DictMapCont&) = default;
  DictMapCont* make_copy() const override {
    return new DictMapCont(*this);
  }
  bool init(IntCtx& ctx) override {
    it = dict.init_iterator(false, sgnd);
    return true;
  }
  bool pre_exec(IntCtx& ctx) override;
  bool post_exec(IntCtx& ctx) override;
  bool finalize(IntCtx& ctx) override;
};

bool DictMapCont::pre_exec(IntCtx& ctx) {
  if (it.eof()) {
    return false;
  }
  ctx.stack.push_builder(td::make_ref<vm::CellBuilder>());
  if (ext) {
    ctx.stack.push_int(dict.key_as_integer(it.cur_pos(), sgnd));
  }
  ctx.stack.push_cellslice(it.cur_value());
  return true;
}

bool DictMapCont::post_exec(IntCtx& ctx) {
  if (ctx.stack.pop_bool()) {
    if (!dict2.set_builder(it.cur_pos(), n, ctx.stack.pop_builder())) {
      throw IntError{"cannot insert value into dictionary"};
    }
  }
  return !(++it).eof();
}

bool DictMapCont::finalize(IntCtx& ctx) {
  ctx.stack.push_maybe_cell(std::move(dict2).extract_root_cell());
  return true;
}

Ref<FiftCont> interpret_dict_map(IntCtx& ctx, bool ext, bool sgnd) {
  auto func = pop_exec_token(ctx);
  int n = ctx.stack.pop_smallint_range(vm::Dictionary::max_key_bits);
  return td::make_ref<DictMapCont>(std::move(func), std::move(ctx.next), n, ctx.stack.pop_maybe_cell(), ext, sgnd);
}

class DictIterCont : public LoopCont {
  int n;
  bool reverse;
  bool sgnd;
  bool ok;
  bool inited{false};
  vm::Dictionary dict;
  vm::DictIterator it;

 public:
  DictIterCont(Ref<FiftCont> _func, Ref<FiftCont> _after, int _n, Ref<vm::Cell> dict_root, bool _reverse, bool _sgnd)
      : LoopCont(std::move(_func), std::move(_after))
      , n(_n)
      , reverse(_reverse)
      , sgnd(_sgnd)
      , ok(true)
      , dict(std::move(dict_root), n) {
  }
  DictIterCont(const DictIterCont&) = default;
  DictIterCont* make_copy() const override {
    return new DictIterCont(*this);
  }
  bool do_init();
  bool init(IntCtx& ctx) override {
    return do_init();
  }
  bool pre_exec(IntCtx& ctx) override;
  bool post_exec(IntCtx& ctx) override;
  bool finalize(IntCtx& ctx) override;
  template <typename T>
  bool lookup(const T& key, bool strict, bool backw) {
    return do_init() && it.lookup(key, strict, backw);
  }
};

bool DictIterCont::do_init() {
  if (!inited) {
    it = dict.init_iterator(reverse, sgnd);
    inited = true;
  }
  return true;
}

bool DictIterCont::pre_exec(IntCtx& ctx) {
  if (it.eof()) {
    return false;
  }
  ctx.stack.push_int(dict.key_as_integer(it.cur_pos(), sgnd));
  ctx.stack.push_cellslice(it.cur_value());
  return true;
}

bool DictIterCont::post_exec(IntCtx& ctx) {
  ok = ctx.stack.pop_bool();
  return ok && !(++it).eof();
}

bool DictIterCont::finalize(IntCtx& ctx) {
  ctx.stack.push_bool(ok);
  return true;
}

Ref<FiftCont> interpret_dict_foreach(IntCtx& ctx, bool reverse, bool sgnd) {
  auto func = pop_exec_token(ctx);
  int n = ctx.stack.pop_smallint_range(vm::Dictionary::max_key_bits);
  return td::make_ref<DictIterCont>(std::move(func), std::move(ctx.next), n, ctx.stack.pop_maybe_cell(), reverse, sgnd);
}

// mode: +1 = reverse, +2 = signed, +4 = strict, +8 = lookup backwards, +16 = with hint
Ref<FiftCont> interpret_dict_foreach_from(IntCtx& ctx, int mode) {
  if (mode < 0) {
    mode = ctx.stack.pop_smallint_range(31);
  }
  auto func = pop_exec_token(ctx);
  int n = ctx.stack.pop_smallint_range(vm::Dictionary::max_key_bits);
  auto it_cont = td::make_ref<DictIterCont>(std::move(func), std::move(ctx.next), n, ctx.stack.pop_maybe_cell(),
                                            mode & 1, mode & 2);
  for (int s = (mode >> 4) & 1; s >= 0; --s) {
    unsigned char buffer[vm::Dictionary::max_key_bytes];
    auto key = vm::Dictionary::integer_key(ctx.stack.pop_int(), n, mode & 2, buffer);
    if (!key.is_valid()) {
      throw IntError{"not enough bits for a dictionary key"};
    }
    it_cont.write().lookup(key, mode & 4, mode & 8);
  }
  return it_cont;
}

class DictMergeCont : public LoopCont {
  int n;
  vm::Dictionary dict1, dict2, dict3;
  vm::DictIterator it1, it2;

 public:
  DictMergeCont(Ref<FiftCont> _func, Ref<FiftCont> _after, int _n, Ref<vm::Cell> dict1_root, Ref<vm::Cell> dict2_root)
      : LoopCont(std::move(_func), std::move(_after))
      , n(_n)
      , dict1(std::move(dict1_root), n)
      , dict2(std::move(dict2_root), n)
      , dict3(n) {
  }
  DictMergeCont(const DictMergeCont&) = default;
  DictMergeCont* make_copy() const override {
    return new DictMergeCont(*this);
  }
  bool init(IntCtx& ctx) override {
    it1 = dict1.begin();
    it2 = dict2.begin();
    return true;
  }
  bool pre_exec(IntCtx& ctx) override;
  bool post_exec(IntCtx& ctx) override;
  bool finalize(IntCtx& ctx) override;
};

bool DictMergeCont::pre_exec(IntCtx& ctx) {
  while (!it1.eof() || !it2.eof()) {
    int c = it1.eof() ? 1 : (it2.eof() ? -1 : it1.cur_pos().compare(it2.cur_pos(), n));
    bool ok = true;
    if (c < 0) {
      ok = dict3.set(it1.cur_pos(), n, it1.cur_value());
      ++it1;
    } else if (c > 0) {
      ok = dict3.set(it2.cur_pos(), n, it2.cur_value());
      ++it2;
    } else {
      ctx.stack.push_builder(Ref<vm::CellBuilder>{true});
      ctx.stack.push_cellslice(it1.cur_value());
      ctx.stack.push_cellslice(it2.cur_value());
      return true;
    }
    if (!ok) {
      throw IntError{"cannot insert value into dictionary"};
    }
  }
  return false;
}

bool DictMergeCont::post_exec(IntCtx& ctx) {
  if (ctx.stack.pop_bool() && !dict3.set_builder(it1.cur_pos(), n, ctx.stack.pop_builder())) {
    throw IntError{"cannot insert value into dictionary"};
  }
  ++it1;
  ++it2;
  return true;
}

bool DictMergeCont::finalize(IntCtx& ctx) {
  ctx.stack.push_maybe_cell(std::move(dict3).extract_root_cell());
  return true;
}

Ref<FiftCont> interpret_dict_merge(IntCtx& ctx) {
  auto func = pop_exec_token(ctx);
  int n = ctx.stack.pop_smallint_range(vm::Dictionary::max_key_bits);
  auto dict2_root = ctx.stack.pop_maybe_cell();
  return td::make_ref<DictMergeCont>(std::move(func), std::move(ctx.next), n, ctx.stack.pop_maybe_cell(),
                                     std::move(dict2_root));
}

class DictDiffCont : public LoopCont {
  int n;
  bool ok{true};
  vm::Dictionary dict1, dict2;
  vm::DictIterator it1, it2;

 public:
  DictDiffCont(Ref<FiftCont> _func, Ref<FiftCont> _after, int _n, Ref<vm::Cell> dict1_root, Ref<vm::Cell> dict2_root)
      : LoopCont(std::move(_func), std::move(_after))
      , n(_n)
      , dict1(std::move(dict1_root), n)
      , dict2(std::move(dict2_root), n) {
  }
  DictDiffCont(const DictDiffCont&) = default;
  DictDiffCont* make_copy() const override {
    return new DictDiffCont(*this);
  }
  bool init(IntCtx& ctx) override {
    it1 = dict1.begin();
    it2 = dict2.begin();
    return true;
  }
  bool pre_exec(IntCtx& ctx) override;
  bool post_exec(IntCtx& ctx) override;
  bool finalize(IntCtx& ctx) override;
};

bool DictDiffCont::pre_exec(IntCtx& ctx) {
  while (!it1.eof() || !it2.eof()) {
    int c = it1.eof() ? 1 : (it2.eof() ? -1 : it1.cur_pos().compare(it2.cur_pos(), n));
    if (c < 0) {
      ctx.stack.push_int(dict1.key_as_integer(it1.cur_pos()));
      ctx.stack.push_cellslice(it1.cur_value());
      ctx.stack.push_null();
      ++it1;
    } else if (c > 0) {
      ctx.stack.push_int(dict2.key_as_integer(it2.cur_pos()));
      ctx.stack.push_null();
      ctx.stack.push_cellslice(it2.cur_value());
      ++it2;
    } else {
      if (!it1.cur_value()->contents_equal(*it2.cur_value())) {
        ctx.stack.push_int(dict1.key_as_integer(it1.cur_pos()));
        ctx.stack.push_cellslice(it1.cur_value());
        ctx.stack.push_cellslice(it2.cur_value());
      } else {
        ++it1;
        ++it2;
        continue;
      }
      ++it1;
      ++it2;
    }
    return true;
  }
  return false;
}

bool DictDiffCont::post_exec(IntCtx& ctx) {
  return (ok = ctx.stack.pop_bool());
}

bool DictDiffCont::finalize(IntCtx& ctx) {
  ctx.stack.push_bool(ok);
  return true;
}

Ref<FiftCont> interpret_dict_diff(IntCtx& ctx) {
  auto func = pop_exec_token(ctx);
  int n = ctx.stack.pop_smallint_range(vm::Dictionary::max_key_bits);
  auto dict2_root = ctx.stack.pop_maybe_cell();
  return td::make_ref<DictDiffCont>(std::move(func), std::move(ctx.next), n, ctx.stack.pop_maybe_cell(),
                                    std::move(dict2_root));
}

void interpret_pfx_dict_add(vm::Stack& stack, vm::Dictionary::SetMode mode, bool add_builder) {
  int n = stack.pop_smallint_range(vm::Dictionary::max_key_bits);
  vm::PrefixDictionary dict{stack.pop_maybe_cell(), n};
  auto cs = stack.pop_cellslice();
  bool res;
  if (add_builder) {
    res = dict.set_builder(cs->data_bits(), cs->size(), stack.pop_builder(), mode);
  } else {
    res = dict.set(cs->data_bits(), cs->size(), stack.pop_cellslice(), mode);
  }
  stack.push_maybe_cell(std::move(dict).extract_root_cell());
  stack.push_bool(res);
}

void interpret_pfx_dict_get(vm::Stack& stack) {
  int n = stack.pop_smallint_range(vm::Dictionary::max_key_bits);
  vm::PrefixDictionary dict{stack.pop_maybe_cell(), n};
  auto cs = stack.pop_cellslice();
  auto res = dict.lookup(cs->data_bits(), cs->size());
  if (res.not_null()) {
    stack.push_cellslice(std::move(res));
    stack.push_bool(true);
  } else {
    stack.push_bool(false);
  }
}

void interpret_bitstring_hex_literal(IntCtx& ctx) {
  auto s = ctx.parser->scan_word_to('}');
  unsigned char buff[128];
  int bits = (int)td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), s.begin(), s.end());
  vm::CellBuilder cb;
  if (bits < 0 || !cb.store_bits_bool(td::ConstBitPtr{buff}, bits)) {
    throw IntError{"Invalid hex bitstring constant"};
  }
  ctx.stack.push(cb.as_cellslice_ref());
  push_argcount(ctx, 1);
}

void interpret_bitstring_binary_literal(IntCtx& ctx) {
  auto s = ctx.parser->scan_word_to('}');
  unsigned char buff[128];
  int bits = (int)td::bitstring::parse_bitstring_binary_literal(buff, sizeof(buff) * 8, s.begin(), s.end());
  vm::CellBuilder cb;
  if (bits < 0 || !cb.store_bits_bool(td::ConstBitPtr{buff}, bits)) {
    throw IntError{"Invalid binary bitstring constant"};
  }
  ctx.stack.push(cb.as_cellslice_ref());
  push_argcount(ctx, 1);
}

void interpret_word(IntCtx& ctx) {
  char sep = (char)ctx.stack.pop_smallint_range(127);
  auto word = (sep != ' ' ? ctx.parser->scan_word_to(sep, true) : ctx.parser->scan_word());
  ctx.stack.push_string(word);
}

void interpret_word_ext(IntCtx& ctx) {
  int mode = ctx.stack.pop_smallint_range(11);
  auto delims = ctx.stack.pop_string();
  if (mode & 8) {
    ctx.parser->skipspc(mode & 4);
  }
  ctx.stack.push_string(ctx.parser->scan_word_ext(CharClassifier{delims, mode & 3}));
}

void interpret_skipspc(IntCtx& ctx) {
  ctx.parser->skipspc();
}

void interpret_wordlist_begin_aux(vm::Stack& stack) {
  stack.push_make_object<WordList>();
}

void interpret_wordlist_begin(IntCtx& ctx) {
  ctx.check_not_int_exec();
  interpret_wordlist_begin_aux(ctx.stack);
  push_argcount(ctx, 0);
  ++(ctx.state);
}

void interpret_wordlist_end_aux(vm::Stack& stack) {
  Ref<WordList> wordlist_ref = pop_word_list(stack);
  wordlist_ref.write().close();
  stack.push_object(std::move(wordlist_ref));
}

void interpret_wordlist_end(IntCtx& ctx) {
  ctx.check_compile();
  interpret_wordlist_end_aux(ctx.stack);
  push_argcount(ctx, 1);
  --(ctx.state);
}

void interpret_internal_interpret_begin(IntCtx& ctx) {
  ctx.check_compile();
  push_argcount(ctx, 0);
  ctx.state = -ctx.state;
}

void interpret_internal_interpret_end(IntCtx& ctx) {
  ctx.check_int_exec();
  ctx.state = -ctx.state;
  ctx.stack.push_object(nop_word_def);
}

// (create)
// maybe need an extra argument to identify the vocabulary (namespace) to be edited
void interpret_create_aux(IntCtx& ctx, int mode) {
  if (mode < 0) {
    mode = ctx.stack.pop_smallint_range(3);
  }
  std::string word = ctx.stack.pop_string();
  if (!word.size()) {
    throw IntError{"non-empty word name expected"};
  }
  auto wd_ref = pop_exec_token(ctx.stack);
  if (!(mode & 2)) {
    word += ' ';
  }
  ctx.dictionary.def_word(std::move(word), {std::move(wd_ref), (bool)(mode & 1)});
}

// { bl word 0 (create) } : create
void interpret_create(IntCtx& ctx) {
  auto word = ctx.parser->scan_word();
  if (!word.size()) {
    throw IntError{"non-empty word name expected"};
  }
  ctx.stack.push_string(word);
  interpret_create_aux(ctx, 0);
}

Ref<FiftCont> create_aux_wd{Ref<CtxWord>{true, std::bind(interpret_create_aux, std::placeholders::_1, -1)}};

// { bl word <mode> 2 ' (create) } :: :
void interpret_colon(IntCtx& ctx, int mode) {
  ctx.stack.push_string(ctx.parser->scan_word());
  ctx.stack.push_smallint(mode);
  ctx.stack.push_smallint(2);
  ctx.stack.push_object(create_aux_wd);
  //push_argcount(ctx, 2, create_wd);
}

// (forget)
void interpret_forget_aux(IntCtx& ctx) {
  std::string s = ctx.stack.pop_string();
  auto s_copy = s;
  auto entry = ctx.dictionary.lookup(s);
  if (!entry) {
    s += " ";
    entry = ctx.dictionary.lookup(s);
  }
  if (!entry) {
    throw IntError{"`" + s_copy + "` not found"};
  } else {
    ctx.dictionary.undef_word(s);
  }
}

// { bl word (forget) } : forget
void interpret_forget(IntCtx& ctx) {
  ctx.stack.push_string(ctx.parser->scan_word());
  interpret_forget_aux(ctx);
}

void interpret_quote_str(IntCtx& ctx) {
  ctx.stack.push_string(ctx.parser->scan_word_to('"'));
  push_argcount(ctx, 1);
}

int str_utf8_code(const char* str, int& len) {
  if (len <= 0) {
    return -1;
  }
  if (len >= 1 && (unsigned char)str[0] < 0x80) {
    len = 1;
    return str[0];
  }
  if (len >= 2 && (str[0] & 0xe0) == 0xc0 && (str[1] & 0xc0) == 0x80) {
    len = 2;
    return ((str[0] & 0x1f) << 6) | (str[1] & 0x3f);
  }
  if (len >= 3 && (str[0] & 0xf0) == 0xe0 && (str[1] & 0xc0) == 0x80 && (str[2] & 0xc0) == 0x80) {
    len = 3;
    return ((str[0] & 0x0f) << 12) | ((str[1] & 0x3f) << 6) | (str[2] & 0x3f);
  }
  if (len >= 4 && (str[0] & 0xf8) == 0xf0 && (str[1] & 0xc0) == 0x80 && (str[2] & 0xc0) == 0x80 &&
      (str[3] & 0xc0) == 0x80) {
    len = 4;
    return ((str[0] & 7) << 18) | ((str[1] & 0x3f) << 12) | ((str[2] & 0x3f) << 6) | (str[3] & 0x3f);
  }
  return -1;
}

void interpret_char(IntCtx& ctx) {
  auto s = ctx.parser->scan_word();
  int len = (s.size() < 10 ? (int)s.size() : 10);
  int code = str_utf8_code(s.data(), len);
  if (code < 0 || s.size() != (unsigned)len) {
    throw IntError{"exactly one character expected"};
  }
  ctx.stack.push_smallint(code);
  push_argcount(ctx, 1);
}

void interpret_char_internal(vm::Stack& stack) {
  auto s = stack.pop_string();
  int len = (s.size() < 10 ? (int)s.size() : 10);
  int code = str_utf8_code(s.c_str(), len);
  if (code < 0 || s.size() != (unsigned)len) {
    throw IntError{"exactly one character expected"};
  }
  stack.push_smallint(code);
}

int parse_number(std::string s, td::RefInt256& num, td::RefInt256& denom, bool allow_frac = true,
                 bool throw_error = false) {
  if (allow_frac) {
    auto pos = s.find('/');
    if (pos != std::string::npos) {
      return parse_number(std::string{s, 0, pos}, num, denom, false, throw_error) > 0 &&
                     parse_number(std::string{s, pos + 1}, denom, num, false, throw_error) > 0
                 ? 2
                 : 0;
    }
  }
  const char* str = s.c_str();
  int len = (int)s.size();
  int frac = -1, base, *frac_ptr = allow_frac ? &frac : nullptr;
  num = td::make_refint();
  auto& x = num.unique_write();
  if (len >= 4 && str[0] == '-' && str[1] == '0' && (str[2] == 'x' || str[2] == 'b')) {
    if (str[2] == 'x') {
      base = 16;
      if (x.parse_hex(str + 3, len - 3, frac_ptr) != len - 3) {
        return 0;
      }
    } else {
      base = 2;
      if (x.parse_binary(str + 3, len - 3, frac_ptr) != len - 3) {
        return 0;
      }
    }
    x.negate().normalize();
  } else if (len >= 3 && str[0] == '0' && (str[1] == 'x' || str[1] == 'b')) {
    if (str[1] == 'x') {
      base = 16;
      if (x.parse_hex(str + 2, len - 2, frac_ptr) != len - 2) {
        return 0;
      }
    } else {
      base = 2;
      if (x.parse_binary(str + 2, len - 2, frac_ptr) != len - 2) {
        return 0;
      }
    }
  } else {
    base = 10;
    if (!len || x.parse_dec(str, len, frac_ptr) != len) {
      return 0;
    }
  }
  if (!x.signed_fits_bits(257)) {
    if (throw_error) {
      throw IntError{"integer constant too large"};
    }
    return 0;
  }
  if (frac < 0) {
    return 1;
  } else {
    denom = td::make_refint(1);
    while (frac-- > 0) {
      if (!denom.unique_write().mul_tiny(base).normalize_bool()) {
        if (throw_error) {
          throw IntError{"denominator in constant too large"};
        }
        return 0;
      }
    }
    if (!denom.unique_write().unsigned_fits_bits(256)) {
      if (throw_error) {
        throw IntError{"denominator in constant too large"};
      }
      return 0;
    }
    return 2;
  }
}

void interpret_parse_number(vm::Stack& stack) {
  td::RefInt256 num, denom;
  int res = parse_number(stack.pop_string(), num, denom, true, false);
  if (res >= 1) {
    stack.push_int(std::move(num));
  }
  if (res == 2) {
    stack.push_int(std::move(denom));
  }
  stack.push_smallint(res);
}

void interpret_parse_hex_number(vm::Stack& stack) {
  td::RefInt256 x{true};
  auto str = stack.pop_string();
  bool ok = (str.size() <= 65535) && x.unique_write().parse_hex(str.data(), (int)str.size()) == (int)str.size();
  if (ok) {
    stack.push_int(std::move(x));
  }
  stack.push_smallint(ok);
}

void interpret_quit(IntCtx& ctx) {
  // TODO: change to correct behavior
  ctx.set_exit_code(0);
  ctx.next.clear();
}

void interpret_bye(IntCtx& ctx) {
  ctx.set_exit_code(-1);
  ctx.next.clear();
}

void interpret_halt(IntCtx& ctx) {
  ctx.set_exit_code(~ctx.stack.pop_smallint_range(255));
  ctx.next.clear();
}

void interpret_abort(IntCtx& ctx) {
  throw IntError{ctx.stack.pop_string()};
}

Ref<FiftCont> interpret_execute(IntCtx& ctx) {
  return pop_exec_token(ctx);
}

Ref<FiftCont> interpret_call_cc(IntCtx& ctx) {
  auto next = pop_exec_token(ctx);
  ctx.stack.push_object(std::move(ctx.next));
  return next;
}

Ref<FiftCont> interpret_execute_times(IntCtx& ctx) {
  int count = ctx.stack.pop_smallint_range(1000000000);
  auto body = pop_exec_token(ctx);
  if (count <= 0) {
    return {};
  }
  if (count == 1) {
    return body;
  }
  ctx.next = td::make_ref<TimesCont>(body, std::move(ctx.next), count - 1);
  return body;
}

Ref<FiftCont> interpret_if(IntCtx& ctx) {
  auto true_ref = pop_exec_token(ctx);
  if (ctx.stack.pop_bool()) {
    return true_ref;
  } else {
    return {};
  }
}

Ref<FiftCont> interpret_ifnot(IntCtx& ctx) {
  auto false_ref = pop_exec_token(ctx);
  if (ctx.stack.pop_bool()) {
    return {};
  } else {
    return false_ref;
  }
}

Ref<FiftCont> interpret_cond(IntCtx& ctx) {
  auto false_ref = pop_exec_token(ctx);
  auto true_ref = pop_exec_token(ctx);
  if (ctx.stack.pop_bool()) {
    return true_ref;
  } else {
    return false_ref;
  }
}

Ref<FiftCont> interpret_while(IntCtx& ctx) {
  auto body = pop_exec_token(ctx);
  auto cond = pop_exec_token(ctx);
  ctx.next = td::make_ref<WhileCont>(cond, std::move(body), std::move(ctx.next), true);
  return cond;
}

Ref<FiftCont> interpret_until(IntCtx& ctx) {
  auto body = pop_exec_token(ctx);
  ctx.next = td::make_ref<UntilCont>(body, std::move(ctx.next));
  return body;
}

DictEntry context_lookup(IntCtx& ctx, std::string word, bool append_space = true) {
  if (append_space) {
    auto entry = context_lookup(ctx, word, false);
    if (!entry) {
      entry = context_lookup(ctx, word + ' ', false);
    }
    return entry;
  }
  auto entry = ctx.context.lookup(word);
  if (!entry && ctx.context != ctx.dictionary) {
    entry = ctx.dictionary.lookup(word);
  }
  if (!entry && ctx.main_dictionary != ctx.context && ctx.main_dictionary != ctx.dictionary) {
    entry = ctx.main_dictionary.lookup(word);
  }
  return entry;
}

void interpret_tick(IntCtx& ctx) {
  std::string word = ctx.parser->scan_word().str();
  auto entry = context_lookup(ctx, word);
  if (!entry) {
    throw IntError{"word `" + word + "` undefined"};
  }
  ctx.stack.push_object(entry.get_def());
  push_argcount(ctx, 1);
}

void interpret_find(IntCtx& ctx, int mode) {
  std::string word = ctx.stack.pop_string();
  auto entry = context_lookup(ctx, word, !(mode & 2));
  if (!entry) {
    ctx.stack.push_bool(false);
  } else {
    ctx.stack.push_object(entry.get_def());
    if (!(mode & 1) || !entry.is_active()) {
      ctx.stack.push_bool(true);
    } else {
      ctx.stack.push_smallint(1);
    }
  }
}

void interpret_leave_source(IntCtx& ctx) {
  if (!ctx.leave_ctx()) {
    throw IntError{"cannot leave included file interpretation context"};
  }
}

void interpret_include_depth(IntCtx& ctx) {
  ctx.stack.push_smallint(ctx.include_depth());
}

Ref<FiftCont> interpret_include(IntCtx& ctx) {
  auto fname = ctx.stack.pop_string();
  auto r_file = ctx.source_lookup->lookup_source(fname, ctx.parser->currentd_dir);
  if (r_file.is_error()) {
    throw IntError{"cannot locate file `" + fname + "`"};
  }
  auto file = r_file.move_as_ok();
  auto ss = std::make_unique<std::stringstream>(std::move(file.data));
  if (!ctx.enter_ctx(td::PathView(file.path).file_name().str(), td::PathView(file.path).parent_dir().str(),
                     std::move(ss))) {
    throw IntError{"cannot enter included file interpretation context"};
  }
  ctx.next = SeqCont::seq(td::make_ref<CtxWord>(interpret_leave_source), std::move(ctx.next));
  return td::make_ref<InterpretCont>();
}

td::Ref<vm::Box> exit_interpret{true};

Ref<FiftCont> interpret_skip_source(IntCtx& ctx) {
  auto cont = exit_interpret->get().as_object<FiftCont>();
  ctx.next.clear();
  /*
  if (cont.is_null()) {
    throw IntError{"no interpreter exit point set"};
  }
  */
  return cont;
}

void interpret_words(IntCtx& ctx) {
  for (const auto& x : ctx.dictionary) {
    *ctx.output_stream << vm::StackEntry(x.key()).as_string() << " ";
  }
  *ctx.output_stream << std::endl;
}

void interpret_get_current(IntCtx& ctx) {
  ctx.stack.push(ctx.dictionary.get_box());
}

void interpret_set_current(IntCtx& ctx) {
  ctx.dictionary = ctx.stack.pop_box();
}

void interpret_get_context(IntCtx& ctx) {
  ctx.stack.push(ctx.context.get_box());
}

void interpret_set_context(IntCtx& ctx) {
  ctx.context = ctx.stack.pop_box();
}

void interpret_set_context_to(IntCtx& ctx, Ref<vm::Box> box) {
  ctx.context = std::move(box);
}

void interpret_print_backtrace(IntCtx& ctx) {
  ctx.print_backtrace(*ctx.output_stream, ctx.next);
}

void interpret_print_continuation(IntCtx& ctx) {
  ctx.print_backtrace(*ctx.output_stream, pop_exec_token(ctx));
}

void interpret_pack_std_smc_addr(vm::Stack& stack) {
  block::StdAddress a;
  stack.check_underflow(3);
  int mode = stack.pop_smallint_range(7);
  td::RefInt256 x = stack.pop_int_finite();
  if (td::sgn(x) < 0) {
    throw IntError{"non-negative integer expected"};
  }
  CHECK(x->export_bytes(a.addr.data(), 32, false));
  a.workchain = stack.pop_smallint_range(0x7f, -0x80);
  a.testnet = mode & 2;
  a.bounceable = !(mode & 1);
  stack.push_string(a.rserialize(mode & 4));
}

void interpret_unpack_std_smc_addr(vm::Stack& stack) {
  block::StdAddress a;
  if (!a.parse_addr(stack.pop_string())) {
    stack.push_bool(false);
  } else {
    stack.push_smallint(a.workchain);
    td::RefInt256 x{true};
    CHECK(x.write().import_bytes(a.addr.data(), 32, false));
    stack.push_int(std::move(x));
    stack.push_smallint(a.testnet * 2 + 1 - a.bounceable);
    stack.push_bool(true);
  }
}

void interpret_bytes_to_base64(vm::Stack& stack, bool base64_url) {
  stack.push_string(td::str_base64_encode(stack.pop_bytes(), base64_url));
}

void interpret_base64_to_bytes(vm::Stack& stack, bool allow_base64_url, bool quiet) {
  auto s = stack.pop_string();
  if (!td::is_valid_base64(s, allow_base64_url)) {
    stack.push_bool(false);
    if (!quiet) {
      throw IntError{"invalid base64"};
    }
  } else {
    stack.push_bytes(td::str_base64_decode(s, allow_base64_url));
    if (quiet) {
      stack.push_bool(true);
    }
  }
}

vm::VmLog create_vm_log(td::LogInterface* logger) {
  if (!logger) {
    return {};
  }
  auto options = td::LogOptions::plain();
  options.level = 4;
  options.fix_newlines = true;
  return {logger, options};
}

class StringLogger : public td::LogInterface {
 public:
  void append(td::CSlice slice) override {
    res.append(slice.data(), slice.size());
  }
  std::string res;
};

class OstreamLogger : public td::LogInterface {
 public:
  explicit OstreamLogger(std::ostream* stream) : stream_(stream) {
  }
  void append(td::CSlice slice) override {
    stream_->write(slice.data(), slice.size());
  }

 private:
  std::ostream* stream_{nullptr};
};

td::Ref<vm::Box> vm_libraries{true};

std::vector<Ref<vm::Cell>> get_vm_libraries() {
  if (vm_libraries->get().type() == vm::StackEntry::t_cell) {
    return {vm_libraries->get().as_cell()};
  } else {
    return {};
  }
}

// mode: -1 = pop from stack
// +1 = same_c3 (set c3 to code)
// +2 = push_0 (push an implicit 0 before running the code)
// +4 = load c4 (persistent data) from stack and return its final value
// +8 = load gas limit from stack and return consumed gas
// +16 = load c7 (smart-contract context)
// +32 = return c5 (actions)
// +64 = log vm ops to stderr
// +128 = pop hard gas limit (enabled by ACCEPT) from stack as well
// +256 = enable stack trace
// +512 = enable debug instructions
// +1024 = load global_version from stack
void interpret_run_vm(IntCtx& ctx, int mode) {
  if (mode < 0) {
    mode = ctx.stack.pop_smallint_range(0x7ff);
  }
  bool with_data = mode & 4;
  Ref<vm::Tuple> c7;
  Ref<vm::Cell> data, actions;
  int global_version = (mode & 1024) ? ctx.stack.pop_smallint_range(ton::SUPPORTED_VERSION) : ton::SUPPORTED_VERSION;
  long long gas_max = (mode & 128) ? ctx.stack.pop_long_range(vm::GasLimits::infty) : vm::GasLimits::infty;
  long long gas_limit = (mode & 8) ? ctx.stack.pop_long_range(vm::GasLimits::infty) : vm::GasLimits::infty;
  if (!(mode & 128)) {
    gas_max = gas_limit;
  } else {
    gas_max = std::max(gas_max, gas_limit);
  }
  if (mode & 16) {
    c7 = ctx.stack.pop_tuple();
  }
  if (with_data) {
    data = ctx.stack.pop_cell();
  }
  auto cs = ctx.stack.pop_cellslice();
  OstreamLogger ostream_logger(ctx.error_stream);
  auto log = create_vm_log((mode & 64) && ctx.error_stream ? &ostream_logger : nullptr);
  vm::GasLimits gas{gas_limit, gas_max};
  int res = vm::run_vm_code(cs, ctx.stack, (mode & 3) | ((mode & 0x300) >> 6), &data, log, nullptr, &gas,
                            get_vm_libraries(), std::move(c7), &actions, global_version);
  ctx.stack.push_smallint(res);
  if (with_data) {
    ctx.stack.push_cell(std::move(data));
  }
  if (mode & 32) {
    ctx.stack.push_cell(std::move(actions));
  }
  if (mode & 8) {
    ctx.stack.push_smallint(gas.gas_consumed());
  }
}

void interpret_vmop_len(vm::Stack& stack) {
  int cp = stack.pop_smallint_range(0x7fffffff, -0x80000000);
  auto cs = stack.pop_cellslice();
  auto dispatch = vm::DispatchTable::get_table(cp);
  if (!dispatch) {
    throw IntError{"unknown vm codepage"};
  }
  stack.push_smallint(dispatch->instr_len(*cs));
}

void interpret_vmop_dump(vm::Stack& stack) {
  int cp = stack.pop_smallint_range(0x7fffffff, -0x80000000);
  auto cs = stack.pop_cellslice();
  auto dispatch = vm::DispatchTable::get_table(cp);
  if (!dispatch) {
    throw IntError{"unknown vm codepage"};
  }
  auto dump = dispatch->dump_instr(cs.write());
  stack.push_cellslice(std::move(cs));
  stack.push_string(std::move(dump));
}

void interpret_supported_version(vm::Stack& stack) {
  stack.push_smallint(ton::SUPPORTED_VERSION);
}

void interpret_store_vm_cont(vm::Stack& stack) {
  auto vmcont = stack.pop_cont();
  auto cb = stack.pop_builder();
  if (!vmcont->serialize(cb.write())) {
    throw IntError{"cannot serialize vm continuation"};
  }
  stack.push_builder(std::move(cb));
}

void interpret_fetch_vm_cont(vm::Stack& stack) {
  auto cs = stack.pop_cellslice();
  auto vmcont = vm::Continuation::deserialize(cs.write());
  if (vmcont.is_null()) {
    throw IntError{"cannot deserialize vm continuation"};
  }
  stack.push_cellslice(std::move(cs));
  stack.push_cont(std::move(vmcont));
}

Ref<vm::Box> cmdline_args{true};

void interpret_get_fixed_cmdline_arg(vm::Stack& stack, int n) {
  if (!n) {
    return;
  }
  auto v = cmdline_args->get();
  while (true) {
    if (v.empty()) {
      stack.push(vm::StackEntry{});
      return;
    }
    auto t = v.as_tuple_range(2, 2);
    if (t.is_null()) {
      throw IntError{"invalid cmdline arg list"};
    }
    if (!--n) {
      stack.push(t->at(0));
      return;
    }
    v = t->at(1);
  }
}

// n -- executes $n
Ref<FiftCont> interpret_get_cmdline_arg(IntCtx& ctx) {
  int n = ctx.stack.pop_smallint_range(999999);
  if (n) {
    interpret_get_fixed_cmdline_arg(ctx.stack, n);
    return {};
  }
  auto entry = ctx.dictionary.lookup("$0 ");
  if (!entry) {
    throw IntError{"-?"};
  } else {
    return entry.get_def();
  }
}

void interpret_get_cmdline_arg_count(vm::Stack& stack) {
  auto v = cmdline_args->get();
  int cnt;
  for (cnt = 0; !v.empty(); cnt++) {
    auto t = v.as_tuple_range(2, 2);
    if (t.is_null()) {
      throw IntError{"invalid cmdline arg list"};
    }
    v = t->at(1);
  }
  stack.push_smallint(cnt);
}

void interpret_getenv(vm::Stack& stack) {
  auto str = stack.pop_string();
  auto value = str.size() < 1024 ? getenv(str.c_str()) : nullptr;
  stack.push_string(value ? std::string{value} : "");
}

void interpret_getenv_exists(vm::Stack& stack) {
  auto str = stack.pop_string();
  auto value = str.size() < 1024 ? getenv(str.c_str()) : nullptr;
  if (value) {
    stack.push_string(std::string{value});
  }
  stack.push_bool((bool)value);
}

// x1 .. xn n 'w -->
Ref<FiftCont> interpret_execute_internal(IntCtx& ctx) {
  Ref<FiftCont> word_def = pop_exec_token(ctx);
  int count = ctx.stack.pop_smallint_range(255);
  ctx.stack.check_underflow(count);
  return word_def;
}

void do_compile(vm::Stack& stack, Ref<FiftCont> word_def) {
  Ref<WordList> wl_ref = pop_word_list(stack);
  if (word_def != nop_word_def) {
    auto list_size = word_def->list_size();
    if (list_size >= 0 && (list_size <= 2 || word_def.is_unique())) {
      // inline short and unique definitions
      auto list = word_def->get_list();
      wl_ref.write().append(list, list + list_size);
    } else {
      wl_ref.write().push_back(std::move(word_def));
    }
  }
  stack.push_object(std::move(wl_ref));
}

void compile_one_literal(WordList& wlist, vm::StackEntry val) {
  wlist.push_back(LitCont::literal(std::move(val)));
}

void do_compile_literals(vm::Stack& stack, int count) {
  if (count < 0) {
    throw IntError{"cannot compile a negative number of literals"};
  }
  stack.check_underflow(count + 1);
  Ref<WordList> wl_ref = std::move(stack[count]).as_object<WordList>();
  if (wl_ref.is_null()) {
    throw IntError{"list of words expected"};
  }
  if (count >= 2) {
    std::vector<vm::StackEntry> literals;
    for (int i = count - 1; i >= 0; i--) {
      literals.push_back(std::move(stack[i]));
    }
    wl_ref.write().push_back(td::make_ref<MultiLitCont>(std::move(literals)));
  } else {
    for (int i = count - 1; i >= 0; i--) {
      compile_one_literal(wl_ref.write(), std::move(stack[i]));
    }
  }
  stack.pop_many(count + 1);
  stack.push_object(std::move(wl_ref));
}

// wl x1 .. xn n 'w --> wl'
void interpret_compile_internal(vm::Stack& stack) {
  Ref<FiftCont> word_def = pop_exec_token(stack);
  int count = stack.pop_smallint_range(255);
  do_compile_literals(stack, count);
  if (word_def != nop_word_def) {
    do_compile(stack, std::move(word_def));
  }
}

Ref<FiftCont> interpret_compile_execute(IntCtx& ctx) {
  if (ctx.state > 0) {
    interpret_compile_internal(ctx.stack);
    return {};
  } else {
    return interpret_execute_internal(ctx);
  }
}

void interpret_seekeof(IntCtx& ctx, int mode) {
  if (mode == -1) {
    mode = ctx.stack.pop_smallint_range(3, -1);
  }
  bool eof = true;
  if (ctx.parser && (ctx.parser->get_input() || ctx.parser->load_next_line())) {
    while (true) {
      if (!ctx.parser->is_sb()) {
        ctx.parser->skipspc();
        if (*ctx.parser->get_input()) {
          eof = false;
          break;
        }
      }
      if (mode & 1) {
        *ctx.output_stream << " ok" << std::endl;
      }
      if (!ctx.parser->load_next_line()) {
        break;
      }
    }
  }
  ctx.stack.push_bool(eof);
}

void interpret_word_prefix_find(IntCtx& ctx, int mode) {
  const char *ptr = ctx.parser->get_input(), *start = ptr;
  if (!ptr) {
    ctx.stack.push_string(std::string{});
    ctx.stack.push_bool(false);
    return;
  }
  while (*ptr && *ptr != ' ' && *ptr != '\t') {
    ptr++;
  }
  std::string Word{start, ptr};
  Word.push_back(' ');
  auto entry = context_lookup(ctx, Word, false);
  Word.pop_back();
  if (entry) {
    ctx.parser->set_input(ptr);
    ctx.parser->skipspc();
  } else {
    const char* ptr_end = ptr;
    while (true) {
      entry = context_lookup(ctx, Word, false);
      if (entry) {
        ctx.parser->set_input(ptr);
        break;
      }
      if (ptr == start) {
        Word = std::string{start, ptr_end};
        ctx.parser->set_input(ptr_end);
        ctx.parser->skipspc();
        break;
      }
      Word.pop_back();
      --ptr;
    }
  }
  ctx.parser->word = Word;
  if (!(mode & 2) || !entry) {
    ctx.stack.push_string(std::move(Word));
  }
  if (mode & 1) {
    if (!entry) {
      ctx.stack.push_bool(false);
    } else {
      ctx.stack.push_object(entry.get_def());
      ctx.stack.push_smallint(entry.is_active() ? 1 : -1);
    }
  }
}

// equivalent to
// { ?dup { 1+ { execute } { 0 swap } cond } { (number) ?dup 0= abort"-?" 'nop } cond
// } : (interpret-prepare)
Ref<FiftCont> interpret_prepare(IntCtx& ctx) {
  int found = ctx.stack.pop_smallint_range(1, -1);
  if (!found) {
    // numbers
    interpret_parse_number(ctx);     // (number)
    interpret_cond_dup(ctx);         // ?dup
    auto res = ctx.stack.pop_int();  // 0= abort"-?"
    if (res == 0) {
      throw IntError{"-?"};
    }
    ctx.stack.push_object(nop_word_def);  // 'nop
    return {};
  } else if (found == -1) {
    // ordinary word
    ctx.stack.push_smallint(0);  // 0
    interpret_swap(ctx);         // swap
    return {};
  } else {
    // active word
    return pop_exec_token(ctx);  // execute
  }
}

Ref<FiftCont> InterpretCont::run_tail(IntCtx& ctx) const {
  static Ref<FiftCont> interpret_prepare_ref = td::make_ref<CtxTailWord>(interpret_prepare);
  static Ref<FiftCont> compile_exec_ref = td::make_ref<CtxTailWord>(interpret_compile_execute);
  interpret_seekeof(ctx, !ctx.state && !ctx.include_depth());  // seekeof
  if (ctx.stack.pop_bool()) {
    exit_interpret->clear();
    return {};  // exit loop
  }
  exit_interpret->set({vm::from_object, ctx.next});  // set 'exit-interpret to current continuation
  interpret_word_prefix_find(ctx, 3);                // (word-prefix-find)
  // (interpet-prepare) (compile-execute) and schedule next loop iteration
  ctx.next = SeqCont::seq(compile_exec_ref, SeqCont::seq(self(), std::move(ctx.next)));
  return interpret_prepare_ref;  // (interpret-prepare)
}

void init_words_common(Dictionary& d) {
  using namespace std::placeholders;
  d.def_word("nop ", nop_word_def);
  // stack print/dump words
  d.def_ctx_word(". ", std::bind(interpret_dot, _1, true));
  d.def_ctx_word("._ ", std::bind(interpret_dot, _1, false));
  d.def_ctx_word("x. ", std::bind(interpret_dothex, _1, false, true));
  d.def_ctx_word("x._ ", std::bind(interpret_dothex, _1, false, false));
  d.def_ctx_word("X. ", std::bind(interpret_dothex, _1, true, true));
  d.def_ctx_word("X._ ", std::bind(interpret_dothex, _1, true, false));
  d.def_ctx_word("b. ", std::bind(interpret_dotbinary, _1, true));
  d.def_ctx_word("b._ ", std::bind(interpret_dotbinary, _1, false));
  d.def_ctx_word("csr. ", interpret_dot_cellslice_rec);
  d.def_ctx_word(".s ", interpret_dotstack);
  d.def_ctx_word(".sl ", interpret_dotstack_list);
  d.def_ctx_word(".sL ", interpret_dotstack_list_dump);  // TMP
  d.def_ctx_word(".dump ", interpret_dump);
  d.def_ctx_word(".l ", interpret_print_list);
  d.def_ctx_word(".tc ", interpret_dottc);
  d.def_stack_word("(dump) ", interpret_dump_internal);
  d.def_stack_word("(ldump) ", interpret_list_dump_internal);
  d.def_stack_word("(.) ", interpret_dot_internal);
  d.def_stack_word("(x.) ", std::bind(interpret_dothex_internal, _1, false));
  d.def_stack_word("(X.) ", std::bind(interpret_dothex_internal, _1, true));
  d.def_stack_word("(b.) ", interpret_dotbinary_internal);
  // stack manipulation
  d.def_stack_word("drop ", interpret_drop);
  d.def_stack_word("2drop ", interpret_2drop);
  d.def_stack_word("dup ", interpret_dup);
  d.def_stack_word("over ", interpret_over);
  d.def_stack_word("2dup ", interpret_2dup);
  d.def_stack_word("2over ", interpret_2over);
  d.def_stack_word("swap ", interpret_swap);
  d.def_stack_word("2swap ", interpret_2swap);
  d.def_stack_word("tuck ", interpret_tuck);
  d.def_stack_word("nip ", interpret_nip);
  d.def_stack_word("rot ", interpret_rot);
  d.def_stack_word("-rot ", interpret_rot_rev);
  d.def_stack_word("pick ", interpret_pick);
  d.def_stack_word("roll ", interpret_roll);
  d.def_stack_word("-roll ", interpret_roll_rev);
  d.def_stack_word("reverse ", interpret_reverse);
  d.def_stack_word("exch ", interpret_exch);
  d.def_stack_word("exch2 ", interpret_exch2);
  d.def_stack_word("depth ", interpret_depth);
  d.def_stack_word("?dup ", interpret_cond_dup);
  // low-level stack manipulation
  d.def_stack_word("<xchg> ", interpret_make_xchg);
  d.def_stack_word("<push> ", interpret_make_push);
  d.def_stack_word("<pop> ", interpret_make_pop);
  // arithmetic
  d.def_stack_word("+ ", interpret_plus);
  d.def_stack_word("- ", interpret_minus);
  d.def_stack_word("negate ", interpret_negate);
  d.def_stack_word("1+ ", std::bind(interpret_plus_tiny, _1, 1));
  d.def_stack_word("1- ", std::bind(interpret_plus_tiny, _1, -1));
  d.def_stack_word("2+ ", std::bind(interpret_plus_tiny, _1, 2));
  d.def_stack_word("2- ", std::bind(interpret_plus_tiny, _1, -2));
  d.def_stack_word("* ", interpret_times);
  d.def_stack_word("/ ", std::bind(interpret_div, _1, -1));
  d.def_stack_word("/c ", std::bind(interpret_div, _1, 1));
  d.def_stack_word("/r ", std::bind(interpret_div, _1, 0));
  d.def_stack_word("mod ", std::bind(interpret_mod, _1, -1));
  d.def_stack_word("rmod ", std::bind(interpret_mod, _1, 0));
  d.def_stack_word("cmod ", std::bind(interpret_mod, _1, 1));
  d.def_stack_word("/mod ", std::bind(interpret_divmod, _1, -1));
  d.def_stack_word("/cmod ", std::bind(interpret_divmod, _1, 1));
  d.def_stack_word("/rmod ", std::bind(interpret_divmod, _1, 0));
  d.def_stack_word("*/ ", std::bind(interpret_times_div, _1, -1));
  d.def_stack_word("*/c ", std::bind(interpret_times_div, _1, 1));
  d.def_stack_word("*/r ", std::bind(interpret_times_div, _1, 0));
  d.def_stack_word("*/mod ", std::bind(interpret_times_divmod, _1, -1));
  d.def_stack_word("*/cmod ", std::bind(interpret_times_divmod, _1, 1));
  d.def_stack_word("*/rmod ", std::bind(interpret_times_divmod, _1, 0));
  d.def_stack_word("*mod ", std::bind(interpret_times_mod, _1, -1));
  d.def_stack_word("1<< ", interpret_pow2);
  d.def_stack_word("-1<< ", interpret_neg_pow2);
  d.def_stack_word("1<<1- ", interpret_pow2_minus1);
  d.def_stack_word("%1<< ", interpret_mod_pow2);
  d.def_stack_word("<< ", interpret_lshift);
  d.def_stack_word(">> ", std::bind(interpret_rshift, _1, -1));
  d.def_stack_word(">>c ", std::bind(interpret_rshift, _1, 1));
  d.def_stack_word(">>r ", std::bind(interpret_rshift, _1, 0));
  d.def_stack_word("2* ", std::bind(interpret_lshift_const, _1, 1));
  d.def_stack_word("2/ ", std::bind(interpret_rshift_const, _1, 1));
  d.def_stack_word("*>> ", std::bind(interpret_times_rshift, _1, -1));
  d.def_stack_word("*>>c ", std::bind(interpret_times_rshift, _1, 1));
  d.def_stack_word("*>>r ", std::bind(interpret_times_rshift, _1, 0));
  d.def_stack_word("<</ ", std::bind(interpret_lshift_div, _1, -1));
  d.def_stack_word("<</c ", std::bind(interpret_lshift_div, _1, 1));
  d.def_stack_word("<</r ", std::bind(interpret_lshift_div, _1, 0));
  d.def_stack_word("integer? ", std::bind(interpret_has_type, _1, vm::StackEntry::t_int));
  d.def_stack_word("box? ", std::bind(interpret_has_type, _1, vm::StackEntry::t_box));
  // logical
  d.def_stack_word("not ", interpret_not);
  d.def_stack_word("and ", interpret_and);
  d.def_stack_word("or ", interpret_or);
  d.def_stack_word("xor ", interpret_xor);
  // integer constants
  d.def_word("false ", IntLitCont::literal(0));
  d.def_word("true ", IntLitCont::literal(-1));
  d.def_word("0 ", IntLitCont::literal(0));
  d.def_word("1 ", IntLitCont::literal(1));
  d.def_word("2 ", IntLitCont::literal(2));
  d.def_word("-1 ", IntLitCont::literal(-1));
  d.def_word("bl ", IntLitCont::literal(32));
  // integer comparison
  d.def_stack_word("cmp ", std::bind(interpret_cmp, _1, "\xff\x00\x01"));
  d.def_stack_word("= ", std::bind(interpret_cmp, _1, "\x00\xff\x00"));
  d.def_stack_word("<> ", std::bind(interpret_cmp, _1, "\xff\x00\xff"));
  d.def_stack_word("<= ", std::bind(interpret_cmp, _1, "\xff\xff\x00"));
  d.def_stack_word(">= ", std::bind(interpret_cmp, _1, "\x00\xff\xff"));
  d.def_stack_word("< ", std::bind(interpret_cmp, _1, "\xff\x00\x00"));
  d.def_stack_word("> ", std::bind(interpret_cmp, _1, "\x00\x00\xff"));
  d.def_stack_word("sgn ", std::bind(interpret_sgn, _1, "\xff\x00\x01"));
  d.def_stack_word("0= ", std::bind(interpret_sgn, _1, "\x00\xff\x00"));
  d.def_stack_word("0<> ", std::bind(interpret_sgn, _1, "\xff\x00\xff"));
  d.def_stack_word("0<= ", std::bind(interpret_sgn, _1, "\xff\xff\x00"));
  d.def_stack_word("0>= ", std::bind(interpret_sgn, _1, "\x00\xff\xff"));
  d.def_stack_word("0< ", std::bind(interpret_sgn, _1, "\xff\x00\x00"));
  d.def_stack_word("0> ", std::bind(interpret_sgn, _1, "\x00\x00\xff"));
  d.def_stack_word("fits ", std::bind(interpret_fits, _1, true));
  d.def_stack_word("ufits ", std::bind(interpret_fits, _1, false));
  // char/string manipulation
  d.def_active_word("\"", interpret_quote_str);
  d.def_active_word("char ", interpret_char);
  d.def_stack_word("(char) ", interpret_char_internal);
  d.def_ctx_word("emit ", interpret_emit);
  d.def_ctx_word("space ", std::bind(interpret_emit_const, _1, ' '));
  d.def_ctx_word("cr ", std::bind(interpret_emit_const, _1, '\n'));
  d.def_ctx_word("type ", interpret_type);
  d.def_stack_word("string? ", interpret_is_string);
  d.def_stack_word("chr ", interpret_chr);
  d.def_stack_word("hold ", interpret_hold);
  d.def_stack_word("(number) ", interpret_parse_number);
  d.def_stack_word("(hex-number) ", interpret_parse_hex_number);
  d.def_stack_word("$| ", interpret_str_split);
  d.def_stack_word("$+ ", interpret_str_concat);
  d.def_stack_word("$= ", interpret_str_equal);
  d.def_stack_word("$cmp ", interpret_str_cmp);
  d.def_stack_word("$reverse ", interpret_str_reverse);
  d.def_stack_word("$pos ", interpret_str_pos);
  d.def_stack_word("(-trailing) ", std::bind(interpret_str_remove_trailing_int, _1, 0));
  d.def_stack_word("-trailing ", std::bind(interpret_str_remove_trailing_int, _1, ' '));
  d.def_stack_word("-trailing0 ", std::bind(interpret_str_remove_trailing_int, _1, '0'));
  d.def_stack_word("$len ", interpret_str_len);
  d.def_stack_word("Blen ", interpret_bytes_len);
  d.def_stack_word("$Len ", interpret_utf8_str_len);
  d.def_stack_word("$Split ", interpret_utf8_str_split);
  d.def_stack_word("$Pos ", interpret_utf8_str_pos);
  d.def_ctx_word("Bx. ", std::bind(interpret_bytes_hex_print_raw, _1, true));
  d.def_stack_word("B>X ", std::bind(interpret_bytes_to_hex, _1, true));
  d.def_stack_word("B>x ", std::bind(interpret_bytes_to_hex, _1, false));
  d.def_stack_word("x>B ", std::bind(interpret_hex_to_bytes, _1, false));
  d.def_stack_word("x>B? ", std::bind(interpret_hex_to_bytes, _1, true));
  d.def_stack_word("B| ", interpret_bytes_split);
  d.def_stack_word("B+ ", interpret_bytes_concat);
  d.def_stack_word("B= ", interpret_bytes_equal);
  d.def_stack_word("Bcmp ", interpret_bytes_cmp);
  d.def_stack_word("u>B ", std::bind(interpret_int_to_bytes, _1, false, false));
  d.def_stack_word("i>B ", std::bind(interpret_int_to_bytes, _1, true, false));
  d.def_stack_word("Lu>B ", std::bind(interpret_int_to_bytes, _1, false, true));
  d.def_stack_word("Li>B ", std::bind(interpret_int_to_bytes, _1, true, true));
  d.def_stack_word("B>u@ ", std::bind(interpret_bytes_fetch_int, _1, 0));
  d.def_stack_word("B>i@ ", std::bind(interpret_bytes_fetch_int, _1, 1));
  d.def_stack_word("B>u@+ ", std::bind(interpret_bytes_fetch_int, _1, 2));
  d.def_stack_word("B>i@+ ", std::bind(interpret_bytes_fetch_int, _1, 3));
  d.def_stack_word("B>Lu@ ", std::bind(interpret_bytes_fetch_int, _1, 0x10));
  d.def_stack_word("B>Li@ ", std::bind(interpret_bytes_fetch_int, _1, 0x11));
  d.def_stack_word("B>Lu@+ ", std::bind(interpret_bytes_fetch_int, _1, 0x12));
  d.def_stack_word("B>Li@+ ", std::bind(interpret_bytes_fetch_int, _1, 0x13));
  d.def_stack_word("$>B ", interpret_string_to_bytes);
  d.def_stack_word("B>$ ", interpret_bytes_to_string);
  d.def_stack_word("Bhash ", std::bind(interpret_bytes_hash, _1, true));
  d.def_stack_word("Bhashu ", std::bind(interpret_bytes_hash, _1, true));
  d.def_stack_word("BhashB ", std::bind(interpret_bytes_hash, _1, false));
  // cell manipulation (create, write and modify cells)
  d.def_stack_word("<b ", interpret_empty);
  d.def_stack_word("i, ", std::bind(interpret_store, _1, true));
  d.def_stack_word("u, ", std::bind(interpret_store, _1, false));
  d.def_stack_word("ref, ", interpret_store_ref);
  d.def_stack_word("$, ", interpret_store_str);
  d.def_stack_word("B, ", interpret_store_bytes);
  d.def_stack_word("s, ", interpret_store_cellslice);
  d.def_stack_word("sr, ", interpret_store_cellslice_ref);
  d.def_stack_word("b> ", std::bind(interpret_store_end, _1, false));
  d.def_stack_word("b>spec ", std::bind(interpret_store_end, _1, true));
  d.def_stack_word("$>s ", interpret_string_to_cellslice);
  d.def_stack_word("|+ ", interpret_concat_cellslice);
  d.def_stack_word("|_ ", interpret_concat_cellslice_ref);
  d.def_stack_word("b+ ", interpret_concat_builders);
  d.def_stack_word("bbits ", std::bind(interpret_builder_bitrefs, _1, 1));
  d.def_stack_word("brefs ", std::bind(interpret_builder_bitrefs, _1, 2));
  d.def_stack_word("bbitrefs ", std::bind(interpret_builder_bitrefs, _1, 3));
  d.def_stack_word("brembits ", std::bind(interpret_builder_remaining_bitrefs, _1, 1));
  d.def_stack_word("bremrefs ", std::bind(interpret_builder_remaining_bitrefs, _1, 2));
  d.def_stack_word("brembitrefs ", std::bind(interpret_builder_remaining_bitrefs, _1, 3));
  d.def_stack_word("hash ", std::bind(interpret_cell_hash, _1, true));
  d.def_stack_word("hashu ", std::bind(interpret_cell_hash, _1, true));
  d.def_stack_word("hashB ", std::bind(interpret_cell_hash, _1, false));
  // cellslice manipulation (read from cells)
  d.def_stack_word("<s ", interpret_from_cell);
  d.def_stack_word("i@ ", std::bind(interpret_fetch, _1, 1));
  d.def_stack_word("u@ ", std::bind(interpret_fetch, _1, 0));
  d.def_stack_word("i@+ ", std::bind(interpret_fetch, _1, 3));
  d.def_stack_word("u@+ ", std::bind(interpret_fetch, _1, 2));
  d.def_stack_word("i@? ", std::bind(interpret_fetch, _1, 5));
  d.def_stack_word("u@? ", std::bind(interpret_fetch, _1, 4));
  d.def_stack_word("i@?+ ", std::bind(interpret_fetch, _1, 7));
  d.def_stack_word("u@?+ ", std::bind(interpret_fetch, _1, 6));
  d.def_stack_word("$@ ", std::bind(interpret_fetch_bytes, _1, 0));
  d.def_stack_word("B@ ", std::bind(interpret_fetch_bytes, _1, 1));
  d.def_stack_word("$@+ ", std::bind(interpret_fetch_bytes, _1, 2));
  d.def_stack_word("B@+ ", std::bind(interpret_fetch_bytes, _1, 3));
  d.def_stack_word("$@? ", std::bind(interpret_fetch_bytes, _1, 4));
  d.def_stack_word("B@? ", std::bind(interpret_fetch_bytes, _1, 5));
  d.def_stack_word("$@?+ ", std::bind(interpret_fetch_bytes, _1, 6));
  d.def_stack_word("B@?+ ", std::bind(interpret_fetch_bytes, _1, 7));
  d.def_stack_word("ref@ ", std::bind(interpret_fetch_ref, _1, 0));
  d.def_stack_word("ref@+ ", std::bind(interpret_fetch_ref, _1, 2));
  d.def_stack_word("ref@? ", std::bind(interpret_fetch_ref, _1, 4));
  d.def_stack_word("ref@?+ ", std::bind(interpret_fetch_ref, _1, 6));
  d.def_stack_word("s@ ", std::bind(interpret_fetch_slice, _1, 0));
  d.def_stack_word("sr@ ", std::bind(interpret_fetch_slice, _1, 1));
  d.def_stack_word("s@+ ", std::bind(interpret_fetch_slice, _1, 2));
  d.def_stack_word("sr@+ ", std::bind(interpret_fetch_slice, _1, 3));
  d.def_stack_word("s@? ", std::bind(interpret_fetch_slice, _1, 4));
  d.def_stack_word("sr@? ", std::bind(interpret_fetch_slice, _1, 5));
  d.def_stack_word("s@?+ ", std::bind(interpret_fetch_slice, _1, 6));
  d.def_stack_word("sr@?+ ", std::bind(interpret_fetch_slice, _1, 7));
  d.def_stack_word("s> ", interpret_cell_check_empty);
  d.def_stack_word("empty? ", interpret_cell_empty);
  d.def_stack_word("remaining ", interpret_cell_remaining);
  d.def_stack_word("sbits ", std::bind(interpret_slice_bitrefs, _1, 1));
  d.def_stack_word("srefs ", std::bind(interpret_slice_bitrefs, _1, 2));
  d.def_stack_word("sbitrefs ", std::bind(interpret_slice_bitrefs, _1, 3));
  d.def_stack_word("totalcsize ", std::bind(interpret_cell_datasize, _1, 0));
  d.def_stack_word("totalssize ", std::bind(interpret_cell_datasize, _1, 2));
  // boc manipulation
  d.def_stack_word("B>boc ", interpret_boc_deserialize);
  d.def_stack_word("boc>B ", interpret_boc_serialize);
  d.def_stack_word("boc+>B ", interpret_boc_serialize_ext);
  d.def_ctx_word("file>B ", interpret_read_file);
  d.def_ctx_word("filepart>B ", interpret_read_file_part);
  d.def_ctx_word("B>file ", interpret_write_file);
  d.def_ctx_word("file-exists? ", interpret_file_exists);
  // custom & crypto
  d.def_ctx_word("now ", interpret_now);
  d.def_stack_word("getenv ", interpret_getenv);
  d.def_stack_word("getenv? ", interpret_getenv_exists);
  d.def_stack_word("newkeypair ", interpret_new_keypair);
  d.def_stack_word("priv>pub ", interpret_priv_key_to_pub);
  d.def_stack_word("ed25519_sign ", interpret_ed25519_sign);
  d.def_stack_word("ed25519_chksign ", interpret_ed25519_chksign);
  d.def_stack_word("ed25519_sign_uint ", interpret_ed25519_sign_uint);
  d.def_stack_word("crc16 ", interpret_crc16);
  d.def_stack_word("crc32 ", interpret_crc32);
  d.def_stack_word("crc32c ", interpret_crc32c);
  // hashmaps
  d.def_stack_word("hmapnew ", interpret_hmap_new);
  d.def_stack_word("hmap@ ", std::bind(interpret_hmap_fetch, _1, 6));
  d.def_stack_word("hmap@? ", std::bind(interpret_hmap_fetch, _1, 5));
  d.def_stack_word("hmap- ", std::bind(interpret_hmap_delete, _1, 0));
  d.def_stack_word("hmap-? ", std::bind(interpret_hmap_delete, _1, 1));
  d.def_stack_word("hmap@- ", std::bind(interpret_hmap_delete, _1, 6));
  d.def_stack_word("hmap! ", std::bind(interpret_hmap_store, _1, 0));
  d.def_stack_word("hmap!+ ", std::bind(interpret_hmap_store, _1, 1));
  d.def_stack_word("hmapempty? ", interpret_hmap_is_empty);
  d.def_stack_word("hmapunpack ", std::bind(interpret_hmap_decompose, _1, 1));
  d.def_ctx_tail_word("hmapforeach ", std::bind(interpret_hmap_foreach, _1, 0));
  // vm dictionaries
  d.def_stack_word("dictnew ", interpret_dict_new);
  d.def_stack_word("dict>s ", interpret_dict_to_slice);
  d.def_stack_word("dict, ", interpret_store_dict);
  d.def_stack_word("dict@ ", std::bind(interpret_load_dict, _1, false));
  d.def_stack_word("dict@+ ", std::bind(interpret_load_dict, _1, true));
  d.def_stack_word("sdict!+ ", std::bind(interpret_dict_add, _1, vm::Dictionary::SetMode::Add, false, -1));
  d.def_stack_word("sdict! ", std::bind(interpret_dict_add, _1, vm::Dictionary::SetMode::Set, false, -1));
  d.def_stack_word("b>sdict!+ ", std::bind(interpret_dict_add, _1, vm::Dictionary::SetMode::Add, true, -1));
  d.def_stack_word("b>sdict! ", std::bind(interpret_dict_add, _1, vm::Dictionary::SetMode::Set, true, -1));
  d.def_stack_word("sdict@ ", std::bind(interpret_dict_get, _1, -1, 3));
  d.def_stack_word("sdict@- ", std::bind(interpret_dict_get, _1, -1, 7));
  d.def_stack_word("sdict- ", std::bind(interpret_dict_get, _1, -1, 5));
  d.def_stack_word("udict!+ ", std::bind(interpret_dict_add, _1, vm::Dictionary::SetMode::Add, false, 0));
  d.def_stack_word("udict! ", std::bind(interpret_dict_add, _1, vm::Dictionary::SetMode::Set, false, 0));
  d.def_stack_word("b>udict!+ ", std::bind(interpret_dict_add, _1, vm::Dictionary::SetMode::Add, true, 0));
  d.def_stack_word("b>udict! ", std::bind(interpret_dict_add, _1, vm::Dictionary::SetMode::Set, true, 0));
  d.def_stack_word("udict@ ", std::bind(interpret_dict_get, _1, 0, 3));
  d.def_stack_word("udict@- ", std::bind(interpret_dict_get, _1, 0, 7));
  d.def_stack_word("udict- ", std::bind(interpret_dict_get, _1, 0, 5));
  d.def_stack_word("idict!+ ", std::bind(interpret_dict_add, _1, vm::Dictionary::SetMode::Add, false, 1));
  d.def_stack_word("idict! ", std::bind(interpret_dict_add, _1, vm::Dictionary::SetMode::Set, false, 1));
  d.def_stack_word("b>idict!+ ", std::bind(interpret_dict_add, _1, vm::Dictionary::SetMode::Add, true, 1));
  d.def_stack_word("b>idict! ", std::bind(interpret_dict_add, _1, vm::Dictionary::SetMode::Set, true, 1));
  d.def_stack_word("idict@ ", std::bind(interpret_dict_get, _1, 1, 3));
  d.def_stack_word("idict@- ", std::bind(interpret_dict_get, _1, 1, 7));
  d.def_stack_word("idict- ", std::bind(interpret_dict_get, _1, 1, 5));
  d.def_stack_word("pfxdict!+ ", std::bind(interpret_pfx_dict_add, _1, vm::Dictionary::SetMode::Add, false));
  d.def_stack_word("pfxdict! ", std::bind(interpret_pfx_dict_add, _1, vm::Dictionary::SetMode::Set, false));
  d.def_stack_word("pfxdict@ ", interpret_pfx_dict_get);
  d.def_ctx_tail_word("dictmap ", std::bind(interpret_dict_map, _1, false, false));
  d.def_ctx_tail_word("dictmapext ", std::bind(interpret_dict_map, _1, true, false));
  d.def_ctx_tail_word("idictmapext ", std::bind(interpret_dict_map, _1, true, true));
  d.def_ctx_tail_word("dictforeach ", std::bind(interpret_dict_foreach, _1, false, false));
  d.def_ctx_tail_word("idictforeach ", std::bind(interpret_dict_foreach, _1, false, true));
  d.def_ctx_tail_word("dictforeachrev ", std::bind(interpret_dict_foreach, _1, true, false));
  d.def_ctx_tail_word("idictforeachrev ", std::bind(interpret_dict_foreach, _1, true, true));
  d.def_ctx_tail_word("dictforeachfromx ", std::bind(interpret_dict_foreach_from, _1, -1));
  d.def_ctx_tail_word("dictmerge ", interpret_dict_merge);
  d.def_ctx_tail_word("dictdiff ", interpret_dict_diff);
  // slice/bitstring constants
  d.def_active_word("x{", interpret_bitstring_hex_literal);
  d.def_active_word("b{", interpret_bitstring_binary_literal);
  // boxes/holes/variables
  d.def_stack_word("hole ", interpret_hole);
  d.def_stack_word("box ", interpret_box);
  d.def_stack_word("@ ", interpret_box_fetch);
  d.def_stack_word("! ", interpret_box_store);
  d.def_stack_word("null ", interpret_push_null);
  d.def_stack_word("null? ", interpret_is_null);
  // tuples/arrays
  d.def_stack_word("| ", interpret_empty_tuple);
  d.def_stack_word(", ", interpret_tuple_push);
  d.def_stack_word("tpop ", interpret_tuple_pop);
  d.def_stack_word("[] ", interpret_tuple_index);
  d.def_stack_word("[]= ", interpret_tuple_set);
  d.def_stack_word("count ", interpret_tuple_len);
  d.def_stack_word("tuple? ", interpret_is_tuple);
  d.def_stack_word("tuple ", interpret_make_tuple);
  d.def_stack_word("untuple ", std::bind(interpret_tuple_explode, _1, true));
  d.def_stack_word("explode ", std::bind(interpret_tuple_explode, _1, false));
  d.def_stack_word("allot ", interpret_allot);
  // atoms
  d.def_stack_word("anon ", interpret_atom_anon);
  d.def_stack_word("(atom) ", interpret_atom);
  d.def_stack_word("atom>$ ", interpret_atom_name);
  d.def_stack_word("eq? ", interpret_is_eq);
  d.def_stack_word("eqv? ", interpret_is_eqv);
  d.def_stack_word("atom? ", interpret_is_atom);
  // execution control
  d.def_ctx_tail_word("execute ", interpret_execute);
  d.def_ctx_tail_word("call/cc ", interpret_call_cc);
  d.def_ctx_tail_word("times ", interpret_execute_times);
  d.def_ctx_tail_word("if ", interpret_if);
  d.def_ctx_tail_word("ifnot ", interpret_ifnot);
  d.def_ctx_tail_word("cond ", interpret_cond);
  d.def_ctx_tail_word("while ", interpret_while);
  d.def_ctx_tail_word("until ", interpret_until);
  // compiler control
  d.def_active_word("[ ", interpret_internal_interpret_begin);
  d.def_active_word("] ", interpret_internal_interpret_end);
  d.def_active_word("{ ", interpret_wordlist_begin);
  d.def_active_word("} ", interpret_wordlist_end);
  d.def_stack_word("({) ", interpret_wordlist_begin_aux);
  d.def_stack_word("(}) ", interpret_wordlist_end_aux);
  d.def_stack_word("(compile) ", interpret_compile_internal);
  d.def_ctx_tail_word("(execute) ", interpret_execute_internal);
  d.def_ctx_tail_word("(interpret-prepare) ", interpret_prepare);
  d.def_active_word("' ", interpret_tick);
  d.def_word("'nop ", LitCont::literal({vm::from_object, nop_word_def}));
  // dictionary manipulation
  d.def_ctx_word("find ", std::bind(interpret_find, _1, 1));
  d.def_ctx_word("(word-prefix-find) ", std::bind(interpret_word_prefix_find, _1, 3));
  d.def_ctx_word("create ", interpret_create);
  d.def_ctx_word("(create) ", std::bind(interpret_create_aux, _1, -1));
  d.def_active_word(": ", std::bind(interpret_colon, _1, 0));
  d.def_active_word(":: ", std::bind(interpret_colon, _1, 1));
  d.def_active_word(":_ ", std::bind(interpret_colon, _1, 2));
  d.def_active_word("::_ ", std::bind(interpret_colon, _1, 3));
  d.def_ctx_word("(forget) ", interpret_forget_aux);
  d.def_ctx_word("forget ", interpret_forget);
  d.def_ctx_word("words ", interpret_words);
  d.def_word("Fift-wordlist ", LitCont::literal(d.get_box()));
  d.def_ctx_word("Fift ", std::bind(interpret_set_context_to, _1, d.get_box()));
  d.def_ctx_word("current@ ", interpret_get_current);
  d.def_ctx_word("current! ", interpret_set_current);
  d.def_ctx_word("context@ ", interpret_get_context);
  d.def_ctx_word("context! ", interpret_set_context);
  d.def_ctx_word(".bt ", interpret_print_backtrace);
  d.def_ctx_word("cont. ", interpret_print_continuation);
  // input parse
  d.def_ctx_word("word ", interpret_word);
  d.def_ctx_word("(word) ", interpret_word_ext);
  d.def_ctx_word("skipspc ", interpret_skipspc);
  d.def_ctx_word("seekeof? ", std::bind(interpret_seekeof, _1, 1));
  d.def_ctx_word("(seekeof?) ", std::bind(interpret_seekeof, _1, -1));
  d.def_ctx_word("include-depth ", interpret_include_depth);
  d.def_ctx_tail_word("include ", interpret_include);
  d.def_ctx_tail_word("skip-to-eof ", interpret_skip_source);
  d.def_word("'exit-interpret ", LitCont::literal(exit_interpret));
  d.def_ctx_word("abort ", interpret_abort);
  d.def_ctx_word("quit ", interpret_quit);
  d.def_ctx_word("bye ", interpret_bye);
  d.def_ctx_word("halt ", interpret_halt);
  // cmdline args
  d.def_word("$* ", LitCont::literal(cmdline_args));
  d.def_stack_word("$# ", interpret_get_cmdline_arg_count);
  d.def_ctx_tail_word("$() ", interpret_get_cmdline_arg);
}

void init_words_ton(Dictionary& d) {
  using namespace std::placeholders;
  d.def_stack_word("smca>$ ", interpret_pack_std_smc_addr);
  d.def_stack_word("$>smca ", interpret_unpack_std_smc_addr);
  d.def_stack_word("B>base64 ", std::bind(interpret_bytes_to_base64, _1, false));
  d.def_stack_word("B>base64url ", std::bind(interpret_bytes_to_base64, _1, true));
  d.def_stack_word("base64>B ", std::bind(interpret_base64_to_bytes, _1, false, false));
  d.def_stack_word("base64url>B ", std::bind(interpret_base64_to_bytes, _1, true, false));
}

void init_words_vm(Dictionary& d, bool enable_debug) {
  using namespace std::placeholders;
  vm::init_vm(enable_debug).ensure();
  // vm run
  d.def_word("vmlibs ", LitCont::literal(vm_libraries));
  // d.def_ctx_word("runvmcode ", std::bind(interpret_run_vm, _1, 0x40));
  // d.def_ctx_word("runvm ", std::bind(interpret_run_vm, _1, 0x45));
  d.def_ctx_word("runvmx ", std::bind(interpret_run_vm, _1, -1));
  d.def_stack_word("vmcont, ", interpret_store_vm_cont);
  d.def_stack_word("vmcont@ ", interpret_fetch_vm_cont);
  d.def_stack_word("(vmoplen) ", interpret_vmop_len);
  d.def_stack_word("(vmopdump) ", interpret_vmop_dump);
  d.def_ctx_word("supported-version ", interpret_supported_version);
}

void import_cmdline_args(Dictionary& d, std::string arg0, int n, const char* const argv[]) {
  using namespace std::placeholders;
  LOG(DEBUG) << "import_cmdlist_args(" << arg0 << "," << n << ")";
  d.def_word("$0 ", LitCont::literal(arg0));
  vm::StackEntry list;
  for (int i = n - 1; i >= 0; i--) {
    list = vm::StackEntry::cons(vm::StackEntry{argv[i]}, list);
  }
  cmdline_args->set(std::move(list));
  for (int i = 1; i <= n; i++) {
    char buffer[14];
    snprintf(buffer, sizeof(buffer), "$%d ", i);
    d.def_stack_word(buffer, std::bind(interpret_get_fixed_cmdline_arg, _1, i));
  }
}

}  // namespace fift
