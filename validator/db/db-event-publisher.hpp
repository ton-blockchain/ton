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
#include <string>

#include "auto/tl/ton_api.h"
#include "td/actor/actor.h"
#include "td/actor/coro_utils.h"
#include "tl/TlObject.h"

namespace ton::validator {

class DbEventPublisher : public td::actor::Actor {
 public:
  explicit DbEventPublisher(std::string fifo_path) : fifo_path_(std::move(fifo_path)) {
  }
  void publish(tl_object_ptr<ton_api::db_Event> event);

 private:
  std::string fifo_path_;
  bool fifo_ready_ = false;
  bool disabled_ = false;
  bool ready_error_logged_ = false;
  bool write_error_logged_ = false;
  bool no_reader_logged_ = false;
  bool temp_error_logged_ = false;
  bool unsupported_logged_ = false;

#if TD_PORT_POSIX
  enum class WriteStatus { Ok, NoReader, TemporaryError, FatalError };
  td::Status ensure_ready();
  WriteStatus write_once(td::Slice data);
#endif
};

}  // namespace ton::validator