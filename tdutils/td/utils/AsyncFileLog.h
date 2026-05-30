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

#include <memory>

#include "td/utils/Status.h"
#include "td/utils/common.h"
#include "td/utils/logging.h"

namespace td {

// Asynchronous file LogInterface.
// Producers copy formatted lines into per-thread SPSC rings. A writer thread drains the rings into one
// rotatable file. LOG(FATAL) asks the writer to flush pending data and then writes the fatal line synchronously.
class AsyncFileLog : public LogInterface {
 public:
  static constexpr int64 DEFAULT_ROTATE_THRESHOLD = 10 * (1 << 20);
  static constexpr uint32 DEFAULT_RING_CAPACITY = 1u << 20;  // 1 MB per thread, LLC-resident

  AsyncFileLog();
  AsyncFileLog(const AsyncFileLog &) = delete;
  AsyncFileLog &operator=(const AsyncFileLog &) = delete;
  ~AsyncFileLog() override;

  Status init(string path, int64 rotate_threshold = DEFAULT_ROTATE_THRESHOLD, bool redirect_stderr = true,
              uint32 ring_capacity = DEFAULT_RING_CAPACITY) TD_WARN_UNUSED_RESULT;

  static Result<td::unique_ptr<LogInterface>> create(string path, int64 rotate_threshold = DEFAULT_ROTATE_THRESHOLD,
                                                     bool redirect_stderr = true,
                                                     uint32 ring_capacity = DEFAULT_RING_CAPACITY);

  void append(CSlice slice, int log_level) override;
  void rotate() override;
  vector<string> get_file_paths() override;
  uint64 get_dropped_count() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace td
