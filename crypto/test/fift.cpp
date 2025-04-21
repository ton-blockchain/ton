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
#include "fift/words.h"
#include "fift/Fift.h"
#include "fift/utils.h"

#include "td/utils/tests.h"
#include "td/utils/PathView.h"
#include "td/utils/port/path.h"
#include "td/utils/filesystem.h"

std::string current_dir() {
  return td::PathView(td::realpath(__FILE__).move_as_ok()).parent_dir().str();
}

std::string load_test(std::string name) {
  return td::read_file_str(current_dir() + "fift/" + name).move_as_ok();
}

td::Status run_fift(std::string name, bool expect_error = false) {
  auto res = fift::mem_run_fift(load_test(name));
  if (expect_error) {
    res.ensure_error();
    return td::Status::OK();
  }
  res.ensure();
  REGRESSION_VERIFY(res.ok().output);
  return td::Status::OK();
}

TEST(Fift, testvm) {
  run_fift("testvm.fif");
}
TEST(Fift, testvm2) {
  run_fift("testvm2.fif");
}
TEST(Fift, testvm3) {
  run_fift("testvm3.fif");
}
TEST(Fift, testvm4) {
  run_fift("testvm2.fif");
}
TEST(Fift, testvm4a) {
  run_fift("testvm4a.fif");
}
TEST(Fift, testvm4b) {
  run_fift("testvm4b.fif");
}
TEST(Fift, testvm4c) {
  run_fift("testvm4c.fif");
}
TEST(Fift, testvm4d) {
  run_fift("testvm4d.fif");
}
TEST(Fift, testvm5) {
  run_fift("testvm5.fif");
}
TEST(Fift, testvm6) {
  run_fift("testvm6.fif");
}
TEST(Fift, testvm7) {
  run_fift("testvm7.fif");
}
TEST(Fift, testvm8) {
  run_fift("testvm8.fif");
}
TEST(Fift, testvmprog) {
  run_fift("testvmprog.fif");
}
TEST(Fift, bug) {
  run_fift("bug.fif", true);
}
TEST(Fift, contfrac) {
  run_fift("contfrac.fif");
}
TEST(Fift, test) {
  run_fift("test.fif");
}
TEST(Fift, bug_div) {
  run_fift("bug_div.fif");
}

TEST(Fift, bug_ufits) {
  run_fift("bug_ufits.fif");
}

TEST(Fift, test_dict) {
  run_fift("testdict.fif");
}

TEST(Fift, test_fixed) {
  run_fift("fixed.fif");
}

TEST(Fift, test_sort) {
  run_fift("sort.fif");
}

TEST(Fift, test_sort2) {
  run_fift("sort2.fif");
}

TEST(Fift, test_hmap) {
  run_fift("hmap.fif");
}

TEST(Fift, test_disasm) {
  run_fift("disasm.fif");
}

TEST(Fift, test_fiftext) {
  run_fift("fift-ext.fif");
}

TEST(Fift, test_namespaces) {
  run_fift("namespaces.fif");
}

TEST(Fift, test_asm_nested_program) {
  run_fift("asm-nested-program.fif");
}

TEST(Fift, test_adddiv) {
  run_fift("adddiv.fif");
}

TEST(Fift, test_tvm_runvm) {
  run_fift("tvm_runvm.fif");
}

TEST(Fift, test_hash_ext) {
  run_fift("hash_ext.fif");
}

TEST(Fift, test_deep_stack_ops) {
  run_fift("deep_stack_ops.fif");
}

TEST(Fift, test_rist255) {
  run_fift("rist255.fif");
}

TEST(Fift, test_bls) {
  run_fift("bls.fif");
}

TEST(Fift, test_bls_ops) {
  run_fift("bls_ops.fif");
}

TEST(Fift, test_levels) {
  run_fift("levels.fif");
}

TEST(Fift, test_secp256k1) {
  run_fift("secp256k1.fif");
}

TEST(Fift, test_get_extra_balance) {
  run_fift("get_extra_balance.fif");
}
