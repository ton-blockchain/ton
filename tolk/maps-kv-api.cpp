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
#include "maps-kv-api.h"
#include "type-system.h"
#include "generics-helpers.h"
#include "pack-unpack-api.h"

/*
 *   `map<K, V>` is a high-level wrapper over TVM dictionaries.
 *   The compiler automatically constructs correct DICT asm instructions, takes care of packing slices, etc.
 *
 *   In practice, K is most likely intN or address, V is any serializable value.
 * If K is numeric, DICTI* or DICTU* instructions are used.
 * If K is address or bitsN, DICT* instructions are used (internal address assumed).
 * If K is complex, it's automatically packed into a slice, and DICT* instructions are used.
 *
 *   On writing, DICTSETB instructions (providing a builder). Later, if a value is constant (so that a slice pushed
 * into this builder is constant), it's replaced no DICTSET (not B) with a peephole.
 *   For writing `map<K, Cell<T>>`, DICTSETREF instruction is used, but ONLY for writing (not for getting!).
 * REF instructions are not used for getting to maintain the same external interface `MapLookupResult` and `MapEntry`
 * with `loadValue()` etc.
 */

namespace tolk {

enum class DictKeyKind {
  IntKey,         // DICTI*
  UIntKey,        // DICTU*
  SliceKey,       // DICT*
};

enum class DictValueKind {
  SliceValue,     // DICTSET
  BuilderValue,   // DICTSETB
  CellRefValue,   // DICTSETREF
};


// map<int32, V> should use DICTI* instructions
// note that `struct UserId { v: int32 }` also optimized, since it's just a signed int on a stack
static int is_TKey_TVM_int(TypePtr TKey) {
  if (const TypeDataIntN* t_intN = TKey->try_as<TypeDataIntN>(); t_intN && !t_intN->is_variadic && !t_intN->is_unsigned) {
    return t_intN->n_bits;
  }
  if (const TypeDataAlias* t_alias = TKey->try_as<TypeDataAlias>()) {
    return is_TKey_TVM_int(t_alias->underlying_type);
  }
  if (const TypeDataStruct* t_struct = TKey->try_as<TypeDataStruct>(); t_struct && t_struct->struct_ref->get_num_fields() == 1) {
    return is_TKey_TVM_int(t_struct->struct_ref->get_field(0)->declared_type);
  }
  if (TKey == TypeDataBool::create()) {   // allow `bool` as a key with `DICTI` instructions
    return true;
  }
  return 0;
}

// map<uint32, V> should use DICTU* instructions
static int is_TKey_TVM_uint(TypePtr TKey) {
  if (const TypeDataIntN* t_intN = TKey->try_as<TypeDataIntN>(); t_intN && !t_intN->is_variadic && t_intN->is_unsigned) {
    return t_intN->n_bits;
  }
  if (const TypeDataAlias* t_alias = TKey->try_as<TypeDataAlias>()) {
    return is_TKey_TVM_uint(t_alias->underlying_type);
  }
  if (const TypeDataStruct* t_struct = TKey->try_as<TypeDataStruct>(); t_struct && t_struct->struct_ref->get_num_fields() == 1) {
    return is_TKey_TVM_uint(t_struct->struct_ref->get_field(0)->declared_type);
  }
  return 0;
}

// map<address, V> should use DICT* instructions
// note that map<slice, V> is forbidden, since a raw slice doesn't define binary width
static int is_TKey_TVM_slice(TypePtr TKey) {
  if (const TypeDataAddress* t_address = TKey->try_as<TypeDataAddress>()) {
    return t_address->is_internal() ? 3 + 8 + 256 : 0;
  }
  if (const TypeDataBitsN* t_bitsN = TKey->try_as<TypeDataBitsN>()) {
    return t_bitsN->is_bits ? t_bitsN->n_width : t_bitsN->n_width * 8;
  }
  if (const TypeDataAlias* t_alias = TKey->try_as<TypeDataAlias>()) {
    return is_TKey_TVM_slice(t_alias->underlying_type);
  }
  if (const TypeDataStruct* t_struct = TKey->try_as<TypeDataStruct>(); t_struct && t_struct->struct_ref->get_num_fields() == 1) {
    return is_TKey_TVM_slice(t_struct->struct_ref->get_field(0)->declared_type);
  }
  return 0;
}

// we allow `map<K, slice>` and handle it separately, because we don't need to unpack it
static bool is_TValue_raw_slice(TypePtr TValue) {
  return TValue->unwrap_alias() == TypeDataSlice::create();
}

// `map<K, Cell<T>>` can emit SETREF instructions
static bool is_TValue_cell_or_CellT(TypePtr TValue) {
  return TValue->unwrap_alias() == TypeDataCell::create() || is_type_cellT(TValue->unwrap_alias());  
}

bool check_mapKV_TKey_is_valid(TypePtr TKey, std::string& because_msg) {
  if (is_TKey_TVM_int(TKey) || is_TKey_TVM_uint(TKey) || is_TKey_TVM_slice(TKey)) {
    return true;
  }

  // okay, not a trivial key — it must be a serializable struct of a constant size 
  if (TKey->unwrap_alias() == TypeDataSlice::create()) {  // a dedicated error message for `map<slice, V>`
    because_msg = "because it does not specify keyLen for a dictionary\n""hint: use `address` if a key is an internal address\n""hint: use `bits128` and similar if a key represents fixed-width data";
    return false;
  }
  if (!check_struct_can_be_packed_or_unpacked(TKey, false, because_msg)) {
    because_msg = "because it can not be serialized to slice\n" + because_msg;
    return false;
  }

  PackSize pack_size = estimate_serialization_size(TKey);
  if (pack_size.min_bits != pack_size.max_bits) {
    because_msg += "because its binary size is not constant: it's " + std::to_string(pack_size.min_bits) + ".." + std::to_string(pack_size.max_bits) + " bits";
    return false;
  }
  if (pack_size.min_bits > 1023) {
    because_msg += "because its binary size is too large: " + std::to_string(pack_size.min_bits) + " bits";
    return false;
  }
  if (pack_size.max_refs) {
    because_msg += "because it may contain a cell reference, not only data bits";
    return false;
  }
  return true;
}

bool check_mapKV_TValue_is_valid(TypePtr TValue, std::string& because_msg) {
  // we allow `slice` and `RemainingBitsAndRefs` as a value
  if (is_TValue_raw_slice(TValue)) {
    return true;
  }
  // or something that can be packed to/from slice
  if (!check_struct_can_be_packed_or_unpacked(TValue, false, because_msg)) {
    because_msg = "because it can not be serialized\n" + because_msg;
    return false;
  }
  // note that `struct A { s: slice }` can not be used as a value (not serializable),
  // although `slice` can, because in stdlib behavior for TValue=slice is overloaded (no deserialization)
  
  return true;
}


// an internal helper, having TKey and TValue, generate IR variables passed to __dict.* built-in functions
class DictKeyValue {
  CodeBlob& code;
  AnyV origin;

  DictKeyKind key_kind;
  int key_len;
  var_idx_t key_irv = -1;

  DictValueKind value_kind;
  var_idx_t value_irv = -1;

public:

  var_idx_t ir_key_kind() const {
    return code.create_int(origin, static_cast<int>(key_kind), "(key-kind)");
  }

  var_idx_t ir_value_kind() const {
    return code.create_int(origin, static_cast<int>(value_kind), "(value-kind)");
  }

  var_idx_t ir_key_len() const {
    return code.create_int(origin, key_len, "(key-len)");
  }

  var_idx_t ir_key_val() const {
    tolk_assert(key_irv != -1);
    return key_irv;
  }

  var_idx_t ir_value_val() const {
    tolk_assert(value_irv != -1);
    return value_irv;
  }

  DictKeyValue(CodeBlob& code, AnyV origin, TypePtr TKey, const std::vector<var_idx_t>* exact_key, TypePtr TValue, const std::vector<var_idx_t>* exact_value, bool allow_REF_TValue = false)
    : code(code)
    , origin(origin) {
    if (int i_bits = is_TKey_TVM_int(TKey)) {
      key_kind = DictKeyKind::IntKey;
      key_len = i_bits;
      if (exact_key != nullptr) {
        tolk_assert(exact_key->size() == 1);
        key_irv = exact_key->at(0);
      }
    } else if (int u_bits = is_TKey_TVM_uint(TKey)) {
      key_kind = DictKeyKind::UIntKey;
      key_len = u_bits;
      if (exact_key != nullptr) {
        tolk_assert(exact_key->size() == 1);
        key_irv = exact_key->at(0);
      }
    } else if (int s_bits = is_TKey_TVM_slice(TKey)) {
      key_kind = DictKeyKind::SliceKey;
      key_len = s_bits;
      if (exact_key != nullptr) {
        tolk_assert(exact_key->size() == 1);
        key_irv = exact_key->at(0);
      }
    } else {
      key_kind = DictKeyKind::SliceKey;
      PackSize pack_size = EstimateContext().estimate_any(TKey);
      tolk_assert(pack_size.max_refs == 0 && pack_size.min_bits == pack_size.max_bits);
      key_len = pack_size.max_bits;

      if (exact_key != nullptr) {
        std::vector ir_builder = code.create_tmp_var(TypeDataBuilder::create(), origin, "(map-keyB)");
        code.emplace_back(origin, Op::_Call, ir_builder, std::vector<var_idx_t>{}, lookup_function("beginCell"));
        PackContext ctx(code, origin, ir_builder, create_default_PackOptions(code, origin));
        ctx.generate_pack_any(TKey, std::vector(*exact_key));
        std::vector ir_slice = code.create_tmp_var(TypeDataSlice::create(), origin, "(map-key)");
        code.emplace_back(origin, Op::_Call, ir_slice, ir_builder, lookup_function("builder.toSlice"));
        key_irv = ir_slice[0];
      }
    }

    if (is_TValue_raw_slice(TValue)) {
      value_kind = DictValueKind::SliceValue;
      if (exact_value != nullptr) {
        value_irv = exact_value->at(0);
      }
    } else if (allow_REF_TValue && is_TValue_cell_or_CellT(TValue)) {
      // note that we use CellRefValue for writing only (not for reading, not for "set+get"):
      // we don't emit REF for getters to match typing of MapLookupResult and MapEntry,
      // so that `loadValue()` implemented in stdlib works universally for any V (particularly, Cell<V>)
      // (given `map<K, cell>.get`, DICTGET will be emitted, and loadValue() will load a ref correctly)
      value_kind = DictValueKind::CellRefValue;
      if (exact_value != nullptr) {
        value_irv = exact_value->at(0);
      }
    } else {
      value_kind = DictValueKind::BuilderValue;

      if (exact_value != nullptr) {
        std::vector ir_builder = code.create_tmp_var(TypeDataBuilder::create(), origin, "(valueB)");
        code.emplace_back(origin, Op::_Call, ir_builder, std::vector<var_idx_t>{}, lookup_function("beginCell"));
        PackContext ctx(code, origin, ir_builder, create_default_PackOptions(code, origin));
        ctx.generate_pack_any(TValue, std::vector(*exact_value));
        value_irv = ir_builder[0];
      }
    }
  }
};

// MapEntry<K, V> is a built-in struct { rawValue: slice, key: K, isFound: bool }
// when used for numeric K, tvm instructions DICTI* and DICTU* return an integer key onto the stack
// when used for address/bitsN, tvm instructions DICT* return a slice key onto the stack
// so, in practice, we don't need any transformations from a TVM result,
// but when K is complex (like struct Point), TVM instructions return a slice, which is needed to be unpacked to K
GNU_ATTRIBUTE_NOINLINE
static std::vector<var_idx_t> construct_MapEntry_with_non_trivial_key(CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& ir_entry, TypePtr TKey) {
  tolk_assert(ir_entry.size() == 3); // slice value, slice key, isFound

  std::vector ir_key = code.create_tmp_var(TKey, origin, "(entry-key)");
  Op& if_found = code.emplace_back(origin, Op::_If, std::vector{ir_entry[2]});
  {
    code.push_set_cur(if_found.block0);
    UnpackContext ctx(code, origin, std::vector(ir_entry.begin() + 1, ir_entry.begin() + 2), create_default_UnpackOptions(code, origin));
    std::vector ir_unpacked_key = ctx.generate_unpack_any(TKey);
    code.emplace_back(origin, Op::_Let, ir_key, std::move(ir_unpacked_key));
    code.close_pop_cur(origin);
  }
  {
    code.push_set_cur(if_found.block1);
    for (var_idx_t ith_null : ir_key) {
      code.emplace_back(origin, Op::_Let, std::vector{ith_null}, std::vector{ir_entry[1]});
    }
    code.close_pop_cur(origin);
  }

  std::vector<var_idx_t> ir_result;
  ir_result.reserve(2 + ir_key.size());
  ir_result.push_back(ir_entry[0]);   // rawSlice
  ir_result.insert(ir_result.end(), ir_key.begin(), ir_key.end());
  ir_result.push_back(ir_entry[2]);   // isFound
  return ir_result;
}

static std::vector<var_idx_t> create_ir_MapEntry(CodeBlob& code, AnyV origin) {
  return code.create_tmp_var(TypeDataTensor::create({TypeDataSlice::create(), TypeDataInt::create(), TypeDataInt::create()}), origin, "(entry)");
}

// see a comment above construct_MapEntry_with_non_trivial_key() 
static std::vector<var_idx_t> finalize_ir_MapEntry(CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& ir_entry, TypePtr TKey) {
  if (!is_TKey_TVM_int(TKey) && !is_TKey_TVM_uint(TKey) && !is_TKey_TVM_slice(TKey)) {
    ir_entry = construct_MapEntry_with_non_trivial_key(code, origin, std::move(ir_entry), TKey);
  }
  return ir_entry;
}


// ----------------------------------
// generating AsmOp and IR code
//


// "DICTSET" -> "DICTISETB" having key_kind and value_kind
static std::string choose_dict_op(std::string_view op_slice, VarDescr& var_with_key_kind, VarDescr& var_with_value_kind) {
  std::string op(op_slice);

  DictKeyKind key_kind = static_cast<DictKeyKind>(var_with_key_kind.int_const->to_long());
  var_with_key_kind.unused();
  
  if (key_kind == DictKeyKind::UIntKey) op = "DICTU" + op.substr(4);
  if (key_kind == DictKeyKind::IntKey)  op = "DICTI" + op.substr(4);

  DictValueKind value_kind = static_cast<DictValueKind>(var_with_value_kind.int_const->to_long());
  var_with_value_kind.unused();

  if (value_kind == DictValueKind::BuilderValue) op += "B";
  if (value_kind == DictValueKind::CellRefValue) op += "REF";

  return op;
}

// "empty map" is just NULL in TVM; it's extracted as a built-in to check for K/V correctness in advance
AsmOp compile_createEmptyMap(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.empty());
  return AsmOp::Custom(origin, "NEWDICT", 0, 1);
}

// "convert dict to map" is just NOP; it's extracted as a built-in to allow non-1 width K/V
AsmOp compile_createMapFromLowLevelDict(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 1);
  return AsmOp::Parse(origin, "NOP");
}

// DICTGET: k D n => (x −1) OR (0); + NULLSWAPIFNOT => (x -1) OR (null 0)
AsmOp compile_dict_get(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 2 && args.size() == 2+3);
  std::string op = choose_dict_op("DICTGET", args[0], args[1]);
  return AsmOp::Custom(origin, op + " NULLSWAPIFNOT", 3, 2);
}

// DICTMIN: D n => (x k −1) OR (0); + NULLSWAPIFNOT2 => (x k -1) OR (null null 0)
AsmOp compile_dict_getMin(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 3 && args.size() == 2+2);
  std::string op = choose_dict_op("DICTMIN", args[0], args[1]);
  return AsmOp::Custom(origin, op + " NULLSWAPIFNOT2", 2, 3);
}

// DICTMAX: D n => (x k −1) OR (0); + NULLSWAPIFNOT2 => (x k -1) OR (null null 0)
AsmOp compile_dict_getMax(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 3 && args.size() == 2+2);
  std::string op = choose_dict_op("DICTMAX", args[0], args[1]);
  return AsmOp::Custom(origin, op + " NULLSWAPIFNOT2", 2, 3);
}

// DICTGETNEXT: k D n => (x k −1) OR (0); + NULLSWAPIFNOT2 => (x k -1) OR (null null 0)
AsmOp compile_dict_getNext(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 3 && args.size() == 2+3);
  std::string op = choose_dict_op("DICTGETNEXT", args[0], args[1]);
  return AsmOp::Custom(origin, op + " NULLSWAPIFNOT2", 3, 3);
}

// DICTGETNEXTEQ: k D n => (x k −1) OR (0); + NULLSWAPIFNOT2 => (x k -1) OR (null null 0)
AsmOp compile_dict_getNextEq(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 3 && args.size() == 2+3);
  std::string op = choose_dict_op("DICTGETNEXTEQ", args[0], args[1]);
  return AsmOp::Custom(origin, op + " NULLSWAPIFNOT2", 3, 3);
}

// DICTGETPREV: k D n => (x k −1) OR (0); + NULLSWAPIFNOT2 => (x k -1) OR (null null 0)
AsmOp compile_dict_getPrev(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 3 && args.size() == 2+3);
  std::string op = choose_dict_op("DICTGETPREV", args[0], args[1]);
  return AsmOp::Custom(origin, op + " NULLSWAPIFNOT2", 3, 3);
}

// DICTGETPREVEQ: k D n => (x k −1) OR (0); + NULLSWAPIFNOT2 => (x k -1) OR (null null 0)
AsmOp compile_dict_getPrevEq(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 3 && args.size() == 2+3);
  std::string op = choose_dict_op("DICTGETPREVEQ", args[0], args[1]);
  return AsmOp::Custom(origin, op + " NULLSWAPIFNOT2", 3, 3);
}

// DICTSET: x k D n => D'
AsmOp compile_dict_set(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 2+4);
  std::string op = choose_dict_op("DICTSET", args[0], args[1]);
  return AsmOp::Custom(origin, op, 4, 1);
}

// DICTSETGET: x k D n => (D' y −1) or (D' 0); + NULLSWAPIFNOT => (D' y -1) OR (D' null 0)
AsmOp compile_dict_setGet(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 3 && args.size() == 2+4);
  std::string op = choose_dict_op("DICTSETGET", args[0], args[1]);
  return AsmOp::Custom(origin, op + " NULLSWAPIFNOT", 4, 3);
}

// DICTREPLACE: x k D n => (D' -1) OR (D 0)
AsmOp compile_dict_replace(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 2 && args.size() == 2+4);
  std::string op = choose_dict_op("DICTREPLACE", args[0], args[1]);
  return AsmOp::Custom(origin, op, 4, 2);
}

// DICTREPLACEGET: x k D n => (D' y -1) OR (D 0); + NULLSWAPIFNOT => (D' y -1) OR (D null 0)
AsmOp compile_dict_replaceGet(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 3 && args.size() == 2+4);
  std::string op = choose_dict_op("DICTREPLACEGET", args[0], args[1]);
  return AsmOp::Custom(origin, op + " NULLSWAPIFNOT", 4, 3);
}

// DICTADD: x k D n => (D' -1) OR (D 0)
AsmOp compile_dict_add(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 2 && args.size() == 2+4);
  std::string op = choose_dict_op("DICTADD", args[0], args[1]);
  return AsmOp::Custom(origin, op, 4, 2);
}

// DICTADDGET: x k D n => (D' -1) OR (D y 0); + NULLSWAPIF + NOT => (D' null 0) OR (D y -1) (from "isAdded" to "isFound")
AsmOp compile_dict_addGet(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 3 && args.size() == 2+4);
  std::string op = choose_dict_op("DICTADDGET", args[0], args[1]);
  return AsmOp::Custom(origin, op + " NULLSWAPIF" + " NOT", 4, 3);
}

// DICTDEL: k D n => (D' -1) OR (D 0)
AsmOp compile_dict_del(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 2 && args.size() == 2+3);
  std::string op = choose_dict_op("DICTDEL", args[0], args[1]);
  return AsmOp::Custom(origin, op, 3, 2);
}

// DICTDELGET: k D n => (D' x -1) OR (D 0); + NULLSWAPIFNOT => (D' x -1) OR (D null 0)
AsmOp compile_dict_delGet(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 3 && args.size() == 2+3);
  std::string op = choose_dict_op("DICTDELGET", args[0], args[1]);
  return AsmOp::Custom(origin, op + " NULLSWAPIFNOT", 3, 3);
}


// fun map<K,V>.exists(self, key: K): bool
std::vector<var_idx_t> generate_mapKV_exists(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  DictKeyValue kv(code, origin, TKey, &args[1], TypeDataSlice::create(), nullptr);

  std::vector ir_lookup = code.create_tmp_var(TypeDataTensor::create({TypeDataSlice::create(), TypeDataInt::create()}), origin, "(lookup)");
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_lookup, std::move(dict_args), lookup_function("__dict.get"));

  return {ir_lookup[1]};    // isFound from (sliceOrNull isFound)
}

// fun map<K,V>.get(self, key: K): MapLookupResult<V>
std::vector<var_idx_t> generate_mapKV_get(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  DictKeyValue kv(code, origin, TKey, &args[1], TypeDataSlice::create(), nullptr);

  std::vector ir_lookup = code.create_tmp_var(TypeDataTensor::create({TypeDataSlice::create(), TypeDataInt::create()}), origin, "(lookup)");
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_lookup, std::move(dict_args), lookup_function("__dict.get"));

  // in all functions where we return MapLookupResult:
  // on a stack we have (slice, found) - exactly the shape of MapLookupResult<TValue>;
  // the user manually calls `result.loadValue()` after checking result.isFound
  return ir_lookup;
}

// fun map<K,V>.mustGet(self, key: K, throwIfNotFound: int = 9): V
std::vector<var_idx_t> generate_mapKV_mustGet(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  TypePtr TValue = called_f->substitutedTs->typeT_at(1);
  // since we don't return MapLookupResult, for `map<K, Cell<T>>` we can use DICTGETREF
  bool use_DICTGETREF = is_TValue_cell_or_CellT(TValue);
  DictKeyValue kv(code, origin, TKey, &args[1], use_DICTGETREF ? TValue : TypeDataSlice::create(), nullptr, true);

  std::vector ir_lookup = code.create_tmp_var(TypeDataTensor::create({TypeDataSlice::create(), TypeDataInt::create()}), origin, "(lookup)");
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_lookup, std::move(dict_args), lookup_function("__dict.get"));

  std::vector args_throwifnot = { args[2][0], ir_lookup[1] };
  Op& op_assert = code.emplace_back(origin, Op::_Call, std::vector<var_idx_t>{}, std::move(args_throwifnot), lookup_function("__throw_ifnot"));
  op_assert.set_impure_flag();
  // later on, preceding `NULLSWAPIFNOT` will be removed if possible by a peephole optimization

  std::vector ir_slice(ir_lookup.begin(), ir_lookup.begin() + 1);
  if (is_TValue_raw_slice(TValue)) {
    return ir_slice;
  }
  if (use_DICTGETREF) {    // ir_slice holds a cell actually
    return ir_slice;       // (it's exactly what we need, we need to return Cell<T>)
  }

  // load TValue and check for assertEnd (it's the default behavior)
  UnpackContext ctx(code, origin, std::move(ir_slice), create_default_UnpackOptions(code, origin));
  std::vector ir_value = ctx.generate_unpack_any(TValue);
  ctx.assertEndIfOption();
  return ir_value;
}

// fun map<K,V>.set(mutate self, key: K, value: V): self
std::vector<var_idx_t> generate_mapKV_set(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  TypePtr TValue = called_f->substitutedTs->typeT_at(1);
  DictKeyValue kv(code, origin, TKey, &args[1], TValue, &args[2], true);

  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_value_val(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, args[0], std::move(dict_args), lookup_function("__dict.set"));

  return args[0];   // return mutated map
}

// fun map<K,V>.setAndGetPrevious(mutate self, key: K, value: V): MapLookupResult<V>
std::vector<var_idx_t> generate_mapKV_setGet(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  TypePtr TValue = called_f->substitutedTs->typeT_at(1);
  DictKeyValue kv(code, origin, TKey, &args[1], TValue, &args[2]);

  std::vector ir_map_and_lookup = code.create_tmp_var(TypeDataTensor::create({TypeDataCell::create(), TypeDataSlice::create(), TypeDataInt::create()}), origin, "(map-and-lookup)");
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_value_val(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_map_and_lookup, std::move(dict_args), lookup_function("__dict.setGet"));

  return ir_map_and_lookup;    
}

// fun map<K,V>.replaceIfExists(mutate self, key: K, value: V): bool
std::vector<var_idx_t> generate_mapKV_replace(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  TypePtr TValue = called_f->substitutedTs->typeT_at(1);
  DictKeyValue kv(code, origin, TKey, &args[1], TValue, &args[2], true);

  std::vector ir_map_and_was_added = code.create_tmp_var(TypeDataTensor::create({TypeDataCell::create(), TypeDataBool::create()}), origin, "(map-and-was-added)");
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_value_val(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_map_and_was_added, std::move(dict_args), lookup_function("__dict.replace"));

  return ir_map_and_was_added;
}

// fun map<K,V>.replaceAndGetPrevious(mutate self, key: K, value: V): MapLookupResult<V>
std::vector<var_idx_t> generate_mapKV_replaceGet(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  TypePtr TValue = called_f->substitutedTs->typeT_at(1);
  DictKeyValue kv(code, origin, TKey, &args[1], TValue, &args[2]);

  std::vector ir_map_and_lookup = code.create_tmp_var(TypeDataTensor::create({TypeDataCell::create(), TypeDataSlice::create(), TypeDataInt::create()}), origin, "(map-and-lookup)");
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_value_val(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_map_and_lookup, std::move(dict_args), lookup_function("__dict.replaceGet"));

  return ir_map_and_lookup;
}

// fun map<K,V>.addIfNotExists(mutate self, key: K, value: V): bool
std::vector<var_idx_t> generate_mapKV_add(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  TypePtr TValue = called_f->substitutedTs->typeT_at(1);
  DictKeyValue kv(code, origin, TKey, &args[1], TValue, &args[2], true);

  std::vector ir_map_and_was_added = code.create_tmp_var(TypeDataTensor::create({TypeDataCell::create(), TypeDataBool::create()}), origin, "(map-and-was-added)");
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_value_val(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_map_and_was_added, std::move(dict_args), lookup_function("__dict.add"));

  return ir_map_and_was_added;
}

// fun map<K,V>.addOrGetExisting(mutate self, key: K, value: V): MapLookupResult<V>
std::vector<var_idx_t> generate_mapKV_addGet(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  TypePtr TValue = called_f->substitutedTs->typeT_at(1);
  DictKeyValue kv(code, origin, TKey, &args[1], TValue, &args[2]);

  std::vector ir_map_and_lookup = code.create_tmp_var(TypeDataTensor::create({TypeDataCell::create(), TypeDataSlice::create(), TypeDataInt::create()}), origin, "(lookup)");
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_value_val(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_map_and_lookup, std::move(dict_args), lookup_function("__dict.addGet"));

  return ir_map_and_lookup;
}

// fun map<K, V>.delete(mutate self, key: K): bool
std::vector<var_idx_t> generate_mapKV_del(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  DictKeyValue kv(code, origin, TKey, &args[1], TypeDataSlice::create(), nullptr);

  std::vector ir_map_and_was_deleted = code.create_tmp_var(TypeDataTensor::create({TypeDataCell::create(), TypeDataBool::create()}), origin, "(map-and-was-added)");
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_map_and_was_deleted, std::move(dict_args), lookup_function("__dict.del"));

  return ir_map_and_was_deleted;
}

// fun map<K,V>.deleteAndGetDeleted(mutate self, key: K): MapLookupResult<V>
std::vector<var_idx_t> generate_mapKV_delGet(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  DictKeyValue kv(code, origin, TKey, &args[1], TypeDataSlice::create(), nullptr);

  std::vector ir_map_and_lookup = code.create_tmp_var(TypeDataTensor::create({TypeDataCell::create(), TypeDataSlice::create(), TypeDataInt::create()}), origin, "(lookup)");
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_map_and_lookup, std::move(dict_args), lookup_function("__dict.delGet"));

  return ir_map_and_lookup;
}

// fun map<K,V>.findFirst(): MapEntry<K, V>
std::vector<var_idx_t> generate_mapKV_findFirst(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  DictKeyValue kv(code, origin, TKey, nullptr, TypeDataSlice::create(), nullptr);
  
  // on a stack (ir_entry) we will have: either (x k −1) or (null null 0)
  std::vector ir_entry = create_ir_MapEntry(code, origin);
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_entry, std::move(dict_args), lookup_function("__dict.getMin"));

  return finalize_ir_MapEntry(code, origin, std::move(ir_entry), TKey);
}

// fun map<K,V>.findLast(): MapEntry<K, V>
std::vector<var_idx_t> generate_mapKV_findLast(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  DictKeyValue kv(code, origin, TKey, nullptr, TypeDataSlice::create(), nullptr);
  
  std::vector ir_entry = create_ir_MapEntry(code, origin);
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_entry, std::move(dict_args), lookup_function("__dict.getMax"));

  return finalize_ir_MapEntry(code, origin, std::move(ir_entry), TKey);
}

// fun map<K,V>.findKeyGreater(pivotKey: K): MapEntry<K, V>
std::vector<var_idx_t> generate_mapKV_findKeyGreater(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  DictKeyValue kv(code, origin, TKey, &args[1], TypeDataSlice::create(), nullptr);
  
  std::vector ir_entry = create_ir_MapEntry(code, origin);
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_entry, std::move(dict_args), lookup_function("__dict.getNext"));

  return finalize_ir_MapEntry(code, origin, std::move(ir_entry), TKey);
}

// fun map<K,V>.findKeyGreaterOrEqual(pivotKey: K): MapEntry<K, V>
std::vector<var_idx_t> generate_mapKV_findKeyGreaterOrEqual(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  DictKeyValue kv(code, origin, TKey, &args[1], TypeDataSlice::create(), nullptr);
  
  std::vector ir_entry = create_ir_MapEntry(code, origin);
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_entry, std::move(dict_args), lookup_function("__dict.getNextEq"));

  return finalize_ir_MapEntry(code, origin, std::move(ir_entry), TKey);
}

// fun map<K,V>.findKeyLess(pivotKey: K): MapEntry<K, V>
std::vector<var_idx_t> generate_mapKV_findKeyLess(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  DictKeyValue kv(code, origin, TKey, &args[1], TypeDataSlice::create(), nullptr);
  
  std::vector ir_entry = create_ir_MapEntry(code, origin);
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_entry, std::move(dict_args), lookup_function("__dict.getPrev"));

  return finalize_ir_MapEntry(code, origin, std::move(ir_entry), TKey);
}

// fun map<K,V>.findKeyLessOrEqual(pivotKey: K): MapEntry<K, V>
std::vector<var_idx_t> generate_mapKV_findKeyLessOrEqual(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  DictKeyValue kv(code, origin, TKey, &args[1], TypeDataSlice::create(), nullptr);
  
  std::vector ir_entry = create_ir_MapEntry(code, origin);
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_entry, std::move(dict_args), lookup_function("__dict.getPrevEq"));

  return finalize_ir_MapEntry(code, origin, std::move(ir_entry), TKey);
}

// fun map<K,V>.iterateNext(current: MapEntry<K, V>): MapEntry<K, V>
std::vector<var_idx_t> generate_mapKV_iterateNext(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  std::vector ir_pivot_key(args[1].begin() + 1, args[1].end() - 1);
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  DictKeyValue kv(code, origin, TKey, &ir_pivot_key, TypeDataSlice::create(), nullptr);
  
  std::vector ir_entry = create_ir_MapEntry(code, origin);
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_entry, std::move(dict_args), lookup_function("__dict.getNext"));

  return finalize_ir_MapEntry(code, origin, std::move(ir_entry), TKey);
}

// fun map<K,V>.iteratePrev(current: MapEntry<K, V>): MapEntry<K, V>
std::vector<var_idx_t> generate_mapKV_iteratePrev(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  std::vector ir_pivot_key(args[1].begin() + 1, args[1].end() - 1);
  TypePtr TKey = called_f->substitutedTs->typeT_at(0);
  DictKeyValue kv(code, origin, TKey, &ir_pivot_key, TypeDataSlice::create(), nullptr);
  
  std::vector ir_entry = create_ir_MapEntry(code, origin);
  std::vector dict_args = { kv.ir_key_kind(), kv.ir_value_kind(), kv.ir_key_val(), args[0][0], kv.ir_key_len() };
  code.emplace_back(origin, Op::_Call, ir_entry, std::move(dict_args), lookup_function("__dict.getPrev"));

  return finalize_ir_MapEntry(code, origin, std::move(ir_entry), TKey);
}

} // namespace tolk
