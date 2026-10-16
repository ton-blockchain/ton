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
#include "td/utils/tests.h"
#include "vm/cells.h"

#include "tlbc-generator-auto.h"
#include "tlbc-generator-default-auto.h"

namespace {

std::string generated_json(const tlb::TLB& type, td::Ref<vm::Cell> cell) {
  tlb::JsonPrinter pp;
  ASSERT_TRUE(type.print_ref(pp, std::move(cell)));
  ASSERT_TRUE(pp.ok());
  return pp.get_json();
}

}  // namespace

TEST(TlbcGenerator, UnnamedBoolAndNamedField) {
  vm::CellBuilder cb;
  cb.store_long(1, 1).store_long(0, 1);
  ASSERT_EQ(generated_json(tlbc_generator_test::t_UnnamedBool, cb.finalize()),
            R"({"@type":"unnamed","@0":true,"named":false})");
}

TEST(TlbcGenerator, IdentifierCollisions) {
  vm::CellBuilder cb;
  cb.store_long(7, 8).store_long(9, 8);
  auto json = generated_json(tlbc_generator_test::t_Collision, cb.finalize());
  ASSERT_TRUE(json.find("\"@type\":\"cons_name\"") != std::string::npos);
  ASSERT_TRUE(json.find("\"cons_name\":\"7\"") != std::string::npos);
  ASSERT_TRUE(json.find("\"JsonPrinter\":\"9\"") != std::string::npos);

  // The generated C++ class cannot be named tlb::JsonPrinter (imported via the header).
  cb.reset();
  cb.store_long(1, 1);
  ASSERT_EQ(generated_json(tlbc_generator_test::t_JsonPrinter1, cb.finalize()),
            R"({"@type":"json_printer","cons_name":true})");
}

TEST(TlbcGenerator, StandardBlockSchemaStillWorks) {
  td::Ref<vm::Cell> cell;
  ASSERT_TRUE(block::gen::t_Bit.cell_pack_bit(cell, true));
  ASSERT_EQ(generated_json(block::gen::t_Bit, cell), R"({"@type":"bit","@0":"1"})");
}

TEST(TlbcGenerator, BuiltinSnakeStringInDefaultNamespace) {
  vm::CellBuilder cb;
  cb.store_bytes("A", 1);
  ASSERT_EQ(generated_json(tlb::t_SnakeHolder, cb.finalize()), R"({"@type":"snake_holder","value":"A"})");
}
