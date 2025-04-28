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
#include "td/db/KeyValue.h"
#include "vm/db/DynamicBagOfCellsDb.h"
#include "vm/cells.h"

#include "td/utils/Slice.h"
#include "td/utils/Status.h"

namespace vm {
using KeyValue = td::KeyValue;
using KeyValueReader = td::KeyValueReader;

class CellLoader {
 public:
  struct LoadResult {
   public:
    enum { Ok, NotFound } status{NotFound};

    Ref<DataCell> &cell() {
      DCHECK(status == Ok);
      return cell_;
    }

    td::int32 refcnt() const {
      return refcnt_;
    }

    Ref<DataCell> cell_;
    td::int32 refcnt_{0};
    bool stored_boc_{false};
  };
  CellLoader(std::shared_ptr<KeyValueReader> reader, std::function<void(const LoadResult &)> on_load_callback = {});
  td::Result<LoadResult> load(td::Slice hash, bool need_data, ExtCellCreator &ext_cell_creator);
  td::Result<std::vector<LoadResult>> load_bulk(td::Span<td::Slice> hashes, bool need_data, ExtCellCreator &ext_cell_creator);
  static td::Result<LoadResult> load(td::Slice hash, td::Slice value, bool need_data, ExtCellCreator &ext_cell_creator);
  td::Result<LoadResult> load_refcnt(td::Slice hash);  // This only loads refcnt_, cell_ == null
  KeyValueReader &key_value_reader() const {
    return *reader_;
  }

 private:
  std::shared_ptr<KeyValueReader> reader_;
  std::function<void(const LoadResult &)> on_load_callback_;
};

class CellStorer {
 public:
  CellStorer(KeyValue &kv);
  td::Status erase(td::Slice hash);
  td::Status set(td::int32 refcnt, const td::Ref<DataCell> &cell, bool as_boc);
  td::Status merge(td::Slice hash, td::int32 refcnt_diff);

  static void merge_value_and_refcnt_diff(std::string &value, td::Slice right);
  static void merge_refcnt_diffs(std::string &left, td::Slice right);
  static std::string serialize_refcnt_diffs(td::int32 refcnt_diff);

  static std::string serialize_value(td::int32 refcnt, const td::Ref<DataCell> &cell, bool as_boc);

  struct Diff {
    enum Type { Set, Erase, Merge } type{Set};
    CellHash key;
    std::string value{};
  };
  td::Status apply_diff(const Diff &diff);

  struct MetaDiff {
    enum Type { Set, Erase } type{Set};
    std::string key;
    std::string value{};
  };
  td::Status apply_meta_diff(const MetaDiff &diff);

 private:
  KeyValue &kv_;
};
}  // namespace vm
