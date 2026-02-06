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
#include "compilation-errors.h"
#include "generics-helpers.h"
#include "type-system.h"
#include "openssl/digest.hpp"
#include "crypto/common/util.h"
#include "td/utils/crypto.h"
#include "ton/ton-types.h"
#include <unordered_map>

namespace tolk {

static Error err_const_string_required(std::string_view f_name, std::string_view example_arg) {
  return err("function `{}` requires a constant string, like `{}(\"{}\")`", f_name, f_name, example_arg);
}

static Error err_const_string_required(std::string_view method_name) {
  return err("method `{}` requires a constant string, like \"some_str\".{}()", method_name, method_name);
}

static Error err_not_a_constant_expression() {
  return err("not a constant expression");
}

static thread_local std::unordered_map<GlobalConstPtr, ConstValExpression> computed_constants_cache;


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
  if (pos == std::string::npos) {   // workchain (before a colon) is mandatory
    return false;
  }
  td::Result<int> r_wc = td::to_integer_safe<ton::WorkchainId>(td::Slice(acc_string.data(), pos++));
  if (r_wc.is_error()) {
    return false;
  }
  workchain = r_wc.move_as_ok();

  if (acc_string.size() != pos + 64) {
    return false;
  }

  for (int i = 0; i < 64; ++i) {    // loop through each hex digit
    char c = acc_string[pos + i];
    int x;
    if (c >= '0' && c <= '9') {
      x = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      x = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
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

static void parse_any_std_address(std::string_view str, SrcRange range, unsigned char (*data)[267]) {
  ton::WorkchainId workchain = 0;
  ton::StdSmcAddress addr;
  bool correct = (str.size() == 48 && parse_friendly_address(str.data(), workchain, addr)) ||
                 (str.size() != 48 && parse_raw_address(str, workchain, addr));
  if (!correct) {
    err("invalid standard address").fire(range);
  }
  if (workchain < -128 || workchain >= 128) {
    err("anycast addresses not supported").fire(range);
  }

  td::bitstring::bits_store_long_top(*data, 0, static_cast<uint64_t>(4) << (64 - 3), 3);
  td::bitstring::bits_store_long_top(*data, 3, static_cast<uint64_t>(workchain) << (64 - 8), 8);
  td::bitstring::bits_memcpy(*data, 3 + 8, addr.bits().ptr, 0, ton::StdSmcAddress::size());
}

// internal helper: for `ton("0.05")`, parse string literal "0.05" to 50000000
static td::RefInt256 parse_nanotons_as_floating_string(SrcRange range, std::string_view str) {
  bool is_negative = false;
  size_t i = 0;

  // handle optional leading sign
  if (str[0] == '-') {
    is_negative = true;
    i++;
  } else if (str[0] == '+') {
    i++;
  }

  if (i >= str.size()) {
    err("argument is not a valid number like \"0.05\"").fire(range);
  }

  // parse "0.05" into integer part (before dot) and fractional (after)
  int64_t integer_part = 0;
  int64_t fractional_part = 0;
  int integer_digits = 0;
  int fractional_digits = 0;
  bool seen_dot = false;

  for (; i < str.size(); ++i) {
    char c = str[i];
    if (c == '.' && !seen_dot && integer_digits > 0) {
      seen_dot = true;
    } else if (c >= '0' && c <= '9' && !seen_dot) {
      integer_part = integer_part * 10 + (c - '0');
      if (++integer_digits > 9) {
        err("argument is too big and leads to overflow").fire(range);
      }
    } else if (c >= '0' && c <= '9') {
      fractional_part = fractional_part * 10 + (c - '0');
      if (++fractional_digits > 9) {
        err("too many digits after a dot, nanotons are 10^9").fire(range);
      }
    } else {
      err("argument is not a valid number like \"0.05\"").fire(range);
    }
  }

  while (fractional_digits < 9) {     // after "0.05" fractional_digits is 2, scale up to 9
    fractional_part *= 10;
    fractional_digits++;
  }

  int64_t result = integer_part * 1000000000LL + fractional_part;
  return td::make_refint(is_negative ? -result : result);
}

// `1 + 2` evaluated to ConstValInt `3` (and similar expressions inside constants and default values),
// are created using this function that fires "integer overflow"
static ConstValInt create_const_int(td::RefInt256&& int_val, AnyExprV origin) {
  // for "division by zero" (value is NaN, it's invalid) we also fire "integer overflow"
  if (UNLIKELY(!int_val->signed_fits_bits(257))) {
    err("integer overflow").fire(origin);
  }
  return ConstValInt{std::move(int_val)};
}

// a simple wrapper for ConstValBool constructor 
static ConstValBool create_const_bool(bool bool_val) {
  return ConstValBool{bool_val};
}

// a simple wrapper for ConstValCastToType constructor
static ConstValCastToType create_const_cast(ConstValExpression&& inner, TypePtr cast_to) {
  // to store ConstValExpression inside (recursively), we need to place it into the heap;
  // the best way I've found is to create std::vector with a single element (std::unique_ptr doesn't work)
  return ConstValCastToType{{std::move(inner)}, cast_to};
}

// extract `5` from `(5 as int32 as int64)` in terms of ConstValExpression
static ConstValExpression unwrap_const_cast(ConstValExpression val) {
  while (const ConstValCastToType* val_cast = std::get_if<ConstValCastToType>(&val)) {
    val = val_cast->inner.front();
  }
  return val;
}

// operator lshift is missing in td::BigInt implementation
static td::RefInt256 operator<<(td::RefInt256 x, const td::RefInt256& y) {
  if (y < 0 || y > 256) {
    x.write().invalidate();
  } else {
    x.write() <<= static_cast<int>(y->to_long());
  }
  return x;
}

// operator rshift is missing in td::BigInt implementation
static td::RefInt256 operator>>(td::RefInt256 x, const td::RefInt256& y) {
  if (y < 0 || y > 256) {
    x.write().invalidate();
  } else {
    x.write() >>= static_cast<int>(y->to_long());
  }
  return x;
}

// for `"some_str".crc32()` we also accept any string-const expressions, like `SOME_STR.crc32()`
static bool extract_string_literal_from_v(AnyExprV v, std::string& out) {
  try {
    ConstValExpression val = unwrap_const_cast(eval_expression_if_const_or_fire(v));
    if (ConstValString* val_s = std::get_if<ConstValString>(&val)) {
      out = std::move(val_s->str_val);
      return true;
    }
  } catch (...) {}
  return false;
}

// given `ton("0.05")` evaluate it to 50000000
// given `stringCrc32("some_str")` or `"some_str".crc32()` evaluate it
// etc.
static ConstValExpression parse_vertex_call_to_compile_time_function(V<ast_function_call> v, std::string_view f_name) {
  TypePtr receiver = v->fun_maybe->receiver_type;

  // reflection — compile-time introspection;
  // most methods of `reflect` are not "consteval", only a couple are, to be used in constants
  if (v->fun_maybe->is_static_method() && receiver->try_as<TypeDataStruct>()) {
    f_name = v->fun_maybe->method_name;

    if (f_name == "typeNameOf" || f_name == "typeNameOfObject") {
      TypePtr typeT = v->fun_maybe->substitutedTs->typeT_at(0);
      return ConstValString{typeT->as_human_readable()};
    }

    if (f_name == "sourceLocation") {
      SrcRange::DecodedRange d = v->range.decode_offsets();
      StructPtr s_SourceLocation = v->fun_maybe->declared_return_type->try_as<TypeDataStruct>()->struct_ref;
      return ConstValObject{s_SourceLocation, {
        ConstValInt{td::make_refint(d.start_line_no)},
        ConstValInt{td::make_refint(d.start_char_no)},
        ConstValString{v->range.get_src_file()->realpath},
      }};
    }

    if (f_name == "sourceLocationAsString") {
      SrcRange::DecodedRange d = v->range.decode_offsets();
      std::string loc_str = v->range.get_src_file()->realpath + ":" + std::to_string(d.start_line_no) + ":" + std::to_string(d.start_char_no);
      return ConstValString{std::move(loc_str)};
    }
  }

  // string methods: "hello".crc32(), "hello".sha256(), etc.
  // copy-paste from `stringCrc32()` and similar (below), which are deprecated and will be deleted soon
  if (receiver == TypeDataString::create()) {
    f_name = v->fun_maybe->method_name;

    // support both `"abc".crc32()` and `string.crc32("abc")`
    tolk_assert(v->get_num_args() == !v->dot_obj_is_self);
    AnyExprV self_obj = v->dot_obj_is_self ? v->get_self_obj() : v->get_arg(0)->get_expr();
    std::string str;
    if (!extract_string_literal_from_v(self_obj, str)) {
      err_const_string_required(f_name).fire(v);
    }

    if (f_name == "crc32") {
      return ConstValInt{td::make_refint(td::crc32(td::Slice{str.data(), str.size()}))};
    }
    if (f_name == "crc16") {
      return ConstValInt{td::make_refint(td::crc16(td::Slice{str.data(), str.size()}))};
    }
    if (f_name == "sha256") {
      unsigned char hash[32];
      digest::hash_str<digest::SHA256>(hash, str.data(), str.size());
      return ConstValInt{td::bits_to_refint(hash, 256, false)};
    }
    if (f_name == "sha256_32") {
      unsigned char hash[32];
      digest::hash_str<digest::SHA256>(hash, str.data(), str.size());
      return ConstValInt{td::bits_to_refint(hash, 32, false)};
    }
    if (f_name == "hexToSlice") {
      unsigned char buff[128];
      long bits = td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), str.data(), str.data() + str.size());
      if (bits < 0 || bits > 1023) {
        err("invalid hex bitstring constant").fire(self_obj);
      }
      return ConstValSlice{static_cast<std::string>(str)};
    }
    if (f_name == "toBase256") {
      td::RefInt256 intval = td::hex_string_to_int256(td::hex_encode(td::Slice(str.data(), str.size())));
      if (intval.is_null() || !intval->signed_fits_bits(257)) {
        err("invalid or too long integer ascii-constant").fire(self_obj);
      }
      return ConstValInt{std::move(intval)};
    }
    if (f_name == "literalSlice") {
      if (str.size() > 127) {
        err("too long const string, can't get raw bytes, it's a snake cell").fire(self_obj);
      }
      return ConstValSlice{td::hex_encode(td::Slice(str.data(), str.size()))};
    }
  }

  tolk_assert(v->get_num_args() == 1);    // checked by type inferring
  AnyExprV v_arg = v->get_arg(0)->get_expr();

  std::string str;
  if (!extract_string_literal_from_v(v_arg, str)) {
    // ton(SOME_CONST) is not supported
    // ton(0.05) is not supported (it can't be represented in AST even)
    // stringCrc32(SOME_CONST) / stringCrc32(some_var) also, it's compile-time literal-only
    err_const_string_required(f_name, f_name == "ton" ? "0.05" : "some_str").fire(v);
  }

  if (f_name == "ton") {
    return create_const_cast(   // insert "50000000 as coins"
      ConstValInt{parse_nanotons_as_floating_string(v_arg->range, str)},
      TypeDataCoins::create()
    );
  }

  if (f_name == "address") {              // previously, postfix "..."a
    unsigned char data[267];
    parse_any_std_address(str, v_arg->range, &data);
    return ConstValAddress{td::BitSlice{data, sizeof(data)}.to_hex()};
  }

  if (f_name == "stringCrc32") {          // previously, postfix "..."c
    return ConstValInt{td::make_refint(td::crc32(td::Slice{str.data(), str.size()}))};
  }

  if (f_name == "stringCrc16") {          // previously, there was no postfix in FunC, no way to calc at compile-time
    return ConstValInt{td::make_refint(td::crc16(td::Slice{str.data(), str.size()}))};
  }

  if (f_name == "stringSha256") {         // previously, postfix "..."H
    unsigned char hash[32];
    digest::hash_str<digest::SHA256>(hash, str.data(), str.size());
    return ConstValInt{td::bits_to_refint(hash, 256, false)};
  }

  if (f_name == "stringSha256_32") {      // previously, postfix "..."h
    unsigned char hash[32];
    digest::hash_str<digest::SHA256>(hash, str.data(), str.size());
    return ConstValInt{td::bits_to_refint(hash, 32, false)};
  }

  if (f_name == "stringHexToSlice") {     // previously, postfix "..."s
    unsigned char buff[128];
    long bits = td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), str.data(), str.data() + str.size());
    if (bits < 0 || bits > 1023) {
      err("invalid hex bitstring constant").fire(v_arg);
    }
    return ConstValSlice{static_cast<std::string>(str)};    // the same string, we've just checked it
  }

  if (f_name == "stringToBase256") {      // previously, postfix "..."u
    td::RefInt256 intval = td::hex_string_to_int256(td::hex_encode(td::Slice(str.data(), str.size())));
    if (intval.is_null() || !intval->signed_fits_bits(257)) {
      err("invalid or too long integer ascii-constant").fire(v_arg);
    }
    return ConstValInt{std::move(intval)};
  }

  tolk_assert(false);
}


// ConstExpressionEvaluator is a class with static methods (visitors)
// handling supported operations inside constant expressions
// (in `const name = ...`, field/param defaults, enum members, etc.)
class ConstExpressionEvaluator {
  // `-5` => int(-5), `!true` => false 
  static ConstValExpression handle_unary_operator(V<ast_unary_operator> v) {
    ConstValExpression expr = unwrap_const_cast(eval_any_v_or_fire(v->get_rhs()));

    switch (v->tok) {
      case tok_minus:
        if (const ConstValInt* i = std::get_if<ConstValInt>(&expr)) {
          return create_const_int(-i->int_val, v);
        }
        break;
      case tok_bitwise_not:
        if (const ConstValInt* i = std::get_if<ConstValInt>(&expr)) {
          return create_const_int(~i->int_val, v);
        }
        break;
      case tok_plus:
        if (const ConstValInt* i = std::get_if<ConstValInt>(&expr)) {
          return create_const_int(td::RefInt256(i->int_val), v);
        }
        break;
      case tok_logical_not:
        if (const ConstValInt* i = std::get_if<ConstValInt>(&expr)) {
          return create_const_bool(i->int_val == 0);
        }
        if (const ConstValBool* b = std::get_if<ConstValBool>(&expr)) {
          return create_const_bool(!b->bool_val);
        }
        break;
      default:
        break;
    }
    err_not_a_constant_expression().fire(v);
  }

  // `2 + 3` => int(5), `10 > 3` => true, `true & false` => 0
  static ConstValExpression handle_binary_operator(V<ast_binary_operator> v) {
    ConstValExpression expr_lhs = unwrap_const_cast(eval_any_v_or_fire(v->get_lhs())); 
    ConstValExpression expr_rhs = unwrap_const_cast(eval_any_v_or_fire(v->get_rhs()));

    // operators for 2 integers
    if (std::holds_alternative<ConstValInt>(expr_lhs) && std::holds_alternative<ConstValInt>(expr_rhs)) {
      td::RefInt256 lhs = std::get<ConstValInt>(expr_lhs).int_val;
      td::RefInt256 rhs = std::get<ConstValInt>(expr_rhs).int_val;

      switch (v->tok) {
        case tok_plus:          return create_const_int(lhs + rhs, v);
        case tok_minus:         return create_const_int(lhs - rhs, v);
        case tok_mul:           return create_const_int(lhs * rhs, v);
        case tok_div:           return create_const_int(lhs / rhs, v);
        case tok_mod:           return create_const_int(lhs % rhs, v);
        case tok_bitwise_and:   return create_const_int(lhs & rhs, v);
        case tok_bitwise_or:    return create_const_int(lhs | rhs, v);
        case tok_bitwise_xor:   return create_const_int(lhs ^ rhs, v);
        case tok_lshift:        return create_const_int(lhs << rhs, v);
        case tok_rshift:        return create_const_int(lhs >> rhs, v);
        case tok_logical_and:   return create_const_bool(lhs != 0 && rhs != 0);
        case tok_logical_or:    return create_const_bool(lhs != 0 || rhs != 0);
        case tok_gt:            return create_const_bool(td::cmp(lhs, rhs) >  0);
        case tok_geq:           return create_const_bool(td::cmp(lhs, rhs) >= 0);
        case tok_lt:            return create_const_bool(td::cmp(lhs, rhs) <  0);
        case tok_leq:           return create_const_bool(td::cmp(lhs, rhs) <= 0);
        case tok_eq:            return create_const_bool(td::cmp(lhs, rhs) == 0);
        case tok_neq:           return create_const_bool(td::cmp(lhs, rhs) != 0);
        default:                break;
      }
    }

    // operators for 2 booleans
    if (std::holds_alternative<ConstValBool>(expr_lhs) && std::holds_alternative<ConstValBool>(expr_rhs)) {
      bool lhs = std::get<ConstValBool>(expr_lhs).bool_val;
      bool rhs = std::get<ConstValBool>(expr_rhs).bool_val;

      switch (v->tok) {
        case tok_bitwise_and:   return create_const_bool((lhs & rhs) != 0);
        case tok_bitwise_or:    return create_const_bool((lhs | rhs) != 0);
        case tok_bitwise_xor:   return create_const_bool((lhs ^ rhs) != 0);
        case tok_logical_and:   return create_const_bool(lhs && rhs);
        case tok_logical_or:    return create_const_bool(lhs || rhs);
        case tok_eq:            return create_const_bool(lhs == rhs);
        case tok_neq:           return create_const_bool(lhs != rhs);
        default:                break;
      }
    }

    err("can not calculate the value of operator `{}` in a constant expression", v->operator_name).fire(v);
  }

  // `lhs as <type>`; we allow any `as` casts inside constants, storing the cast as a separate constexpr
  static ConstValExpression handle_cast_as_operator(V<ast_cast_as_operator> v) {
    ConstValExpression val = eval_any_v_or_fire(v->get_expr());

    TypePtr cast_to = v->inferred_type;
    return create_const_cast(std::move(val), cast_to);
  }

  // `ton("0.05")` and other compile-time functions
  static ConstValExpression handle_function_call(V<ast_function_call> v) {
    FunctionPtr fun_ref = v->fun_maybe;
    if (!fun_ref || !fun_ref->is_compile_time_const_val()) {
      err_not_a_constant_expression().fire(v);
    }
    
    return parse_vertex_call_to_compile_time_function(v, fun_ref->name);
  }
  
  // `const a = ANOTHER`, or in field default, enum member, etc.
  static ConstValExpression handle_reference(V<ast_reference> v) {
    GlobalConstPtr const_ref = v->sym->try_as<GlobalConstPtr>();
    if (!const_ref) {
      err("symbol `{}` is not a constant", v->get_name()).fire(v);
    }
    return eval_and_cache_const_init_val(const_ref);
  }

  // `anotherConst.0` or `Color.Red`
  static ConstValExpression handle_dot_access(V<ast_dot_access> v) {
    if (v->is_target_indexed_access()) {    // anotherConst.0
      int index_at = std::get<int>(v->target);
      ConstValExpression lhs = unwrap_const_cast(eval_any_v_or_fire(v->get_obj()));
      if (ConstValTensor* lhs_tensor = std::get_if<ConstValTensor>(&lhs)) {
        return lhs_tensor->items[index_at];   // an index exists: constant evaluation happens after
      }                                          // type inferring and type checking
      if (ConstValShapedTuple* lhs_shaped = std::get_if<ConstValShapedTuple>(&lhs)) {
        return lhs_shaped->items[index_at];
      }
    }
    if (v->is_target_struct_field()) {      // constObj.field
      ConstValExpression lhs = unwrap_const_cast(eval_any_v_or_fire(v->get_obj()));
      if (ConstValObject* lhs_obj = std::get_if<ConstValObject>(&lhs)) {
        StructFieldPtr field = std::get<StructFieldPtr>(v->target);
        return lhs_obj->fields[field->field_idx];
      }
    }
    if (v->is_target_enum_member()) {       // Color.Red
      EnumDefPtr enum_ref = v->inferred_type->unwrap_alias()->try_as<TypeDataEnum>()->enum_ref;
      std::vector<td::RefInt256> enum_values = calculate_enum_members_with_values(enum_ref);
      td::RefInt256 member_value = enum_values[std::get<EnumMemberPtr>(v->target)->member_idx];
      ConstValInt val{std::move(member_value)};
      return create_const_cast(std::move(val), TypeDataEnum::create(enum_ref));
    }
    err_not_a_constant_expression().fire(v);
  }

public:
  // this function either returns or fires "not a constant expression" or something more meaningful
  static ConstValExpression eval_any_v_or_fire(AnyExprV v) {
    if (auto v_int = v->try_as<ast_int_const>()) {
      return ConstValInt{v_int->intval};
    }
    if (auto v_bool = v->try_as<ast_bool_const>()) {
      return ConstValBool{v_bool->bool_val};
    }
    if (auto v_string = v->try_as<ast_string_const>()) {
      return ConstValString{v_string->str_val};
    }
    if (auto v_un = v->try_as<ast_unary_operator>()) {
      return handle_unary_operator(v_un);
    }
    if (auto v_bin = v->try_as<ast_binary_operator>()) {
      return handle_binary_operator(v_bin);
    }
    if (auto v_as = v->try_as<ast_cast_as_operator>()) {
      return handle_cast_as_operator(v_as);
    }
    if (auto v_ref = v->try_as<ast_reference>()) {
      return handle_reference(v_ref);
    }
    if (auto v_dot = v->try_as<ast_dot_access>()) {
      return handle_dot_access(v_dot);
    }
    if (auto v_call = v->try_as<ast_function_call>()) {
      return handle_function_call(v_call);
    }
    if (auto v_tensor = v->try_as<ast_tensor>()) {
      std::vector<ConstValExpression> items;
      items.reserve(v_tensor->size());
      for (int i = 0; i < v_tensor->size(); ++i) {
        items.push_back(eval_any_v_or_fire(v_tensor->get_item(i)));
      }
      return ConstValTensor{std::move(items)};
    }
    if (auto v_square = v->try_as<ast_square_brackets>()) {
      std::vector<ConstValExpression> items;
      items.reserve(v_square->size());
      for (int i = 0; i < v_square->size(); ++i) {
        items.push_back(eval_any_v_or_fire(v_square->get_item(i)));
      }
      return ConstValShapedTuple{std::move(items)};
    }
    if (auto v_object = v->try_as<ast_object_literal>()) {
      // we also support `const obj: SomeObject = { field: value, ... }`
      // in order to construct ConstValObject correctly, we should also handle missing fields (defaults)
      StructPtr struct_ref = v_object->struct_ref;
      V<ast_object_body> v_body = v_object->get_body();
      std::vector<ConstValExpression> fields;
      fields.reserve(struct_ref->get_num_fields());
      for (StructFieldPtr field_ref : struct_ref->fields) {   // in the declared order
        AnyExprV v_init_val = field_ref->default_value;
        for (int i = 0; i < v_body->get_num_fields(); ++i) {
          if (v_body->get_field(i)->get_field_name() == field_ref->name) {
            v_init_val = v_body->get_field(i)->get_init_val();
            break;
          }
        }
        if (!v_init_val) {    // type `void` and missing fields not supported
          err("some fields of a struct are missing").fire(v);
        }
        fields.emplace_back(eval_any_v_or_fire(v_init_val));
      }
      return ConstValObject{struct_ref, std::move(fields)};
    }
    if (v->try_as<ast_null_keyword>()) {
      return ConstValNullLiteral{};
    }
    err_not_a_constant_expression().fire(v);
  }
};

void check_expression_is_constant_or_fire(AnyExprV v_expr) {
  // inline the most popular case
  if (v_expr->kind == ast_int_const) {
    return;
  }
  ConstExpressionEvaluator::eval_any_v_or_fire(v_expr);
}

ConstValExpression eval_and_cache_const_init_val(GlobalConstPtr const_ref) {
  auto it = computed_constants_cache.find(const_ref);
  if (it != computed_constants_cache.end()) {
    return it->second;
  }

  // constants initializers are not recursive (checked at inferring), so no stack guards here
  ConstValExpression v = ConstExpressionEvaluator::eval_any_v_or_fire(const_ref->init_value);
  // for `const A: coins = 10` or `const A: lisp_list<int> = []` insert a cast for correctness
  if (TypePtr cast_to = const_ref->declared_type) {
    // but don't insert for obvious `const A: coins = ton("1")` or `const A: int = 1`
    const ConstValCastToType* already_cast = std::get_if<ConstValCastToType>(&v);
    const ConstValInt* already_int = std::get_if<ConstValInt>(&v);
    bool insert_cast = already_cast ? !already_cast->cast_to->equal_to(cast_to) : already_int ? cast_to != TypeDataInt::create() : true;
    if (insert_cast) {
      v = create_const_cast(std::move(v), cast_to);
    }
  }
  computed_constants_cache[const_ref] = v;
  return v;
}

std::vector<td::RefInt256> calculate_enum_members_with_values(EnumDefPtr enum_ref) {
  static thread_local std::vector<EnumDefPtr> called_stack;

  // prevent recursion like `enum Color { v = Another.item } enum Another { item = Color.v }`
  // (unlike constants, enums initializers were not checked earlier for recursion)
  bool contains = std::find(called_stack.begin(), called_stack.end(), enum_ref) != called_stack.end();
  if (contains) {
    err("enum `{}` initializers circularly references itself", enum_ref).fire(enum_ref->ident_anchor);
  }

  std::vector<td::RefInt256> values;
  values.reserve(enum_ref->members.size());
  called_stack.push_back(enum_ref);

  td::RefInt256 prev_value = td::make_refint(-1);
  for (EnumMemberPtr member_ref : enum_ref->members) {
    td::RefInt256 cur_value = prev_value + 1;
    if (member_ref->has_init_value()) {
      ConstValExpression assigned = unwrap_const_cast(ConstExpressionEvaluator::eval_any_v_or_fire(member_ref->init_value));
      ConstValInt* assigned_int = std::get_if<ConstValInt>(&assigned);
      if (!assigned_int) {
        err("invalid enum member initializer, not an integer").fire(member_ref->ident_anchor);
      }
      cur_value = assigned_int->int_val;
    }
    if (!cur_value->is_valid() || !cur_value->signed_fits_bits(257)) {
      err("integer overflow").fire(member_ref->ident_anchor);
    }

    values.push_back(cur_value);
    prev_value = std::move(cur_value);
  }

  called_stack.pop_back();
  return values;
}

// for any constant expression: `1 + 2` / `ton("0.05")` / `SOME_STR.crc32()`, consteval them;
// a non-constant expression: `a + b` / `foo()`, will fire (and can be wrapped by try/catch)
ConstValExpression eval_expression_if_const_or_fire(AnyExprV v) {
  return ConstExpressionEvaluator::eval_any_v_or_fire(v);
}

void clear_computed_constants_cache() {
  computed_constants_cache.clear();
}

} // namespace tolk
