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
#include "td/fec/algebra/InactivationDecoding.h"

#include "td/utils/logging.h"

#include <algorithm>

namespace td {
InactivationDecodingResult InactivationDecoding::run() {
  init();
  loop();

  LOG(DEBUG) << tag("A_small.cols", L_.cols() - p_rows_.size()) << tag("Total columns", L_.cols()) << tag("PI", PI_)
             << tag("A_small.cols - PI", L_.cols() - PI_ - p_rows_.size());
  for (uint32 row = 0; row < rows_; row++) {
    if (!was_row_[row]) {
      p_rows_.push_back(row);
    }
  }

  uint32 side = narrow_cast<uint32>(p_cols_.size());
  std::reverse(inactive_cols_.begin(), inactive_cols_.end());
  for (auto col : inactive_cols_) {
    p_cols_.push_back(col);
  }
  for (uint32 i = 0; i < PI_; i++) {
    p_cols_.push_back(cols_ + i);
  }
  check(side);
  return {side, std::move(p_rows_), std::move(p_cols_)};
}

void InactivationDecoding::init() {
  was_row_ = vector<bool>(rows_, false);
  was_col_ = vector<bool>(cols_, false);

  col_cnt_ = vector<uint32>(cols_, 0);
  row_cnt_ = vector<uint32>(rows_, 0);
  row_xor_ = vector<uint32>(rows_, 0);
  L_.generate([&](auto row, auto col) {
    if (col >= cols_) {
      return;
    }
    col_cnt_[col]++;
    row_cnt_[row]++;
    row_xor_[row] ^= col;
  });

  sort_rows();
}

void InactivationDecoding::loop() {
  while (row_cnt_offset_[1] != rows_) {
    auto row = sorted_rows_[row_cnt_offset_[1]];
    uint32 col = choose_col(row);
    LOG_CHECK(col_cnt_[col] >= 1) << col;

    auto cnt = row_cnt_[row];
    CHECK(row_cnt_offset_[cnt] == row_cnt_offset_[1]);
    CHECK(row_pos_[row] == row_cnt_offset_[1]);
    p_cols_.push_back(col);
    p_rows_.push_back(row);

    if (cnt == 1) {
      inactivate_col(col);
    } else {
      for (auto x : L_rows_.col(row)) {
        if (x >= cols_ || was_col_[x]) {
          continue;
        }
        if (x != col) {
          inactive_cols_.push_back(x);
        }
        inactivate_col(x);
      }
    }
    was_row_[row] = true;
  }
}

void InactivationDecoding::check_sorted() {
  for (size_t i = 0; i < rows_; i++) {
    CHECK(sorted_rows_[row_pos_[i]] == i);
  }
  for (size_t i = 1; i < rows_; i++) {
    CHECK(row_cnt_[sorted_rows_[i - 1]] <= row_cnt_[sorted_rows_[i]]);
  }
  for (size_t i = 1; i <= cols_ + 1; i++) {
    CHECK(row_cnt_offset_[i - 1] <= row_cnt_offset_[i]);
  }
  for (size_t i = 0; i < rows_; i++) {
    auto pos = row_pos_[i];
    auto cnt = row_cnt_[i];
    CHECK(pos >= row_cnt_offset_[cnt]);
    CHECK(pos < row_cnt_offset_[cnt + 1]);
  }
}

uint32 InactivationDecoding::choose_col(uint32 row) {
  auto cnt = row_cnt_[row];
  if (cnt == 1) {
    return row_xor_[row];
  }
  uint32 best_col = uint32(-1);
  for (auto col : L_rows_.col(row)) {
    if (col >= cols_ || was_col_[col]) {
      continue;
    }
    DCHECK(col_cnt_[col] >= 1);
    if (best_col == uint32(-1) || col_cnt_[col] < col_cnt_[best_col]) {
      best_col = col;
    }
  }
  DCHECK(best_col != uint32(-1));
  return best_col;
}

void InactivationDecoding::inactivate_col(uint32 col) {
  was_col_[col] = true;
  for (auto row : L_.col(col)) {
    if (was_row_[row]) {
      continue;
    }
    auto pos = row_pos_[row];
    DCHECK(sorted_rows_[pos] == row);
    auto cnt = row_cnt_[row];
    LOG_DCHECK(cnt >= 1) << row << " " << col;
    auto offset = row_cnt_offset_[cnt];
    std::swap(sorted_rows_[pos], sorted_rows_[offset]);
    row_pos_[sorted_rows_[pos]] = pos;
    row_pos_[sorted_rows_[offset]] = offset;
    row_cnt_offset_[cnt]++;
    row_cnt_[row]--;
    row_xor_[row] ^= col;
  }
}

void InactivationDecoding::sort_rows() {
  vector<uint32> offset(cols_ + 2, 0);
  for (size_t i = 0; i < rows_; i++) {
    offset[row_cnt_[i] + 1]++;
  }
  for (size_t i = 1; i <= cols_ + 1; i++) {
    offset[i] += offset[i - 1];
  }
  row_cnt_offset_ = offset;

  sorted_rows_.resize(rows_);
  row_pos_.resize(rows_);
  for (uint32 i = 0; i < rows_; i++) {
    auto pos = offset[row_cnt_[i]]++;
    sorted_rows_[pos] = i;
    row_pos_[i] = pos;
  }
}

void InactivationDecoding::check(uint32 side) {
  auto inv_p_cols = inverse_permutation(p_cols_);
  auto inv_p_rows = inverse_permutation(p_rows_);
  for (uint32 i = 0; i < side; i++) {
    CHECK(inv_p_cols[p_cols_[i]] == i);
    auto col = L_.col(p_cols_[i]);
    CHECK(col.size() >= 1);
    for (auto x : col) {
      CHECK(inv_p_rows[x] >= i);
    }
  }
}
}  // namespace td
