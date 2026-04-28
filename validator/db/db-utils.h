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
#pragma once
#include <set>
#include <string>

// FIXME: Remove once RocksDB stops triggering this warning.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-int-float-conversion"
#include "rocksdb/merge_operator.h"
#include "rocksdb/utilities/optimistic_transaction_db.h"
#pragma GCC diagnostic pop
#include "vm/db/CellStorage.h"

namespace ton::validator {

class PercentileStats {
 public:
  void insert(double value);
  std::string to_string() const;
  void clear();

 private:
  std::multiset<double> values_;
};

struct MergeOperatorAddCellRefcnt : public rocksdb::MergeOperator {
  const char* Name() const override {
    return "MergeOperatorAddCellRefcnt";
  }
  static auto to_td(rocksdb::Slice value) -> td::Slice {
    return td::Slice(value.data(), value.size());
  }
  bool FullMergeV2(const MergeOperationInput& merge_in, MergeOperationOutput* merge_out) const override {
    CHECK(merge_in.existing_value);
    auto& value = *merge_in.existing_value;
    CHECK(merge_in.operand_list.size() >= 1);
    td::Slice diff;
    std::string diff_buf;
    if (merge_in.operand_list.size() == 1) {
      diff = to_td(merge_in.operand_list[0]);
    } else {
      diff_buf = merge_in.operand_list[0].ToString();
      for (size_t i = 1; i < merge_in.operand_list.size(); ++i) {
        vm::CellStorer::merge_refcnt_diffs(diff_buf, to_td(merge_in.operand_list[i]));
      }
      diff = diff_buf;
    }

    merge_out->new_value = value.ToString();
    vm::CellStorer::merge_value_and_refcnt_diff(merge_out->new_value, diff);
    return true;
  }
  bool PartialMerge(const rocksdb::Slice& /*key*/, const rocksdb::Slice& left, const rocksdb::Slice& right,
                    std::string* new_value, rocksdb::Logger* logger) const override {
    *new_value = left.ToString();
    vm::CellStorer::merge_refcnt_diffs(*new_value, to_td(right));
    return true;
  }
};

class CellDbAsyncExecutor : public vm::DynamicBagOfCellsDb::AsyncExecutor {
 public:
  explicit CellDbAsyncExecutor(td::actor::ActorId<> actor) : actor_(std::move(actor)) {
  }

  void execute_async(std::function<void()> f) override {
    class Runner : public td::actor::Actor {
     public:
      explicit Runner(std::function<void()> f) : f_(std::move(f)) {
      }
      void start_up() override {
        f_();
        stop();
      }

     private:
      std::function<void()> f_;
    };
    td::actor::create_actor<Runner>("executeasync", std::move(f)).release();
  }

  void execute_sync(std::function<void()> f) override {
    td::actor::send_lambda(actor_, std::move(f));
  }

 private:
  td::actor::ActorId<> actor_;
};

}  // namespace ton::validator
