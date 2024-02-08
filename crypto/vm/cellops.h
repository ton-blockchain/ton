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
#include "vm/cellslice.h"

namespace vm {

class OpcodeTable;

void register_cell_ops(OpcodeTable& cp0);

std::string dump_push_ref(CellSlice& cs, unsigned args, int pfx_bits, std::string name);
int compute_len_push_ref(const CellSlice& cs, unsigned args, int pfx_bits);

std::string dump_push_ref2(CellSlice& cs, unsigned args, int pfx_bits, std::string name);
int compute_len_push_ref2(const CellSlice& cs, unsigned args, int pfx_bits);

namespace util {

// "_q" functions throw on error if not quiet, return false if quiet (leaving cs unchanged)
bool load_int256_q(CellSlice& cs, td::RefInt256& res, int len, bool sgnd, bool quiet);
bool load_long_q(CellSlice& cs, td::int64& res, int len, bool quiet);
bool load_ulong_q(CellSlice& cs, td::uint64& res, int len, bool quiet);
bool load_ref_q(CellSlice& cs, td::Ref<Cell>& res, bool quiet);
bool load_maybe_ref_q(CellSlice& cs, td::Ref<Cell>& res, bool quiet);
bool skip_bits_q(CellSlice& cs, int bits, bool quiet);

// Non-"_q" functions throw on error
td::RefInt256 load_int256(CellSlice& cs, int len, bool sgnd);
td::int64 load_long(CellSlice& cs, int len);
td::uint64 load_ulong(CellSlice& cs, int len);
td::Ref<Cell> load_ref(CellSlice& cs);
td::Ref<Cell> load_maybe_ref(CellSlice& cs);
void check_have_bits(const CellSlice& cs, int bits);
void skip_bits(CellSlice& cs, int bits);
void end_parse(CellSlice& cs);

// store_... functions throw on error if not quiet, return false if quiet (leaving cb unchanged)
bool store_int256(CellBuilder& cb, const td::RefInt256& x, int len, bool sgnd, bool quiet = false);
bool store_long(CellBuilder& cb, td::int64 x, int len, bool quiet = false);
bool store_ulong(CellBuilder& cb, td::uint64 x, int len, bool quiet = false);
bool store_ref(CellBuilder& cb, td::Ref<Cell> x, bool quiet = false);
bool store_maybe_ref(CellBuilder& cb, td::Ref<Cell> x, bool quiet = false);
bool store_slice(CellBuilder& cb, const CellSlice& cs, bool quiet = false);

}  // namespace util

}  // namespace vm
