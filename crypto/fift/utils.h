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
#pragma once

#include "Fift.h"
#include <vector>

namespace fift {
struct FiftOutput {
  SourceLookup source_lookup;
  std::string output;
};

// given a valid Fift code PROGRAM{ ... }END>c, compile_asm_program() returns this output
// now it's used primarily for wasm output (see tolk-js, for example)
struct CompiledProgramOutput {
  std::string fiftCode;
  std::string codeBoc64;
  std::string codeHashHex;
};

td::Result<fift::SourceLookup> create_mem_source_lookup(std::string main, std::string fift_dir = "",
                                                        bool need_preamble = true, bool need_asm = true,
                                                        bool need_ton_util = true, bool need_lisp = true,
                                                        bool need_w3_code = true);
td::Result<FiftOutput> mem_run_fift(std::string source, std::vector<std::string> args = {}, std::string fift_dir = "");
td::Result<FiftOutput> mem_run_fift(SourceLookup source_lookup, std::vector<std::string> args);
td::Result<td::Ref<vm::Cell>> compile_asm(td::Slice asm_code);
td::Result<CompiledProgramOutput> compile_asm_program(std::string&& program_code, const std::string& fift_dir);
}  // namespace fift
