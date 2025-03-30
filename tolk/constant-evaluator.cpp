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
#include "constant-evaluator.h"
#include "ast.h"
#include "tolk.h"
#include "openssl/digest.hpp"
#include "crypto/common/util.h"
#include "td/utils/crypto.h"
#include "ton/ton-types.h"

namespace tolk {

GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_const_string_required(SrcLocation loc, std::string_view f_name, std::string_view example_arg) {
  std::string name = static_cast<std::string>(f_name);
  std::string example = static_cast<std::string>(example_arg);
  throw ParseError(loc, "function `" +  name + "` requires a constant string, like `" + name + "(\"" + example + "\")`");
}


// parse address like "EQCRDM9h4k3UJdOePPuyX40mCgA4vxge5Dc5vjBR8djbEKC5"
// based on unpack_std_smc_addr() from block.cpp
// (which is not included to avoid linking with ton_crypto)
static bool parse_friendly_address(const char packed[48], ton::WorkchainId& workchain, ton::StdSmcAddress& addr) {
  unsigned char buffer[36];
  if (!td::buff_base64_decode(td::MutableSlice{buffer, 36}, td::Slice{packed, 48}, true)) {
    return false;
  }
  td::uint16 crc = td::crc16(td::Slice{buffer, 34});
  if (buffer[34] != (crc >> 8) || buffer[35] != (crc & 0xff) || (buffer[0] & 0x3f) != 0x11) {
    return false;
  }
  workchain = static_cast<td::int8>(buffer[1]);
  std::memcpy(addr.data(), buffer + 2, 32);
  return true;
}

// parse address like "0:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8"
// based on StdAddress::parse_addr() from block.cpp
// (which is not included to avoid linking with ton_crypto)
static bool parse_raw_address(std::string_view acc_string, int& workchain, ton::StdSmcAddress& addr) {
  size_t pos = acc_string.find(':');
  if (pos != std::string::npos) {
    td::Result<int> r_wc = td::to_integer_safe<ton::WorkchainId>(td::Slice(acc_string.data(), pos));
    if (r_wc.is_error()) {
      return false;
    }
    workchain = r_wc.move_as_ok();
    pos++;
  } else {
    pos = 0;
  }
  if (acc_string.size() != pos + 64) {
    return false;
  }

  for (int i = 0; i < 64; ++i) {    // loop through each hex digit
    char c = acc_string[pos + i];
    int x;
    if (c >= '0' && c <= '9') {
      x = c - '0';
    } else if (c >= 'a' && c <= 'z') {
      x = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'Z') {
      x = c - 'A' + 10;
    } else {
      return false;
    }

    if ((i & 1) == 0) {
      addr.data()[i >> 1] = static_cast<unsigned char>((addr.data()[i >> 1] & 0x0F) | (x << 4));
    } else {
      addr.data()[i >> 1] = static_cast<unsigned char>((addr.data()[i >> 1] & 0xF0) | x);
    }
  }
  return true;
}

// internal helper: for `ton("0.05")`, parse string literal "0.05" to 50000000
static td::RefInt256 parse_nanotons_as_floating_string(SrcLocation loc, std::string_view str) {
  bool is_negative = false;
  size_t i = 0;

  // handle optional leading sign
  if (str[0] == '-') {
    is_negative = true;
    i++;
  } else if (str[0] == '+') {
    i++;
  }

  // parse "0.05" into integer part (before dot) and fractional (after)
  int64_t integer_part = 0;
  int64_t fractional_part = 0;
  int fractional_digits = 0;
  bool seen_dot = false;

  for (; i < str.size(); ++i) {
    char c = str[i];
    if (c == '.') {
      if (seen_dot) {
        throw ParseError(loc, "argument is not a valid number like \"0.05\"");
      }
      seen_dot = true;
    } else if (c >= '0' && c <= '9') {
      if (!seen_dot) {
        integer_part = integer_part * 10 + (c - '0');
      } else if (fractional_digits < 9) {
        fractional_part = fractional_part * 10 + (c - '0');
        fractional_digits++;
      }
    } else {
      throw ParseError(loc, "argument is not a valid number like \"0.05\"");
    }
  }

  while (fractional_digits < 9) {     // after "0.05" fractional_digits is 2, scale up to 9
    fractional_part *= 10;
    fractional_digits++;
  }

  int64_t result = integer_part * 1000000000LL + fractional_part;
  return td::make_refint(is_negative ? -result : result);
}

static ConstantValue parse_vertex_string_const(V<ast_string_const> v) {
  td::Slice str_slice = td::Slice(v->str_val.data(), v->str_val.size());
  return ConstantValue(td::hex_encode(str_slice));
}

// given `ton("0.05")` evaluate it to 50000000
// given `stringCrc32("some_str")` evaluate it
// etc.
// currently, all compile-time functions accept 1 argument, a literal string
static ConstantValue parse_vertex_call_to_compile_time_function(V<ast_function_call> v, std::string_view f_name) {
  tolk_assert(v->get_num_args() == 1);    // checked by type inferring
  AnyExprV v_arg = v->get_arg(0)->get_expr();

  std::string_view str;
  if (auto as_string = v_arg->try_as<ast_string_const>()) {
    str = as_string->str_val;
  } else {
    // ton(SOME_CONST) is not supported
    // ton(0.05) is not supported (it can't be represented in AST even)
    // stringCrc32(SOME_CONST) / stringCrc32(some_var) also, it's compile-time literal-only
  }
  if (str.empty()) {
    fire_error_const_string_required(v->loc, f_name, f_name == "ton" ? "0.05" : "some_str");
  }

  if (f_name == "ton") {
    return ConstantValue(parse_nanotons_as_floating_string(v_arg->loc, str));
  }

  if (f_name == "stringCrc32") {          // previously, postfix "..."c
    return ConstantValue(td::make_refint(td::crc32(td::Slice{str.data(), str.size()})));
  }

  if (f_name == "stringCrc16") {          // previously, there was no postfix in FunC, no way to calc at compile-time
    return ConstantValue(td::make_refint(td::crc16(td::Slice{str.data(), str.size()})));
  }

  if (f_name == "stringSha256") {         // previously, postfix "..."H
    unsigned char hash[32];
    digest::hash_str<digest::SHA256>(hash, str.data(), str.size());
    return ConstantValue(td::bits_to_refint(hash, 256, false));
  }

  if (f_name == "stringSha256_32") {      // previously, postfix "..."h
    unsigned char hash[32];
    digest::hash_str<digest::SHA256>(hash, str.data(), str.size());
    return ConstantValue(td::bits_to_refint(hash, 32, false));
  }

  if (f_name == "stringAddressToSlice") { // previously, postfix "..."a
    ton::WorkchainId workchain;
    ton::StdSmcAddress addr;
    bool correct = (str.size() == 48 && parse_friendly_address(str.data(), workchain, addr)) ||
                   (str.size() != 48 && parse_raw_address(str, workchain, addr));
    if (!correct) {
      v_arg->error("invalid standard address");
    }
    if (workchain < -128 || workchain >= 128) {
      v_arg->error("anycast addresses not supported");
    }

    unsigned char data[3 + 8 + 256];  // addr_std$10 anycast:(Maybe Anycast) workchain_id:int8 address:bits256 = MsgAddressInt;
    td::bitstring::bits_store_long_top(data, 0, static_cast<uint64_t>(4) << (64 - 3), 3);
    td::bitstring::bits_store_long_top(data, 3, static_cast<uint64_t>(workchain) << (64 - 8), 8);
    td::bitstring::bits_memcpy(data, 3 + 8, addr.bits().ptr, 0, ton::StdSmcAddress::size());
    return ConstantValue(td::BitSlice{data, sizeof(data)}.to_hex());
  }

  if (f_name == "stringHexToSlice") {     // previously, postfix "..."s
    unsigned char buff[128];
    long bits = td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), str.data(), str.data() + str.size());
    if (bits < 0) {
      v_arg->error("invalid hex bitstring constant");
    }
    return ConstantValue(static_cast<std::string>(str));
  }

  if (f_name == "stringToBase256") {      // previously, postfix "..."u
    td::RefInt256 intval = td::hex_string_to_int256(td::hex_encode(td::Slice(str.data(), str.size())));
    if (str.empty()) {
      v_arg->error("empty integer ascii-constant");
    }
    if (intval.is_null()) {
      v_arg->error("too long integer ascii-constant");
    }
    return ConstantValue(std::move(intval));
  }

  tolk_assert(false);
  return ConstantValue();
}


struct ConstantEvaluator {
  static bool is_overflow(const td::RefInt256& intval) {
    return intval.is_null() || !intval->signed_fits_bits(257);
  }

  static ConstantValue handle_unary_operator(V<ast_unary_operator> v, const ConstantValue& rhs) {
    tolk_assert(rhs.is_int());   // type inferring has passed before, so it's int/bool
    td::RefInt256 intval = rhs.as_int();

    switch (v->tok) {
      case tok_minus:
        intval = -intval;
        break;
      case tok_plus:
        break;
      case tok_bitwise_not:
        intval = ~intval;
        break;
      case tok_logical_not:
        intval = td::make_refint(intval == 0 ? -1 : 0);
        break;
      default:
        v->error("not a constant expression");
    }

    if (is_overflow(intval)) {
      v->error("integer overflow");
    }
    return ConstantValue(std::move(intval));
  }

  static ConstantValue handle_binary_operator(V<ast_binary_operator> v, const ConstantValue& lhs, const ConstantValue& rhs) {
    tolk_assert(lhs.is_int() && rhs.is_int());   // type inferring has passed before, so they are int/bool
    td::RefInt256 lhs_intval = lhs.as_int();
    td::RefInt256 rhs_intval = rhs.as_int();
    td::RefInt256 intval;

    switch (v->tok) {
      case tok_minus:
        intval = lhs_intval - rhs_intval;
        break;
      case tok_plus:
        intval = lhs_intval + rhs_intval;
        break;
      case tok_mul:
        intval = lhs_intval * rhs_intval;
        break;
      case tok_div:
        intval = lhs_intval / rhs_intval;
        break;
      case tok_mod:
        intval = lhs_intval % rhs_intval;
        break;
      case tok_lshift:
        intval = lhs_intval << static_cast<int>(rhs_intval->to_long());
        break;
      case tok_rshift:
        intval = lhs_intval >> static_cast<int>(rhs_intval->to_long());
        break;
      case tok_bitwise_and:
        intval = lhs_intval & rhs_intval;
        break;
      case tok_bitwise_or:
        intval = lhs_intval | rhs_intval;
        break;
      case tok_bitwise_xor:
        intval = lhs_intval ^ rhs_intval;
        break;
      case tok_eq:
        intval = td::make_refint(lhs_intval == rhs_intval ? -1 : 0);
        break;
      case tok_lt:
        intval = td::make_refint(lhs_intval < rhs_intval ? -1 : 0);
        break;
      case tok_gt:
        intval = td::make_refint(lhs_intval > rhs_intval ? -1 : 0);
        break;
      case tok_leq:
        intval = td::make_refint(lhs_intval <= rhs_intval ? -1 : 0);
        break;
      case tok_geq:
        intval = td::make_refint(lhs_intval >= rhs_intval ? -1 : 0);
        break;
      case tok_neq:
        intval = td::make_refint(lhs_intval != rhs_intval ? -1 : 0);
        break;
      case tok_logical_and:
        intval = td::make_refint(lhs_intval != 0 && rhs_intval != 0 ? -1 : 0);
        break;
      case tok_logical_or:
        intval = td::make_refint(lhs_intval != 0 || rhs_intval != 0 ? -1 : 0);
        break;
      default:
        v->error("unsupported binary operator in constant expression");
    }

    if (is_overflow(intval)) {
      v->error("integer overflow");
    }
    return ConstantValue(std::move(intval));
  }

  // `const a = 1 + b`, we met `b`
  static ConstantValue handle_reference(V<ast_reference> v) {
    GlobalConstPtr const_ref = v->sym->try_as<GlobalConstPtr>();
    if (!const_ref) {
      v->error("symbol `" + static_cast<std::string>(v->get_name()) + "` is not a constant");
    }

    if (!const_ref->value.initialized()) {          // maybe, `b` was already calculated
      eval_and_assign_const_init_value(const_ref);  // if not, dig recursively into `b`
    }
    return const_ref->value;
  }

  // `const a = ton("0.05")`, we met `ton("0.05")`
  static ConstantValue handle_function_call(V<ast_function_call> v) {
    if (v->fun_maybe && v->fun_maybe->is_compile_time_only()) {
      return parse_vertex_call_to_compile_time_function(v, v->fun_maybe->name);
    }
    v->error("not a constant expression");
  }

  static ConstantValue visit(AnyExprV v) {
    if (auto v_int = v->try_as<ast_int_const>()) {
      return ConstantValue(v_int->intval);
    }
    if (auto v_bool = v->try_as<ast_bool_const>()) {
      return ConstantValue(v_bool->bool_val ? -1 : 0);
    }
    if (auto v_unop = v->try_as<ast_unary_operator>()) {
      return handle_unary_operator(v_unop, visit(v_unop->get_rhs()));
    }
    if (auto v_binop = v->try_as<ast_binary_operator>()) {
      return handle_binary_operator(v_binop, visit(v_binop->get_lhs()), visit(v_binop->get_rhs()));
    }
    if (auto v_ref = v->try_as<ast_reference>()) {
      return handle_reference(v_ref);
    }
    if (auto v_call = v->try_as<ast_function_call>()) {
      return handle_function_call(v_call);
    }
    if (auto v_par = v->try_as<ast_parenthesized_expression>()) {
      return visit(v_par->get_expr());
    }
    if (auto v_cast_to = v->try_as<ast_cast_as_operator>()) {
      return visit(v_cast_to->get_expr());
    }
    if (auto v_string = v->try_as<ast_string_const>()) {
      return parse_vertex_string_const(v_string);
    }
    v->error("not a constant expression");
  }

  // evaluate `const a = 2 + 3` into 5
  // type inferring has already passed, to types are correct, `const a = 1 + ""` can not occur
  // recursive initializers `const a = b; const b = a` also 100% don't exist, checked on type inferring
  // if init_value is not a constant expression like `const a = foo()`, an exception is thrown
  static ConstantValue eval_const_init_value(GlobalConstPtr const_ref) {
    return visit(const_ref->init_value);
  }
};

ConstantValue eval_string_const_standalone(AnyExprV v_string) {
  auto v = v_string->try_as<ast_string_const>();
  tolk_assert(v);
  return parse_vertex_string_const(v);
}

ConstantValue eval_call_to_compile_time_function(AnyExprV v_call) {
  auto v = v_call->try_as<ast_function_call>();
  tolk_assert(v && v->fun_maybe->is_compile_time_only());
  return parse_vertex_call_to_compile_time_function(v, v->fun_maybe->name);
}

void eval_and_assign_const_init_value(GlobalConstPtr const_ref) {
  ConstantValue init_value = ConstantEvaluator::eval_const_init_value(const_ref);
  const_ref->mutate()->assign_const_value(std::move(init_value));
}

} // namespace tolk
