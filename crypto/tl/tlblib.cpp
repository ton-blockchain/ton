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
#include "td/utils/base64.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/utf8.h"
#include "vm/cells/Cell.h"
#include "vm/cells/CellBuilder.h"
#include "vm/excno.hpp"
#include "tl/tlblib.hpp"
#include <iomanip>


#include "vm/dict.h"

namespace tlb {

const False t_False;
const True t_True;
const Unit t_Unit;

const Bool t_Bool;

const Int t_int8{8}, t_int16{16}, t_int24{24}, t_int32{32}, t_int64{64}, t_int128{128}, t_int256{256}, t_int257{257};
const UInt t_uint8{8}, t_uint16{16}, t_uint24{24}, t_uint32{32}, t_uint64{64}, t_uint128{128}, t_uint256{256};
const NatWidth t_Nat{32};

const Anything t_Anything;
const RefAnything t_RefCell;

const SnakeString t_SnakeString;

std::string TLB::get_type_name() const {
  std::ostringstream os;
  print_type(os);
  return os.str();
}

bool Bool::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  int t = get_tag(cs);
  return cs.advance(1) && pp.out(t ? "bool_true" : "bool_false");
}

bool Bool::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  int t = get_tag(cs);
  return cs.advance(1) && pp.out(t ? "bool_true" : "bool_false");
}

bool NatWidth::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  long long value = (long long)cs.fetch_ulong(n);
  return value >= 0 && pp.out_int(value);
}

bool NatLeq::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  long long value = (long long)as_uint(cs);
  return value >= 0 && skip(cs) && pp.out_int(value);
}

bool NatLess::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  long long value = (long long)as_uint(cs);
  return value >= 0 && skip(cs) && pp.out_int(value);
}

bool TupleT::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  pp.open("tuple ");
  pp.os << n << " [";
  pp.mode_nl();
  int i = n;
  for (; i > 0; --i) {
    if (!X.print_skip(pp, cs)) {
      return false;
    }
    pp.mode_nl();
  }
  return pp.close("]");
}

bool TupleT::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  pp.open("tuple");
  for (int i = 0; i < n; i++) {
    if (!pp.open(std::to_string(i)) ||
        !X.print_skip(pp, cs) ||
        !pp.close()) {
      return false;
    }
  }
  return pp.close();
}

bool CondT::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  return (n > 0 ? X.print_skip(pp, cs) : (!n && pp.out("()")));
}

bool CondT::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  return (n > 0 ? X.print_skip(pp, cs) : (!n && pp.out("()")));
}

bool Int::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  if (n <= 64) {
    long long value;
    return cs.fetch_int_to(n, value) && pp.out_int(value);
  } else {
    return pp.out_integer(cs.fetch_int256(n, true));
  }
}

bool Int::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  if (n <= 64) {
    long long value;
    return cs.fetch_int_to(n, value) && pp.out_int(value);
  } else {
    return pp.out_integer(cs.fetch_int256(n, true));
  }
}

bool UInt::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  if (n <= 64) {
    unsigned long long value;
    return cs.fetch_uint_to(n, value) && pp.out_uint(value);
  } else {
    return pp.out_integer(cs.fetch_int256(n, false));
  }
}

bool UInt::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  if (n <= 64) {
    unsigned long long value;
    return cs.fetch_uint_to(n, value) && pp.out_uint(value);
  } else {
    return pp.out_integer(cs.fetch_int256(n, false));
  }
}

bool Bits::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  if (cs.have(n)) {
    pp.os << 'x' << cs.fetch_bits(n).to_hex();
    return true;
  } else {
    return false;
  }
}

bool Bits::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  if (cs.have(n)) {
    return pp.out(cs.fetch_bits(n).to_hex());
  } else {
    return false;
  }
}

namespace {
// The snake occupies the remaining bytes and optional single reference in its
// root slice. Validate children without replacing the caller's root cursor.
bool consume_snake(vm::CellSlice& root, int* ops, std::vector<unsigned char>* bytes) {
  try {
    vm::CellSlice current{root};
    for (int cells = 0; cells < 1000; ++cells) {
      if (!current.is_valid() || current.is_special() || current.size() % 8 != 0 || current.size_refs() > 1) {
        return false;
      }
      if (bytes) {
        while (current.size() > 0) {
          // fetch_ulong advances the slice cursor; fetch_octet only advances its bit cache.
          const auto octet = current.fetch_ulong(8);
          if (octet == vm::CellSlice::fetch_ulong_eof) {
            return false;
          }
          bytes->push_back(static_cast<unsigned char>(octet));
        }
      } else if (!current.advance(current.size())) {
        return false;
      }
      if (current.size_refs() == 0) {
        vm::CellSlice finished{root};
        if (!finished.advance(finished.size()) || (finished.size_refs() && finished.fetch_ref().is_null())) {
          return false;
        }
        root = std::move(finished);
        return true;
      }
      // A validate_ref caller has already charged for the root. Inline fields
      // likewise share their parent's cell; only following a ref visits a new cell.
      if (ops) {
        if (*ops <= 0) {
          return false;
        }
        --*ops;
      }
      auto ref = current.fetch_ref();
      if (ref.is_null() || !current.load(vm::NoVm{}, std::move(ref))) {
        return false;
      }
    }
  } catch (vm::VmError&) {
    return false;
  } catch (vm::VmVirtError&) {
    return false;
  } catch (vm::VmNoGas&) {
    return false;
  }
  return false;
}
}  // namespace

int SnakeString::get_size(const vm::CellSlice& cs) const {
  vm::CellSlice copy{cs};
  return skip(copy) ? copy.subtract_base_ext(cs) : -1;
}

bool SnakeString::skip(vm::CellSlice& cs) const {
  return consume_snake(cs, nullptr, nullptr);
}

bool SnakeString::validate_skip(int* ops, vm::CellSlice& cs, bool) const {
  return consume_snake(cs, ops, nullptr);
}

td::Result<std::vector<unsigned char>> SnakeString::load_snake_binary(vm::CellSlice& cs) const {
  std::vector<unsigned char> data;
  if (!consume_snake(cs, nullptr, &data)) {
    return td::Status::Error("invalid or oversized byte-aligned snake string");
  }
  return data;
}

td::Result<std::string> SnakeString::load_snake_string(vm::CellSlice& cs) const {
  TRY_RESULT(binary_data, load_snake_binary(cs));
  return std::string(binary_data.begin(), binary_data.end());
}

bool SnakeString::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  auto text_result = load_snake_string(cs);
  if (text_result.is_error()) {
    return pp.fail(text_result.error().message().str());
  }
  auto text = text_result.move_as_ok();
  pp.os << '"';
  for (char c : text) {
    if (c == '"') {
      pp.os << "\\\"";
    } else if (c == '\\') {
      pp.os << "\\\\";
    } else if (c == '\n') {
      pp.os << "\\n";
    } else if (c == '\r') {
      pp.os << "\\r";
    } else if (c == '\t') {
      pp.os << "\\t";
    } else if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) >= 127) {
      pp.os << "\\x" << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(c)) << std::dec;
    } else {
      pp.os << c;
    }
  }
  pp.os << '"';
  return true;
}

bool SnakeString::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  auto text_result = load_snake_string(cs);
  if (text_result.is_error()) {
    return pp.fail(text_result.error().message().str());
  }
  auto str = text_result.move_as_ok();
  if (!td::check_utf8(str)) {
    return pp.fail("invalid utf-8 string");
  }
  return pp.out(str);
}

bool TupleT::skip(vm::CellSlice& cs) const {
  int i = n;
  for (; i > 0; --i) {
    if (!X.skip(cs)) {
      break;
    }
  }
  return !i;
}

bool TupleT::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  int i = n;
  for (; i > 0; --i) {
    if (!X.validate_skip(ops, cs, weak)) {
      break;
    }
  }
  return !i;
}

bool TLB::validate_ref_internal(int* ops, Ref<vm::Cell> cell_ref, bool weak) const {
  auto cache = ValidateCache::get();
  if (cache && !(*cache)(this, cell_ref)) {
    return true;
  }
  if (ops) {
    if (*ops <= 0) {
      return false;
    }
    --*ops;
  }
  bool is_special;
  auto cs = load_cell_slice_special(std::move(cell_ref), is_special);
  if (cs.special_type() == vm::Cell::SpecialType::PrunnedBranch && weak) {
    return true;
  }
  if (always_special() != is_special) {
    return false;
  }
  return validate_skip(ops, cs, weak) && cs.empty_ext();
}

bool TLB::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  pp.open("raw@");
  pp << *this << ' ';
  vm::CellSlice cs_copy{cs};
  int size_limit = pp.limit;
  if (!validate_skip(&size_limit, cs) || !cs_copy.cut_tail(cs)) {
    return pp.fail("invalid value");
  }
  pp.raw_nl();
  return (cs_copy.print_rec(pp.os, &pp.limit, pp.indent) && pp.mkindent() && pp.close()) ||
         pp.fail("raw value too long");
}

bool TLB::print_skip(tlb::JsonPrinter& pp, vm::CellSlice& cs) const {
  // print base64 encoded boc, so other programs can parse it
  vm::CellSlice cs_copy{cs};
  if (!validate_skip(nullptr, cs) || !cs_copy.cut_tail(cs)) {
    return pp.fail("invalid value");
  }
  vm::CellBuilder cb;
  cell_builder_add_slice(cb, cs_copy);
  Ref<vm::Cell> new_cell = cb.finalize();
  auto boc = vm::std_boc_serialize(new_cell);

  if (boc.is_ok()) {
    auto b64str = td::base64_encode(boc.move_as_ok().as_slice());
    return pp.out(b64str);
  } else {
    return pp.fail("failed to serialize cell");
  }
}

bool TLB::print_special(PrettyPrinter& pp, vm::CellSlice& cs) const {
  pp.open("raw@");
  pp << *this << ' ';
  pp.raw_nl();
  return (cs.print_rec(pp.os, &pp.limit, pp.indent) && pp.mkindent() && pp.close()) || pp.fail("raw value too long");
}

bool TLB::print_special(JsonPrinter& pp, vm::CellSlice& cs) const {
  return print_skip(pp, cs);
}

bool TLB::print_ref(PrettyPrinter& pp, Ref<vm::Cell> cell_ref) const {
  if (cell_ref.is_null()) {
    return pp.fail("null cell reference");
  }
  if (!pp.register_recursive_call()) {
    return pp.fail("too many recursive calls while printing a TL-B value");
  }
  bool is_special;
  auto cs = load_cell_slice_special(std::move(cell_ref), is_special);
  if (is_special) {
    return print_special(pp, cs);
  } else {
    return print_skip(pp, cs) && (cs.empty_ext() || pp.fail("extra data in cell"));
  }
}

bool TLB::print_ref(JsonPrinter& pp, Ref<vm::Cell> cell_ref) const {
  if (cell_ref.is_null()) {
    return pp.fail("null cell reference");
  }
  if (!pp.register_recursive_call()) {
    return pp.fail("too many recursive calls while printing a TL-B value");
  }
  try {
    bool is_special;
    auto cs = load_cell_slice_special(cell_ref, is_special);
    if (always_special() != is_special) {
      return pp.fail("incorrect TL-B cell kind");
    }
    if (is_special) {
      int ops = default_validate_max_cells;
      if (!validate_skip(&ops, cs) || !cs.empty_ext()) {
        return pp.fail("invalid special TL-B cell");
      }
      auto boc = vm::std_boc_serialize(cell_ref);
      return boc.is_ok() ? pp.out(td::base64_encode(boc.move_as_ok().as_slice()))
                         : pp.fail("failed to serialize special cell");
    }
    if (!print_skip(pp, cs)) {
      return pp.fail("invalid TL-B value");
    }
    return cs.empty_ext() || pp.fail("extra data in cell");
  } catch (vm::VmError&) {
    return pp.fail("failed to load TL-B cell");
  } catch (vm::VmVirtError&) {
    return pp.fail("failed to load virtualized TL-B cell");
  } catch (vm::VmNoGas&) {
    return pp.fail("out of gas while printing TL-B cell");
  } catch (vm::CellBuilder::CellCreateError&) {
    return pp.fail("failed to build TL-B cell");
  } catch (vm::CellBuilder::CellWriteError&) {
    return pp.fail("failed to build TL-B cell");
  }
}

bool TLB::print_skip(std::ostream& os, vm::CellSlice& cs, int indent, int rec_limit) const {
  PrettyPrinter pp{os, indent};
  pp.set_limit(rec_limit);
  return pp.fail_unless(print_skip(pp, cs));
}

bool TLB::print(std::ostream& os, const vm::CellSlice& cs, int indent, int rec_limit) const {
  PrettyPrinter pp{os, indent};
  pp.set_limit(rec_limit);
  return pp.fail_unless(print(pp, cs));
}

bool TLB::print_ref(std::ostream& os, Ref<vm::Cell> cell_ref, int indent, int rec_limit) const {
  PrettyPrinter pp{os, indent};
  pp.set_limit(rec_limit);
  return pp.fail_unless(print_ref(pp, std::move(cell_ref)));
}

bool TLB::print_ref(td::StringBuilder& sb, Ref<vm::Cell> cell_ref, int indent, int rec_limit) const {
  std::ostringstream ss;
  auto result = print_ref(ss, std::move(cell_ref), indent, rec_limit);
  sb << ss.str();
  return result;
}

std::string TLB::as_string_skip(vm::CellSlice& cs, int indent) const {
  std::ostringstream os;
  print_skip(os, cs, indent);
  return os.str();
}

std::string TLB::as_string(const vm::CellSlice& cs, int indent) const {
  std::ostringstream os;
  print(os, cs, indent);
  return os.str();
}

std::string TLB::as_string_ref(Ref<vm::Cell> cell_ref, int indent) const {
  std::ostringstream os;
  print_ref(os, std::move(cell_ref), indent);
  return os.str();
}

namespace {

struct PrintAug : vm::dict::AugmentationData {
  const TLB& aug_type;
  explicit PrintAug(const TLB& aug_type) : aug_type(aug_type) {
  }
  bool skip_extra(vm::CellSlice& cs) const override {
    return aug_type.skip(cs);
  }
  bool eval_leaf(vm::CellBuilder&, vm::CellSlice&) const override {
    return false;
  }
  bool eval_fork(vm::CellBuilder&, vm::CellSlice&, vm::CellSlice&) const override {
    return false;
  }
  bool eval_empty(vm::CellBuilder&) const override {
    return false;
  }
  bool check_empty(vm::CellSlice& cs) const override {
    return true;
  }
  bool check_fork(vm::CellSlice& cs, vm::CellSlice& left_cs, vm::CellSlice& right_cs) const override {
    return true;
  }
  bool check_leaf(vm::CellSlice& cs, vm::CellSlice& val_cs) const override {
    return true;
  }
};

}  // namespace

bool print_hashmap(PrettyPrinter& pp, vm::CellSlice& cs, unsigned n, const TLB& dict_type, const TLB& value_type,
                   bool non_empty) {
  try {
    vm::CellSlice dict_cs{cs};
    if (!dict_type.skip(cs)) {
      return pp.fail("invalid Hashmap");
    }
    dict_cs.cut_tail(cs);
    vm::Dictionary dict{(int)n};
    if (non_empty) {
      dict = vm::Dictionary{vm::DictNonEmpty{}, dict_cs, (int)n, false};
    } else {
      dict = vm::Dictionary{dict_cs, (int)n, false};
    }
    pp.open(non_empty ? "Hashmap" : "HashmapE");
    return dict.check_for_each([&](Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
      if (!pp.register_recursive_call()) {
        return pp.fail("too many recursive calls while printing a TL-B value");
      }
      pp.mode_nl();
      pp.os << "x" << key.to_hex(key_len) << ":";
      return (value_type.print_skip(pp, value.write()) && value->empty_ext()) || pp.fail("invalid hashmap value");
    }) && pp.close();
  } catch (vm::VmError&) {
    return pp.fail("invalid Hashmap");
  }
}

bool print_hashmap_aug(PrettyPrinter& pp, vm::CellSlice& cs, unsigned n, const TLB& dict_type, const TLB& value_type,
                       const TLB& aug_type, bool non_empty) {
  try {
    vm::CellSlice dict_cs{cs};
    if (!dict_type.skip(cs)) {
      return pp.fail("invalid HashmapAug");
    }
    dict_cs.cut_tail(cs);
    PrintAug aug{aug_type};
    std::unique_ptr<vm::AugmentedDictionary> dict;
    if (non_empty) {
      dict = std::make_unique<vm::AugmentedDictionary>(vm::DictNonEmpty{}, Ref<vm::CellSlice>{true, std::move(dict_cs)},
                                                       (int)n, aug, false);
    } else {
      dict =
          std::make_unique<vm::AugmentedDictionary>(Ref<vm::CellSlice>{true, std::move(dict_cs)}, (int)n, aug, false);
    }
    pp.open(non_empty ? "HashmapAug" : "HashmapAugE");
    return dict->check_for_each_extra([&](Ref<vm::CellSlice> value, Ref<vm::CellSlice>, td::ConstBitPtr key,
                                          int key_len) {
      if (!pp.register_recursive_call()) {
        return pp.fail("too many recursive calls while printing a TL-B value");
      }
      pp.mode_nl();
      pp.os << "x" << key.to_hex(key_len) << ":";
      return (value_type.print_skip(pp, value.write()) && value->empty_ext()) || pp.fail("invalid hashmapAug value");
    }) && pp.close();
  } catch (vm::VmError&) {
    return pp.fail("invalid HashmapAug");
  }
}

PrettyPrinter::~PrettyPrinter() {
  if (failed || level) {
    if (nl_used) {
      nl(-2 * level);
    }
    os << "PRINTING FAILED";
    while (level > 0) {
      os << ')';
      --level;
    }
  }
  if (nl_used) {
    os << std::endl;
  }
}

bool PrettyPrinter::fail(std::string msg) {
  os << "<FATAL: " << msg << ">" << std::endl;
  failed = true;
  return false;
}

bool PrettyPrinter::mkindent(int delta) {
  indent += delta;
  for (int i = 0; i < indent; i++) {
    os << ' ';
  }
  nl_used = true;
  return true;
}

bool PrettyPrinter::nl(int delta) {
  os << std::endl;
  return mkindent(delta);
}
bool PrettyPrinter::raw_nl(int delta) {
  os << std::endl;
  indent += delta;
  nl_used = true;
  return true;
}

bool PrettyPrinter::open(std::string msg) {
  os << "(" << msg;
  indent += 2;
  level++;
  return true;
}

bool PrettyPrinter::close() {
  return close("");
}

bool PrettyPrinter::close(std::string msg) {
  if (level <= 0) {
    return fail("cannot close scope");
  }
  indent -= 2;
  --level;
  os << msg << ")";
  return true;
}

bool PrettyPrinter::mode_nl() {
  if (mode & 1) {
    return nl();
  } else {
    os << ' ';
    return true;
  }
}

bool PrettyPrinter::field(std::string name) {
  mode_nl();
  os << name << ':';
  return true;
}

bool PrettyPrinter::field() {
  mode_nl();
  return true;
}

bool PrettyPrinter::field_int(long long x, std::string name) {
  os << ' ' << name << ':' << x;
  return true;
}

bool PrettyPrinter::field_int(long long x) {
  os << ' ' << x;
  return true;
}

bool PrettyPrinter::field_uint(unsigned long long x, std::string name) {
  os << ' ' << name << ':' << x;
  return true;
}

bool PrettyPrinter::field_uint(unsigned long long x) {
  os << ' ' << x;
  return true;
}

bool PrettyPrinter::fetch_bits_field(vm::CellSlice& cs, int n) {
  os << " x";
  return cs.have(n) && out(cs.fetch_bits(n).to_hex());
}

bool PrettyPrinter::fetch_bits_field(vm::CellSlice& cs, int n, std::string name) {
  os << ' ' << name << ":x";
  return cs.have(n) && out(cs.fetch_bits(n).to_hex());
}

bool PrettyPrinter::fetch_int_field(vm::CellSlice& cs, int n) {
  return cs.have(n) && field_int(cs.fetch_long(n));
}

bool PrettyPrinter::fetch_int_field(vm::CellSlice& cs, int n, std::string name) {
  return cs.have(n) && field_int(cs.fetch_long(n), name);
}

bool PrettyPrinter::fetch_uint_field(vm::CellSlice& cs, int n) {
  return cs.have(n) && field_uint(cs.fetch_ulong(n));
}

bool PrettyPrinter::fetch_uint_field(vm::CellSlice& cs, int n, std::string name) {
  return cs.have(n) && field_uint(cs.fetch_ulong(n), name);
}

bool PrettyPrinter::fetch_int256_field(vm::CellSlice& cs, int n) {
  os << ' ';
  return out_integer(cs.fetch_int256(n, true));
}

bool PrettyPrinter::fetch_int256_field(vm::CellSlice& cs, int n, std::string name) {
  os << ' ' << name << ':';
  return out_integer(cs.fetch_int256(n, true));
}

bool PrettyPrinter::fetch_uint256_field(vm::CellSlice& cs, int n) {
  os << ' ';
  return out_integer(cs.fetch_int256(n, false));
}

bool PrettyPrinter::fetch_uint256_field(vm::CellSlice& cs, int n, std::string name) {
  os << ' ' << name << ':';
  return out_integer(cs.fetch_int256(n, false));
}

bool PrettyPrinter::fetch_bool_field(vm::CellSlice& cs) {
  os << ' ';
  return cs.have(1) && out(cs.fetch_ulong(1) ? "true" : "false");
}

bool PrettyPrinter::fetch_bool_field(vm::CellSlice& cs, std::string name) {
  os << ' ' << name << ':';
  return cs.have(1) && out(cs.fetch_ulong(1) ? "true" : "false");
}

}  // namespace tlb

namespace tlb {

bool TypenameLookup::register_types(typename TypenameLookup::register_func_t func) {
  return func([this](const char* name, const TLB* tp) { return register_type(name, tp); });
}

bool TypenameLookup::register_type(const char* name, const TLB* tp) {
  if (!name || !tp) {
    return false;
  }
  auto res = types.emplace(name, tp);
  return res.second;
}

const TLB* TypenameLookup::lookup(std::string str) const {
  auto it = types.find(str);
  return it != types.end() ? it->second : nullptr;
}

const TLB* TypenameLookup::lookup(td::Slice str) const {
  auto it = std::lower_bound(types.begin(), types.end(), str,
                             [](const auto& x, const auto& y) { return td::Slice(x.first) < y; });
  return it != types.end() && td::Slice(it->first) == str ? it->second : nullptr;
}

TLB::ValidateCache TLB::ValidateCache::create_for_type(const TLB* type) {
  return ValidateCache{
      [s = td::HashSet<vm::CellHash>(), type](const TLB* t, const td::Ref<vm::Cell>& cell) mutable -> bool {
        if (type != t) {
          return true;
        }
        return s.insert(cell->get_hash()).second;
      }};
}

namespace {

bool json_digit(char c) {
  return c >= '0' && c <= '9';
}

int json_hex(char c) {
  if (json_digit(c)) {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// The td JSON decoder accepts nonstandard numeric literals and escapes. Check
// lexical rules on the original text before letting it validate the structure.
bool valid_json_lexemes(const std::string& json) {
  for (size_t i = 0; i < json.size();) {
    char c = json[i];
    if (c == '"') {
      ++i;
      bool closed = false;
      while (i < json.size()) {
        c = json[i++];
        if (c == '"') {
          closed = true;
          break;
        }
        if (static_cast<unsigned char>(c) < 0x20) {
          return false;
        }
        if (c != '\\') {
          continue;
        }
        if (i == json.size()) {
          return false;
        }
        c = json[i++];
        if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't') {
          continue;
        }
        if (c != 'u' || json.size() - i < 4) {
          return false;
        }
        unsigned codepoint = 0;
        for (int j = 0; j < 4; ++j) {
          int digit = json_hex(json[i++]);
          if (digit < 0) {
            return false;
          }
          codepoint = (codepoint << 4) | digit;
        }
        if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
          return false;
        }
        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
          if (json.size() - i < 6 || json[i++] != '\\' || json[i++] != 'u') {
            return false;
          }
          unsigned low = 0;
          for (int j = 0; j < 4; ++j) {
            int digit = json_hex(json[i++]);
            if (digit < 0) {
              return false;
            }
            low = (low << 4) | digit;
          }
          if (low < 0xdc00 || low > 0xdfff) {
            return false;
          }
        }
      }
      if (!closed) {
        return false;
      }
    } else if (c == '-' || c == '+' || c == '.' || json_digit(c)) {
      if (c == '-') {
        ++i;
      }
      if (i == json.size() || !json_digit(json[i])) {
        return false;
      }
      if (json[i] == '0') {
        ++i;
      } else {
        do {
          ++i;
        } while (i < json.size() && json_digit(json[i]));
      }
      if (i < json.size() && json[i] == '.') {
        ++i;
        if (i == json.size() || !json_digit(json[i])) {
          return false;
        }
        do {
          ++i;
        } while (i < json.size() && json_digit(json[i]));
      }
      if (i < json.size() && (json[i] == 'e' || json[i] == 'E')) {
        ++i;
        if (i < json.size() && (json[i] == '+' || json[i] == '-')) {
          ++i;
        }
        if (i == json.size() || !json_digit(json[i])) {
          return false;
        }
        do {
          ++i;
        } while (i < json.size() && json_digit(json[i]));
      }
      // A number must end at a JSON value delimiter, not an additional token.
      if (i < json.size() && json[i] != ',' && json[i] != ']' && json[i] != '}' && json[i] != ' ' &&
          json[i] != '\t' && json[i] != '\n' && json[i] != '\r') {
        return false;
      }
    } else {
      if (static_cast<unsigned char>(c) < 0x20 && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        return false;
      }
      ++i;
    }
  }
  return true;
}

}  // namespace

std::string JsonPrinter::escape_string(const std::string& str) {
  std::string result;
  result.reserve(str.size() + 10);

  for (char c : str) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      default:
        if (static_cast<unsigned char>(c) < 32 || c == 127) {
          // escaping control chars and DEL
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

bool JsonPrinter::begin_value() {
  if (failed_) {
    return false;
  }
  if (objects_.empty()) {
    if (root_started_ || !buffer().empty()) {
      return fail("multiple JSON root values");
    }
    root_started_ = true;
  } else {
    if (!objects_.back().pending_value && !field()) {
      return false;
    }
    objects_.back().pending_value = false;
  }
  return true;
}

bool JsonPrinter::open(std::string msg) {
  if (!td::check_utf8(msg)) {
    return fail("invalid UTF-8 type name");
  }
  if (!begin_value()) {
    return false;
  }
  buffer() += '{';
  objects_.emplace_back();
  return msg.empty() || (field("@type") && out(std::move(msg)));
}

bool JsonPrinter::close() {
  return close("");
}

bool JsonPrinter::close(std::string) {
  if (failed_ || objects_.empty() || objects_.back().pending_value) {
    return fail("unbalanced JSON object");
  }
  buffer() += '}';
  objects_.pop_back();
  return true;
}

bool JsonPrinter::field(std::string name) {
  if (!td::check_utf8(name)) {
    return fail("invalid UTF-8 field name");
  }
  if (failed_ || objects_.empty() || objects_.back().pending_value) {
    return fail("JSON field without object or value");
  }
  auto& object = objects_.back();
  if (!object.names.insert(name).second) {
    return fail("duplicate JSON field");
  }
  if (!object.first_field) {
    buffer() += ',';
  }
  buffer() += "\"" + escape_string(name) + "\":";
  object.first_field = false;
  object.pending_value = true;
  return true;
}

bool JsonPrinter::field() {
  if (objects_.empty()) {
    return fail("unnamed field without object");
  }
  auto& object = objects_.back();
  std::string name;
  do {
    name = "@" + std::to_string(object.unnamed_index++);
  } while (object.names.count(name));
  return field(std::move(name));
}

bool JsonPrinter::field_int(long long value) {
  return out_int(value);
}

bool JsonPrinter::field_int(long long value, std::string name) {
  return (name.empty() || field(std::move(name))) && out_int(value);
}

bool JsonPrinter::field_uint(unsigned long long value) {
  return out_uint(value);
}

bool JsonPrinter::field_uint(unsigned long long value, std::string name) {
  return (name.empty() || field(std::move(name))) && out_uint(value);
}

bool JsonPrinter::fetch_bits_field(vm::CellSlice& cs, int n) {
  return cs.have(n) ? out(cs.fetch_bits(n).to_hex()) : fail("not enough bits");
}

bool JsonPrinter::fetch_bits_field(vm::CellSlice& cs, int n, std::string name) {
  return cs.have(n) && (name.empty() || field(std::move(name))) ? fetch_bits_field(cs, n)
                                                              : fail("not enough bits or invalid field");
}

bool JsonPrinter::fetch_int_field(vm::CellSlice& cs, int n) {
  return cs.have(n) ? out_int(cs.fetch_long(n)) : fail("not enough bits");
}

bool JsonPrinter::fetch_int_field(vm::CellSlice& cs, int n, std::string name) {
  return cs.have(n) && (name.empty() || field(std::move(name))) ? fetch_int_field(cs, n)
                                                              : fail("not enough bits or invalid field");
}

bool JsonPrinter::fetch_uint_field(vm::CellSlice& cs, int n) {
  return cs.have(n) ? out_uint(cs.fetch_ulong(n)) : fail("not enough bits");
}

bool JsonPrinter::fetch_uint_field(vm::CellSlice& cs, int n, std::string name) {
  return cs.have(n) && (name.empty() || field(std::move(name))) ? fetch_uint_field(cs, n)
                                                              : fail("not enough bits or invalid field");
}

bool JsonPrinter::fetch_int256_field(vm::CellSlice& cs, int n) {
  if (!cs.have(n)) {
    return fail("not enough bits");
  }
  return out_integer(cs.fetch_int256(n, true));
}

bool JsonPrinter::fetch_int256_field(vm::CellSlice& cs, int n, std::string name) {
  return cs.have(n) && (name.empty() || field(std::move(name))) ? fetch_int256_field(cs, n)
                                                              : fail("not enough bits or invalid field");
}

bool JsonPrinter::fetch_uint256_field(vm::CellSlice& cs, int n) {
  if (!cs.have(n)) {
    return fail("not enough bits");
  }
  return out_integer(cs.fetch_int256(n, false));
}

bool JsonPrinter::fetch_uint256_field(vm::CellSlice& cs, int n, std::string name) {
  return cs.have(n) && (name.empty() || field(std::move(name))) ? fetch_uint256_field(cs, n)
                                                              : fail("not enough bits or invalid field");
}

bool JsonPrinter::fetch_bool_field(vm::CellSlice& cs) {
  return cs.have(1) ? write_raw(cs.fetch_ulong(1) ? "true" : "false") : fail("not enough bits");
}

bool JsonPrinter::fetch_bool_field(vm::CellSlice& cs, std::string name) {
  return cs.have(1) && (name.empty() || field(std::move(name))) ? fetch_bool_field(cs)
                                                              : fail("not enough bits or invalid field");
}

bool JsonPrinter::out(std::string str) {
  if (!td::check_utf8(str)) {
    return fail("invalid UTF-8 string");
  }
  if (!begin_value()) {
    return false;
  }
  buffer() += "\"" + escape_string(str) + "\"";
  return true;
}

bool JsonPrinter::out_int(long long value) {
  return out(std::to_string(value));
}

bool JsonPrinter::out_uint(unsigned long long value) {
  return out(std::to_string(value));
}

bool JsonPrinter::out_integer(td::RefInt256 value) {
  if (value.is_null()) {
    return fail("invalid integer");
  }
  std::ostringstream oss;
  oss << value;
  return out(oss.str());
}

bool JsonPrinter::cons(std::string str) {
  return open(str) && close();
}

bool JsonPrinter::register_recursive_call() {
  if (limit_ <= 0) {
    return false;
  }
  --limit_;
  return true;
}

void JsonPrinter::set_limit(int new_limit) {
  if (new_limit >= 0) {
    limit_ = new_limit;
  }
}

bool JsonPrinter::fail(std::string msg) {
  if (!failed_) {
    if (!td::check_utf8(msg)) {
      msg = "invalid TL-B value";
    }
    buffer() = "{\"@error\":\"" + escape_string(msg) + "\"}";
    objects_.clear();
    root_started_ = true;
    failed_ = true;
  }
  return false;
}

bool JsonPrinter::fail_unless(bool res) {
  return res || fail("invalid TL-B value");
}

bool JsonPrinter::write_raw(const std::string& json) {
  if (!td::check_utf8(json)) {
    return fail("invalid UTF-8 raw JSON value");
  }
  if (!valid_json_lexemes(json)) {
    return fail("invalid raw JSON value");
  }
  std::string copy = json;
  if (td::json_decode(td::MutableSlice(copy)).is_error()) {
    return fail("invalid raw JSON value");
  }
  if (!begin_value()) {
    return false;
  }
  buffer() += json;
  return true;
}

}  // namespace tlb
