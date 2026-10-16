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

#include <string>

#include "block/block-auto.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/base64.h"
#include "td/utils/tests.h"
#include "tl/tlblib.hpp"
#include "vm/boc.h"
#include "vm/cells.h"

namespace {

td::Ref<vm::Cell> byte_cell(unsigned value) {
  vm::CellBuilder cb;
  cb.store_long(value, 8);
  return cb.finalize();
}

std::string print_cell(const tlb::TLB& type, td::Ref<vm::Cell> cell, bool& ok, int limit = 4096) {
  tlb::JsonPrinter pp;
  pp.set_limit(limit);
  ok = type.print_ref(pp, std::move(cell));
  ok = ok && pp.ok();
  return pp.get_json();
}

}  // namespace

TEST(TlbJson, MixedFieldsEscapingAndInvalidUtf8) {
  tlb::JsonPrinter pp;
  ASSERT_TRUE(pp.open("outer"));
  ASSERT_TRUE(pp.field("name\"\\\n"));
  ASSERT_TRUE(pp.out("é \"\\\n\t"));
  ASSERT_TRUE(pp.out("implicit"));
  ASSERT_TRUE(pp.field("child"));
  ASSERT_TRUE(pp.open("inner"));
  vm::CellBuilder flag;
  flag.store_long(1, 1);
  auto flag_cs = vm::load_cell_slice(flag.finalize());
  ASSERT_TRUE(pp.fetch_bool_field(flag_cs, "active"));
  ASSERT_TRUE(flag_cs.empty_ext());
  ASSERT_TRUE(pp.close());
  ASSERT_TRUE(pp.out("second"));
  ASSERT_TRUE(pp.close());
  ASSERT_TRUE(pp.ok());
  ASSERT_EQ(
      pp.get_json(),
      R"json({"@type":"outer","name\"\\\n":"é \"\\\n\t","@0":"implicit","child":{"@type":"inner","active":true},"@1":"second"})json");

  tlb::JsonPrinter bad;
  ASSERT_TRUE(bad.open());
  ASSERT_TRUE(bad.field("text"));
  ASSERT_TRUE(!bad.out(std::string("\xff", 1)));
  ASSERT_TRUE(!bad.ok());
  ASSERT_TRUE(bad.get_json().find("@error") != std::string::npos);
}

TEST(TlbJson, DuplicateKeysMalformedRawAndIncompleteValues) {
  tlb::JsonPrinter pp;
  ASSERT_TRUE(!pp.ok());
  ASSERT_TRUE(pp.open());
  ASSERT_TRUE(pp.field("@0"));
  ASSERT_TRUE(pp.out("named"));
  ASSERT_TRUE(pp.field());
  ASSERT_TRUE(pp.out("unnamed"));
  ASSERT_TRUE(pp.close());
  ASSERT_TRUE(pp.ok());
  ASSERT_EQ(pp.get_json(), R"({"@0":"named","@1":"unnamed"})");

  tlb::JsonPrinter duplicate;
  ASSERT_TRUE(duplicate.open());
  ASSERT_TRUE(duplicate.field("name"));
  ASSERT_TRUE(duplicate.out("first"));
  ASSERT_TRUE(!duplicate.field("name"));
  ASSERT_TRUE(!duplicate.ok());
  auto error_json = duplicate.get_json();
  ASSERT_TRUE(td::json_decode(td::MutableSlice(error_json)).is_ok());

  tlb::JsonPrinter raw;
  ASSERT_TRUE(raw.open());
  ASSERT_TRUE(raw.field("raw"));
  ASSERT_TRUE(!raw.write_raw("{bad json}"));
  ASSERT_TRUE(!raw.ok());
  auto raw_error = raw.get_json();
  ASSERT_TRUE(td::json_decode(td::MutableSlice(raw_error)).is_ok());

  for (const auto& malformed : {"1e2e3", "01", "+1", ".5", "1.", "1true"}) {
    tlb::JsonPrinter invalid_number;
    ASSERT_TRUE(invalid_number.open());
    ASSERT_TRUE(invalid_number.field("number"));
    ASSERT_TRUE(!invalid_number.write_raw(malformed));
    ASSERT_TRUE(!invalid_number.ok());
  }

  tlb::JsonPrinter truncated;
  ASSERT_TRUE(truncated.open());
  vm::CellBuilder empty;
  auto empty_cs = vm::load_cell_slice(empty.finalize());
  ASSERT_TRUE(!truncated.fetch_uint_field(empty_cs, 8, "value"));
  ASSERT_TRUE(!truncated.ok());
  auto truncated_error = truncated.get_json();
  ASSERT_TRUE(td::json_decode(td::MutableSlice(truncated_error)).is_ok());

  tlb::JsonPrinter limited;
  limited.set_limit(1);
  ASSERT_TRUE(limited.register_recursive_call());
  ASSERT_TRUE(!limited.register_recursive_call());
  ASSERT_TRUE(!limited.register_recursive_call());
}

TEST(TlbJson, GeneratedBitBoolAndNestedTypes) {
  bool ok = false;
  td::Ref<vm::Cell> bit;
  ASSERT_TRUE(block::gen::t_Bit.cell_pack_bit(bit, true));
  ASSERT_EQ(print_cell(block::gen::t_Bit, bit, ok), R"({"@type":"bit","@0":"1"})");
  ASSERT_TRUE(ok);

  td::Ref<vm::Cell> boolean;
  ASSERT_TRUE(block::gen::t_Bool.cell_pack_bool_false(boolean));
  ASSERT_EQ(print_cell(block::gen::t_Bool, boolean, ok), R"({"@type":"bool_false"})");
  ASSERT_TRUE(ok);

  block::gen::Either either(tlb::t_uint8, block::gen::t_Bit);
  block::gen::Maybe maybe(either);
  block::gen::Both both(maybe, block::gen::t_Bool);
  vm::CellBuilder cb;
  cb.store_long(1, 1);  // just
  cb.store_long(1, 1);  // right
  cb.store_long(1, 1);  // bit
  cb.store_long(0, 1);  // bool_false
  ASSERT_EQ(
      print_cell(both, cb.finalize(), ok),
      R"({"@type":"pair","first":{"@type":"just","value":{"@type":"right","value":{"@type":"bit","@0":"1"}}},"second":{"@type":"bool_false"}})");
  ASSERT_TRUE(ok);

  tlb::TupleT tuple(2, tlb::t_uint8);
  cb.reset();
  cb.store_long(7, 8).store_long(42, 8);
  auto json = print_cell(tuple, cb.finalize(), ok);
  ASSERT_TRUE(ok);
  ASSERT_TRUE(json.find("\"@type\":\"tuple\"") != std::string::npos);
  ASSERT_TRUE(json.find("\"7\"") != std::string::npos);
  ASSERT_TRUE(json.find("\"42\"") != std::string::npos);

  td::Ref<vm::Cell> none;
  ASSERT_TRUE(block::gen::t_MsgAddressExt.cell_pack_addr_none(none));
  ASSERT_EQ(print_cell(block::gen::t_MsgAddress, none, ok), R"({"@0":{"@type":"addr_none"}})");
  ASSERT_TRUE(ok);
}

TEST(TlbJson, TruncationUnknownTagAndExactReference) {
  bool ok = false;
  print_cell(block::gen::t_Bit, vm::CellBuilder().finalize(), ok);
  ASSERT_TRUE(!ok);

  vm::CellBuilder cb;
  cb.store_long(1, 2);  // addr_extern without its 9-bit length
  print_cell(block::gen::t_MsgAddressExt, cb.finalize(), ok);
  ASSERT_TRUE(!ok);

  cb.reset();
  cb.store_long(3, 2);  // invalid MsgAddressExt constructor
  print_cell(block::gen::t_MsgAddressExt, cb.finalize(), ok);
  ASSERT_TRUE(!ok);

  cb.reset();
  cb.store_long(7, 8).store_long(1, 1);  // extra bit after uint8
  print_cell(tlb::t_uint8, cb.finalize(), ok);
  ASSERT_TRUE(!ok);

  cb.reset();
  cb.store_long(7, 8).store_ref(byte_cell(9));  // extra reference after uint8
  print_cell(tlb::t_uint8, cb.finalize(), ok);
  ASSERT_TRUE(!ok);

  tlb::RefT ref_byte(tlb::t_uint8);
  cb.reset();
  cb.store_ref(vm::CellBuilder().finalize());  // truncated referenced byte
  print_cell(ref_byte, cb.finalize(), ok);
  ASSERT_TRUE(!ok);

  vm::CellBuilder overlong;
  overlong.store_long(7, 8).store_ref(byte_cell(9));
  cb.reset();
  cb.store_ref(overlong.finalize());  // referenced type must consume its entire cell
  print_cell(ref_byte, cb.finalize(), ok);
  ASSERT_TRUE(!ok);
}

TEST(TlbJson, RecursionLimitAndRawCellRoundTrip) {
  tlb::RefT ref_byte(tlb::t_uint8);
  vm::CellBuilder cb;
  cb.store_ref(byte_cell(5));
  bool ok = false;
  print_cell(ref_byte, cb.finalize(), ok, 0);
  ASSERT_TRUE(!ok);

  auto ordinary = byte_cell(37);
  vm::CellBuilder payload;
  payload.store_ref(ordinary);
  auto exotic = vm::CellBuilder::create_merkle_proof(payload.finalize());
  ASSERT_TRUE(exotic.not_null());

  for (const auto& ref : {ordinary, td::Ref<vm::Cell>(exotic)}) {
    vm::CellBuilder wrapper;
    wrapper.store_ref(ref);
    auto json = print_cell(tlb::t_RefCell, wrapper.finalize(), ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(json.size() >= 2 && json.front() == '"' && json.back() == '"');
    auto decoded = td::base64_decode(json.substr(1, json.size() - 2));
    ASSERT_TRUE(decoded.is_ok());
    auto restored = vm::std_boc_deserialize(decoded.move_as_ok());
    ASSERT_TRUE(restored.is_ok());
    ASSERT_EQ(restored.ok()->get_hash(), ref->get_hash());
    bool was_special = false;
    bool remains_special = false;
    vm::load_cell_slice_special(ref, was_special);
    vm::load_cell_slice_special(restored.ok(), remains_special);
    ASSERT_EQ(remains_special, was_special);
  }

  bool was_special = false;
  auto exotic_slice = vm::load_cell_slice_special(exotic, was_special);
  ASSERT_TRUE(was_special);
  vm::CellBuilder ordinary_copy_builder;
  ASSERT_TRUE(ordinary_copy_builder.append_cellslice_bool(exotic_slice));
  auto ordinary_copy = ordinary_copy_builder.finalize();
  block::gen::MERKLE_PROOF merkle(tlb::t_Anything);
  ASSERT_TRUE(!merkle.validate_ref(ordinary_copy));
  print_cell(merkle, ordinary_copy, ok);
  ASSERT_TRUE(!ok);
}

TEST(TlbJson, SnakeStringCursorAndUtf8) {
  bool ok = false;
  auto single = byte_cell('A');
  ASSERT_EQ(print_cell(tlb::t_SnakeString, single, ok), R"("A")");
  ASSERT_TRUE(ok);
  ASSERT_TRUE(tlb::t_SnakeString.validate_ref(1, single));
  ASSERT_TRUE(!tlb::t_SnakeString.validate_ref(0, single));
  ASSERT_TRUE(tlb::t_SnakeString.validate_upto(0, vm::load_cell_slice(single)));

  vm::CellBuilder tail;
  tail.store_bytes("llo", 3);
  vm::CellBuilder mid;
  mid.store_bytes("e", 1).store_ref(tail.finalize());
  vm::CellBuilder head;
  head.store_long(5, 3).store_bytes("H", 1).store_ref(mid.finalize());
  auto cs = vm::load_cell_slice(head.finalize());
  ASSERT_EQ(cs.fetch_ulong(3), 5u);
  auto text = tlb::t_SnakeString.load_snake_string(cs);
  ASSERT_TRUE(text.is_ok());
  ASSERT_EQ(text.move_as_ok(), "Hello");
  ASSERT_TRUE(cs.empty_ext());

  vm::CellBuilder tail_with_root;
  tail_with_root.store_bytes("i", 1);
  vm::CellBuilder root_with_ref;
  root_with_ref.store_bytes("H", 1).store_ref(tail_with_root.finalize());
  auto root = root_with_ref.finalize();
  ASSERT_TRUE(!tlb::t_SnakeString.validate_ref(1, root));
  ASSERT_TRUE(tlb::t_SnakeString.validate_ref(2, root));
  vm::CellBuilder parent_builder;
  auto parent = parent_builder.store_ref(root).finalize();
  tlb::RefT snake_ref(tlb::t_SnakeString);
  ASSERT_TRUE(!snake_ref.validate_ref(2, parent));
  ASSERT_TRUE(snake_ref.validate_ref(3, parent));
  ASSERT_TRUE(!tlb::t_SnakeString.validate_upto(0, vm::load_cell_slice(root)));
  ASSERT_TRUE(tlb::t_SnakeString.validate_upto(1, vm::load_cell_slice(root)));
  ASSERT_TRUE(!tlb::t_SnakeString.validate_exact_upto(0, vm::load_cell_slice(root)));
  ASSERT_TRUE(tlb::t_SnakeString.validate_exact_upto(1, vm::load_cell_slice(root)));
  auto budgeted_fetch = vm::load_cell_slice(root);
  int ops = 0;
  ASSERT_TRUE(tlb::t_SnakeString.validate_fetch(&ops, budgeted_fetch).is_null());
  ASSERT_EQ(ops, 0);
  ASSERT_TRUE(!budgeted_fetch.empty_ext());
  ops = 1;
  ASSERT_TRUE(tlb::t_SnakeString.validate_fetch(&ops, budgeted_fetch).not_null());
  ASSERT_EQ(ops, 0);
  ASSERT_TRUE(budgeted_fetch.empty_ext());
  auto root_cs = vm::load_cell_slice(root);
  ASSERT_EQ(tlb::t_SnakeString.load_snake_string(root_cs).move_as_ok(), "Hi");
  ASSERT_TRUE(root_cs.empty_ext());
  ASSERT_EQ(root_cs.get_base_cell()->get_hash(), root->get_hash());
  auto root_validate = vm::load_cell_slice(root);
  ASSERT_TRUE(tlb::t_SnakeString.validate(nullptr, root_validate));
  ASSERT_TRUE(tlb::t_SnakeString.validate_exact(nullptr, root_validate));
  ASSERT_TRUE(tlb::t_SnakeString.validate_fetch(nullptr, root_validate).not_null());
  ASSERT_TRUE(root_validate.empty_ext());

  vm::CellBuilder aligned;
  aligned.store_bytes("A", 1);
  auto aligned_cs = vm::load_cell_slice(aligned.finalize());
  ASSERT_EQ(tlb::t_SnakeString.load_snake_string(aligned_cs).move_as_ok(), "A");
  ASSERT_TRUE(aligned_cs.empty_ext());

  vm::CellBuilder second;
  second.store_bytes("\xa9", 1);
  vm::CellBuilder first;
  first.store_bytes("\xc3", 1).store_ref(second.finalize());
  ASSERT_EQ(print_cell(tlb::t_SnakeString, first.finalize(), ok), "\"é\"");
  ASSERT_TRUE(ok);

  vm::CellBuilder invalid;
  invalid.store_bytes("\xff", 1);
  print_cell(tlb::t_SnakeString, invalid.finalize(), ok);
  ASSERT_TRUE(!ok);

  vm::CellBuilder too_many_refs;
  too_many_refs.store_ref(byte_cell(1)).store_ref(byte_cell(2));
  print_cell(tlb::t_SnakeString, too_many_refs.finalize(), ok);
  ASSERT_TRUE(!ok);

  vm::CellBuilder partial;
  partial.store_long(1, 1);
  auto partial_cs = vm::load_cell_slice(partial.finalize());
  ASSERT_TRUE(!tlb::t_SnakeString.validate_skip(nullptr, partial_cs));
  ASSERT_TRUE(tlb::t_SnakeString.load_snake_binary(partial_cs).is_error());

  vm::CellBuilder bad_child;
  bad_child.store_long(1, 1);
  vm::CellBuilder bad_root_builder;
  auto bad_root = bad_root_builder.store_bytes("H", 1).store_ref(bad_child.finalize()).finalize();
  auto bad_root_cs = vm::load_cell_slice(bad_root);
  int bad_budget = 1;
  ASSERT_TRUE(tlb::t_SnakeString.validate_fetch(&bad_budget, bad_root_cs).is_null());
  ASSERT_EQ(bad_root_cs.get_base_cell()->get_hash(), bad_root->get_hash());
  ASSERT_EQ(bad_root_cs.prefetch_ulong(8), static_cast<unsigned>('H'));
  ASSERT_EQ(bad_root_cs.size_refs(), 1u);

  auto make_chain = [](unsigned cells) {
    td::Ref<vm::Cell> cell = vm::CellBuilder().finalize();
    for (unsigned i = 1; i < cells; ++i) {
      vm::CellBuilder parent;
      parent.store_ref(cell);
      cell = parent.finalize();
    }
    return cell;
  };
  auto thousand = vm::load_cell_slice(make_chain(1000));
  ASSERT_TRUE(tlb::t_SnakeString.validate_skip(nullptr, thousand));
  auto thousand_one = vm::load_cell_slice(make_chain(1001));
  ASSERT_TRUE(!tlb::t_SnakeString.validate_skip(nullptr, thousand_one));
}
