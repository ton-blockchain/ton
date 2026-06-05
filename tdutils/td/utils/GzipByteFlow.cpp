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
#include "td/utils/GzipByteFlow.h"

char disable_linker_warning_about_empty_file_gzipbyteflow_cpp TD_UNUSED;

#if TD_HAVE_ZLIB
#include "td/utils/Status.h"
#include "td/utils/common.h"

namespace td {

bool GzipByteFlow::loop() {
  if (gzip_.need_input()) {
    auto slice = input_->prepare_read();
    if (slice.empty()) {
      if (!is_input_active_) {
        gzip_.close_input();
      } else {
        return false;
      }
    } else {
      gzip_.set_input(input_->prepare_read());
    }
  }
  if (gzip_.need_output()) {
    auto slice = output_.prepare_append();
    CHECK(!slice.empty());
    gzip_.set_output(slice);
  }
  auto r_state = gzip_.run();
  auto output_size = gzip_.flush_output();
  if (output_size) {
    if (output_size > max_output_size_ || total_output_size_ > max_output_size_ - output_size) {
      finish(Status::Error("Max output size limit exceeded"));
      return false;
    }
    total_output_size_ += output_size;
    output_.confirm_append(output_size);
  }

  auto input_size = gzip_.flush_input();
  if (input_size) {
    input_->confirm_read(input_size);
  }
  if (r_state.is_error()) {
    finish(r_state.move_as_error());
    return false;
  }
  auto state = r_state.ok();
  if (state == Gzip::State::Done) {
    consume_input();
    return false;
  }
  return true;
}

}  // namespace td

#endif
