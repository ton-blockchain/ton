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
#include "vm/db/CellStorage.h"
#include "vm/db/DynamicBagOfCellsDb.h"
#include "vm/boc.h"
#include "td/utils/base64.h"
#include "td/utils/tl_parsers.h"
#include "td/utils/tl_helpers.h"

namespace vm {
namespace {
class RefcntCellStorer {
 public:
  RefcntCellStorer(td::int32 refcnt, const td::Ref<DataCell> &cell, bool as_boc)
      : refcnt_(refcnt), cell_(cell), as_boc_(as_boc) {
  }

  template <class StorerT>
  void store(StorerT &storer) const {
    TD_PERF_COUNTER(cell_store);
    using td::store;
    if (as_boc_) {
      td::int32 tag = -1;
      store(tag, storer);
      store(refcnt_, storer);
      td::BufferSlice data = vm::std_boc_serialize(cell_).move_as_ok();
      storer.store_slice(data);
      return;
    }
    store(refcnt_, storer);
    store(*cell_, storer);
    for (unsigned i = 0; i < cell_->size_refs(); i++) {
      auto cell = cell_->get_ref(i);
      auto level_mask = cell->get_level_mask();
      auto level = level_mask.get_level();
      td::uint8 x = static_cast<td::uint8>(level_mask.get_mask());
      storer.store_slice(td::Slice(&x, 1));
      for (unsigned level_i = 0; level_i <= level; level_i++) {
        if (!level_mask.is_significant(level_i)) {
          continue;
        }
        storer.store_slice(cell->get_hash(level_i).as_slice());
      }
      for (unsigned level_i = 0; level_i <= level; level_i++) {
        if (!level_mask.is_significant(level_i)) {
          continue;
        }
        td::uint8 depth_buf[Cell::depth_bytes];
        DataCell::store_depth(depth_buf, cell->get_depth(level_i));
        storer.store_slice(td::Slice(depth_buf, Cell::depth_bytes));
      }
    }
  }

 private:
  td::int32 refcnt_;
  td::Ref<DataCell> cell_;
  bool as_boc_;
};

class RefcntCellParser {
 public:
  RefcntCellParser(bool need_data) : need_data_(need_data) {
  }
  td::int32 refcnt;
  Ref<DataCell> cell;
  bool stored_boc_;

  template <class ParserT>
  void parse(ParserT &parser, ExtCellCreator &ext_cell_creator) {
    using ::td::parse;
    parse(refcnt, parser);
    stored_boc_ = false;
    if (refcnt == -1) {
      stored_boc_ = true;
      parse(refcnt, parser);
    }
    if (!need_data_) {
      return;
    }
    auto status = [&]() -> td::Status {
      TRY_STATUS(parser.get_status());
      auto size = parser.get_left_len();
      td::Slice data = parser.template fetch_string_raw<td::Slice>(size);
      if (stored_boc_) {
        TRY_RESULT(boc, vm::std_boc_deserialize(data, false, true));
        TRY_RESULT(loaded_cell, boc->load_cell());
        cell = std::move(loaded_cell.data_cell);
        return td::Status::OK();
      }
      CellSerializationInfo info;
      auto cell_data = data;
      TRY_STATUS(info.init(cell_data, 0 /*ref_byte_size*/));
      data = data.substr(info.end_offset);

      Ref<Cell> refs[Cell::max_refs];
      for (int i = 0; i < info.refs_cnt; i++) {
        if (data.size() < 1) {
          return td::Status::Error("Not enought data");
        }
        Cell::LevelMask level_mask(data[0]);
        auto n = level_mask.get_hashes_count();
        auto end_offset = 1 + n * (Cell::hash_bytes + Cell::depth_bytes);
        if (data.size() < end_offset) {
          return td::Status::Error("Not enought data");
        }

        TRY_RESULT(ext_cell, ext_cell_creator.ext_cell(level_mask, data.substr(1, n * Cell::hash_bytes),
                                                       data.substr(1 + n * Cell::hash_bytes, n * Cell::depth_bytes)));
        refs[i] = std::move(ext_cell);
        CHECK(refs[i]->get_level() == level_mask.get_level());
        data = data.substr(end_offset);
      }
      if (!data.empty()) {
        return td::Status::Error("Too much data");
      }
      TRY_RESULT(data_cell, info.create_data_cell(cell_data, td::Span<Ref<Cell>>(refs, info.refs_cnt)));
      cell = std::move(data_cell);
      return td::Status::OK();
    }();
    if (status.is_error()) {
      parser.set_error(status.message().str());
      return;
    }
  }

 private:
  bool need_data_;
};
}  // namespace

CellLoader::CellLoader(std::shared_ptr<KeyValueReader> reader, std::function<void(const LoadResult &)> on_load_callback)
    : reader_(std::move(reader)), on_load_callback_(std::move(on_load_callback)) {
  CHECK(reader_);
}

td::Result<CellLoader::LoadResult> CellLoader::load(td::Slice hash, bool need_data, ExtCellCreator &ext_cell_creator) {
  //LOG(ERROR) << "Storage: load cell " << hash.size() << " " << td::base64_encode(hash);
  TD_PERF_COUNTER(cell_load);
  std::string serialized;
  TRY_RESULT(get_status, reader_->get(hash, serialized));
  if (get_status != KeyValue::GetStatus::Ok) {
    DCHECK(get_status == KeyValue::GetStatus::NotFound);
    return LoadResult{};
  }
  TRY_RESULT(res, load(hash, serialized, need_data, ext_cell_creator));
  if (on_load_callback_) {
    on_load_callback_(res);
  }
  return res;
}

td::Result<CellLoader::LoadResult> CellLoader::load(td::Slice hash, td::Slice value, bool need_data,
                                                    ExtCellCreator &ext_cell_creator) {
  LoadResult res;
  res.status = LoadResult::Ok;

  RefcntCellParser refcnt_cell(need_data);
  td::TlParser parser(value);
  refcnt_cell.parse(parser, ext_cell_creator);
  TRY_STATUS(parser.get_status());

  res.refcnt_ = refcnt_cell.refcnt;
  res.cell_ = std::move(refcnt_cell.cell);
  res.stored_boc_ = refcnt_cell.stored_boc_;
  //CHECK(res.cell_->get_hash() == hash);

  return res;
}

td::Result<CellLoader::LoadResult> CellLoader::load_refcnt(td::Slice hash) {
  LoadResult res;
  std::string serialized;
  TRY_RESULT(get_status, reader_->get(hash, serialized));
  if (get_status != KeyValue::GetStatus::Ok) {
    DCHECK(get_status == KeyValue::GetStatus::NotFound);
    return res;
  }
  res.status = LoadResult::Ok;
  td::TlParser parser(serialized);
  td::parse(res.refcnt_, parser);
  if (res.refcnt_ == -1) {
    parse(res.refcnt_, parser);
  }
  TRY_STATUS(parser.get_status());
  return res;
}

CellStorer::CellStorer(KeyValue &kv) : kv_(kv) {
}

td::Status CellStorer::erase(td::Slice hash) {
  return kv_.erase(hash);
}

std::string CellStorer::serialize_value(td::int32 refcnt, const td::Ref<DataCell> &cell, bool as_boc) {
  return td::serialize(RefcntCellStorer(refcnt, cell, as_boc));
}

td::Status CellStorer::set(td::int32 refcnt, const td::Ref<DataCell> &cell, bool as_boc) {
  return kv_.set(cell->get_hash().as_slice(), serialize_value(refcnt, cell, as_boc));
}
}  // namespace vm
