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

    Copyright 2017-2019 Telegram Systems LLP
*/
#pragma once

#include "td/utils/port/path.h"
#include "td/utils/filesystem.h"
#include "td/actor/actor.h"
#include "td/utils/buffer.h"

#include "common/errorcode.h"

namespace ton {

namespace validator {

namespace db {

class WriteFile : public td::actor::Actor {
 public:
  void start_up() override {
    auto R = [&]() {
      td::uint32 cnt = 0;
      while (true) {
        cnt++;
        auto res = td::mkstemp(td::CSlice{tmp_dir_});
        if (res.is_ok() || cnt >= 10) {
          return res;
        }
      }
    }();
    if (R.is_error()) {
      promise_.set_error(R.move_as_error());
      stop();
      return;
    }
    auto res = R.move_as_ok();
    auto file = std::move(res.first);
    auto old_name = res.second;
    size_t offset = 0;
    while (data_.size() > 0) {
      auto R = file.pwrite(data_.as_slice(), offset);
      auto s = R.move_as_ok();
      offset += s;
      data_.confirm_read(s);
    }
    file.sync().ensure();
    if (new_name_.length() > 0) {
      td::rename(old_name, new_name_).ensure();
      promise_.set_value(std::move(new_name_));
    } else {
      promise_.set_value(std::move(old_name));
    }
    stop();
  }
  WriteFile(std::string tmp_dir, std::string new_name, td::BufferSlice data, td::Promise<std::string> promise)
      : tmp_dir_(tmp_dir), new_name_(new_name), data_(std::move(data)), promise_(std::move(promise)) {
  }

 private:
  const std::string tmp_dir_;
  std::string new_name_;
  td::BufferSlice data_;
  td::Promise<std::string> promise_;
};

class ReadFile : public td::actor::Actor {
 public:
  enum Flags : td::uint32 { f_disable_log = 1 };
  void start_up() override {
    auto S = td::read_file(file_name_, max_length_, offset_);
    if (S.is_ok()) {
      promise_.set_result(S.move_as_ok());
    } else {
      // TODO check error code
      if (flags_ & Flags::f_disable_log) {
        LOG(DEBUG) << "missing file " << file_name_;
      } else {
        LOG(ERROR) << "missing file " << file_name_;
      }
      promise_.set_error(td::Status::Error(ErrorCode::notready, "file does not exist"));
    }
    stop();
  }
  ReadFile(std::string file_name, td::int64 offset, td::int64 max_length, td::uint32 flags,
           td::Promise<td::BufferSlice> promise)
      : file_name_(file_name), offset_(offset), max_length_(max_length), flags_(flags), promise_(std::move(promise)) {
  }

 private:
  std::string file_name_;
  td::int64 offset_;
  td::int64 max_length_;
  td::uint32 flags_;
  td::Promise<td::BufferSlice> promise_;
};

}  // namespace db

}  // namespace validator

}  // namespace ton
