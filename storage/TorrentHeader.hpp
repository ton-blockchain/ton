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

#include "td/utils/tl_helpers.h"

#include "TorrentHeader.h"
namespace ton {
template <class StorerT>
void TorrentHeader::store(StorerT &storer) const {
  using td::store;
  store(type, storer);
  store(files_count, storer);
  store(tot_names_size, storer);
  store(tot_data_size, storer);
  td::uint32 fec_type = 0xc82a1964;
  store(fec_type, storer);
  td::uint32 dir_name_size = td::narrow_cast<td::uint32>(dir_name.size());
  store(dir_name_size, storer);
  storer.store_slice(dir_name);
  CHECK(name_index.size() == files_count);
  CHECK(data_index.size() == files_count);
  for (auto x : name_index) {
    store(x, storer);
  }
  for (auto x : data_index) {
    store(x, storer);
  }
  CHECK(tot_names_size == names.size());
  storer.store_slice(names);
}

template <class ParserT>
void TorrentHeader::parse(ParserT &parser) {
  using td::parse;
  td::uint32 got_type;
  parse(got_type, parser);
  if (got_type != type) {
    parser.set_error("Unknown type");
    return;
  }
  parse(files_count, parser);
  parse(tot_names_size, parser);
  parse(tot_data_size, parser);
  td::uint32 fec_type;
  parse(fec_type, parser);
  td::uint32 dir_name_size;
  parse(dir_name_size, parser);
  dir_name = parser.template fetch_string_raw<std::string>(dir_name_size);
  if (fec_type != 0xc82a1964) {
    parser.set_error("Unknown fec type");
    return;
  }
  name_index.resize(files_count);
  for (auto &x : name_index) {
    parse(x, parser);
  }
  data_index.resize(files_count);
  for (auto &x : data_index) {
    parse(x, parser);
  }
  names = parser.template fetch_string_raw<std::string>(tot_names_size);
}
}  // namespace ton
