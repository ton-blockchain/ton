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

static Error err_not_a_constant_expression() {
  return err("not a constant expression");
}

static std::unordered_map<GlobalConstPtr, ConstValExpression> computed_constants_cache;


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
  ton::WorkchainId workchain;
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

  // parse "0.05" into integer part (before dot) and fractional (after)
  int64_t integer_part = 0;
  int64_t fractional_part = 0;
  int integer_digits = 0;
  int fractional_digits = 0;
  bool seen_dot = false;

  for (; i < str.size(); ++i) {
    char c = str[i];
    if (c == '.') {
      if (seen_dot) {
        err("argument is not a valid number like \"0.05\"").fire(range);
      }
      seen_dot = true;
    } else if (c >= '0' && c <= '9') {
      if (!seen_dot) {
        integer_part = integer_part * 10 + (c - '0');
        if (++integer_digits > 9) {
          err("argument is too big and leads to overflow").fire(range);
        }
      } else if (fractional_digits < 9) {
        fractional_part = fractional_part * 10 + (c - '0');
        fractional_digits++;
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

// given `ton("0.05")` evaluate it to 50000000
// given `stringCrc32("some_str")` evaluate it
// etc.
static ConstValExpression parse_vertex_call_to_compile_time_function(V<ast_function_call> v, std::string_view f_name) {
  // most functions accept 1 argument, but static compile-time methods like `MyStruct.getDeclaredPackPrefix()` have 0 args
  if (v->get_num_args() == 0) {
    TypePtr receiver = v->fun_maybe->receiver_type;
    f_name = v->fun_maybe->method_name;

    if (f_name == "getDeclaredPackPrefix" || f_name == "getDeclaredPackPrefixLen") {
      const TypeDataStruct* t_struct = receiver->try_as<TypeDataStruct>();
      if (!t_struct || !t_struct->struct_ref->opcode.exists()) {
        err("type `{}` does not have a serialization prefix", receiver).fire(v);
      }
      uint64_t val = f_name.ends_with('x') ? t_struct->struct_ref->opcode.pack_prefix : t_struct->struct_ref->opcode.prefix_len;
      return ConstValInt{td::make_refint(val)};
    }
    if (f_name == "typeName" || f_name == "typeNameOfObject") {
      std::string readable = receiver->as_human_readable();
      td::Slice str_slice = td::Slice(readable.data(), std::min(126, static_cast<int>(readable.size())));
      return ConstValSlice{td::hex_encode(str_slice)};
    }
  }

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
    err_const_string_required(f_name, f_name == "ton" ? "0.05" : "some_str").fire(v);
  }

  if (f_name == "ton") {
    return ConstValInt{parse_nanotons_as_floating_string(v_arg->range, str)};
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
    if (bits < 0) {
      err("invalid hex bitstring constant").fire(v_arg);
    }
    return ConstValSlice{static_cast<std::string>(str)};    // the same string, we've just checked it
  }

  if (f_name == "stringToBase256") {      // previously, postfix "..."u
    td::RefInt256 intval = td::hex_string_to_int256(td::hex_encode(td::Slice(str.data(), str.size())));
    if (str.empty()) {
      err("empty integer ascii-constant").fire(v_arg);
    }
    if (intval.is_null()) {
      err("too long integer ascii-constant").fire(v_arg);
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
    ConstValExpression expr = eval_any_v_or_fire(v->get_rhs());

    switch (v->tok) {
      case tok_minus:
        if (const ConstValInt* i = std::get_if<ConstValInt>(&expr)) {
          return ConstValInt{-i->int_val};
        }
        break;
      case tok_bitwise_not:
        if (const ConstValInt* i = std::get_if<ConstValInt>(&expr)) {
          return ConstValInt{~i->int_val};
        }
        break;
      case tok_plus:
        if (const ConstValInt* i = std::get_if<ConstValInt>(&expr)) {
          return ConstValInt{i->int_val};
        }
        break;
      case tok_logical_not:
        if (const ConstValInt* i = std::get_if<ConstValInt>(&expr)) {
          return ConstValBool{i->int_val == 0};
        }
        if (const ConstValBool* b = std::get_if<ConstValBool>(&expr)) {
          return ConstValBool{!b->bool_val};
        }
        break;
      default:
        break;
    }
    err_not_a_constant_expression().fire(v);
  }

  // `2 + 3` => int(5), `10 > 3` => true, `true & false` => 0
  static ConstValExpression handle_binary_operator(V<ast_binary_operator> v) {
    ConstValExpression expr_lhs = eval_any_v_or_fire(v->get_lhs()); 
    ConstValExpression expr_rhs = eval_any_v_or_fire(v->get_rhs());

    td::RefInt256 lhs;
    td::RefInt256 rhs;

    if (const ConstValInt* i_lhs = std::get_if<ConstValInt>(&expr_lhs)) {
      lhs = i_lhs->int_val;
    } else if (const ConstValBool* b_lhs = std::get_if<ConstValBool>(&expr_lhs)) {
      lhs = td::make_refint(b_lhs->bool_val ? -1 : 0);
    } 

    if (const ConstValInt* i_rhs = std::get_if<ConstValInt>(&expr_rhs)) {
      rhs = i_rhs->int_val;
    } else if (const ConstValBool* b_rhs = std::get_if<ConstValBool>(&expr_rhs)) {
      rhs = td::make_refint(b_rhs->bool_val ? -1 : 0);
    }

    if (lhs.is_null() || rhs.is_null()) {
      err("operator `{}` is used incorrectly in a constant expression", v->operator_name).fire(v);
    }

    switch (v->tok) {
      case tok_plus:          return ConstValInt{lhs + rhs};
      case tok_minus:         return ConstValInt{lhs - rhs};
      case tok_mul:           return ConstValInt{lhs * rhs};
      case tok_div:           return ConstValInt{lhs / rhs};
      case tok_mod:           return ConstValInt{lhs % rhs};
      case tok_bitwise_and:   return ConstValInt{lhs & rhs};
      case tok_bitwise_or:    return ConstValInt{lhs | rhs};
      case tok_bitwise_xor:   return ConstValInt{lhs ^ rhs};
      case tok_lshift:        return ConstValInt{lhs << static_cast<int>(rhs->to_long())};
      case tok_rshift:        return ConstValInt{lhs >> static_cast<int>(rhs->to_long())};
      case tok_logical_and:   return ConstValBool{lhs != 0 && rhs != 0};
      case tok_logical_or:    return ConstValBool{lhs != 0 || rhs != 0};
      case tok_gt:            return ConstValBool{td::cmp(lhs, rhs) >  0};
      case tok_geq:           return ConstValBool{td::cmp(lhs, rhs) >= 0};
      case tok_lt:            return ConstValBool{td::cmp(lhs, rhs) <  0};
      case tok_leq:           return ConstValBool{td::cmp(lhs, rhs) <= 0};
      case tok_eq:            return ConstValBool{td::cmp(lhs, rhs) == 0};
      case tok_neq:           return ConstValBool{td::cmp(lhs, rhs) != 0};
      default:
        err("operator `{}` is not allowed in a constant expression", v->operator_name).fire(v);
    }
  }

  // `lhs as <type>`; we allow `as` operator inside constants, but it's restricted not to change value's shape;
  // e.g., `5 as int8` or `Color.Red as int` is okay, but `5 as int|slice` is not
  static ConstValExpression handle_cast_as_operator(V<ast_cast_as_operator> v) {
    ConstValExpression val = eval_any_v_or_fire(v->get_expr());

    TypePtr l = v->get_expr()->inferred_type->unwrap_alias();
    TypePtr r = v->inferred_type->unwrap_alias();
    if (l->equal_to(r)) {
      return val;
    }

    bool lhs_is_int = l == TypeDataInt::create() || l == TypeDataCoins::create() || l->try_as<TypeDataIntN>() || l->try_as<TypeDataEnum>();
    bool rhs_is_int = r == TypeDataInt::create() || r == TypeDataCoins::create() || r->try_as<TypeDataIntN>() || r->try_as<TypeDataEnum>();
    if (lhs_is_int && rhs_is_int && std::holds_alternative<ConstValInt>(val)) {
      return val;
    }

    bool lhs_is_slice = l->try_as<TypeDataSlice>() || l->try_as<TypeDataBitsN>();
    bool rhs_is_slice = r->try_as<TypeDataSlice>() || r->try_as<TypeDataBitsN>();
    if (lhs_is_slice && rhs_is_slice && std::holds_alternative<ConstValSlice>(val)) {
      return val;
    }

    bool lhs_is_address = l->try_as<TypeDataAddress>();
    bool rhs_is_address = r->try_as<TypeDataAddress>();
    if (lhs_is_address && rhs_is_address && std::holds_alternative<ConstValAddress>(val)) {
      return val;
    }

    bool rhs_is_nullable = r->try_as<TypeDataUnion>() && r->try_as<TypeDataUnion>()->has_null();
    if (l == TypeDataNullLiteral::create() && rhs_is_nullable && std::holds_alternative<ConstValNullLiteral>(val)) {
      return val;
    }

    err("operator `as` to `{}` from `{}` can not be used in a constant expression", r, l).fire(v);
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
      ConstValExpression lhs = eval_any_v_or_fire(v->get_obj());
      ConstValTensor* lhs_tensor = std::get_if<ConstValTensor>(&lhs);
      if (!lhs_tensor || index_at < 0 || index_at >= static_cast<int>(lhs_tensor->items.size())) {
        err_not_a_constant_expression().fire(v);
      }
      return eval_any_v_or_fire(lhs_tensor->items[index_at]);
    }
    if (v->is_target_enum_member()) {       // Color.Red
      EnumDefPtr enum_ref = v->inferred_type->unwrap_alias()->try_as<TypeDataEnum>()->enum_ref;
      std::vector<td::RefInt256> enum_values = calculate_enum_members_with_values(enum_ref);
      td::RefInt256 member_value = enum_values[std::get<EnumMemberPtr>(v->target)->member_idx];
      return ConstValInt{std::move(member_value)};
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
      td::Slice str_slice = td::Slice(v_string->str_val.data(), v_string->str_val.size());
      return ConstValSlice{td::hex_encode(str_slice)};
    }
    if (auto v_par = v->try_as<ast_parenthesized_expression>()) {
      return eval_any_v_or_fire(v_par->get_expr());
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
      std::vector<AnyExprV> items;
      items.reserve(v_tensor->size());
      for (int i = 0; i < v_tensor->size(); ++i) {
        AnyExprV v_ith = v_tensor->get_item(i);
        check_expression_is_constant_or_fire(v_ith);
        items.emplace_back(v_ith);
      }
      return ConstValTensor{std::move(items)};
    }
    if (auto v_object = v->try_as<ast_object_literal>()) {
      V<ast_object_body> v_body = v_object->get_body();
      std::vector<std::pair<StructFieldPtr, AnyExprV>> fields;
      fields.reserve(v_body->size());
      for (int i = 0; i < v_body->size(); ++i) {
        AnyExprV field_init_val = v_body->get_field(i)->get_init_val(); 
        check_expression_is_constant_or_fire(field_init_val);
        fields.emplace_back(v_body->get_field(i)->field_ref, field_init_val);
      }
      return ConstValObject{v_object->struct_ref, std::move(fields)};
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

ConstValExpression eval_constant_expression_or_fire(AnyExprV v_expr) {
  // inline the most popular case
  if (auto v_int = v_expr->try_as<ast_int_const>()) {
    return ConstValInt{v_int->intval};
  }
  return ConstExpressionEvaluator::eval_any_v_or_fire(v_expr);  
}

ConstValExpression eval_and_cache_const_init_val(GlobalConstPtr const_ref) {
  auto it = computed_constants_cache.find(const_ref);
  if (it != computed_constants_cache.end()) {
    return it->second;
  }

  // constants initializers are not recursive (checked at inferring), so no stack guards here
  ConstValExpression v = ConstExpressionEvaluator::eval_any_v_or_fire(const_ref->init_value);
  computed_constants_cache[const_ref] = v;
  return v;
}

std::vector<td::RefInt256> calculate_enum_members_with_values(EnumDefPtr enum_ref) {
  static std::vector<EnumDefPtr> called_stack;

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
      ConstValExpression assigned = ConstExpressionEvaluator::eval_any_v_or_fire(member_ref->init_value);
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

// for just a string literal "asdf" in Tolk code, it's hex-encoded "61626364",
// and surrounded as `x{61626364}` into Fift output
std::string eval_string_const_standalone(AnyExprV v_string) {
  auto v = v_string->try_as<ast_string_const>();
  tolk_assert(v);
  td::Slice str_slice = td::Slice(v->str_val.data(), v->str_val.size());
  return td::hex_encode(str_slice);
}

// for `ton("0.05")` and similar compile-time only functions, we evaluate them in-place
// and push already evaluated expression to IR vars
ConstValExpression eval_call_to_compile_time_function(AnyExprV v_call) {
  auto v = v_call->try_as<ast_function_call>();
  tolk_assert(v && v->fun_maybe->is_compile_time_const_val());
  return parse_vertex_call_to_compile_time_function(v, v->fun_maybe->name);
}

} // namespace tolk
