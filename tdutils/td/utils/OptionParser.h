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

#include "td/utils/common.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/StringBuilder.h"

#include <functional>

namespace td {

class OptionParser {
  class Option {
   public:
    enum class Type { NoArg, Arg };
    Type type;
    char short_key;
    string long_key;
    string description;
    std::function<Status(Slice)> arg_callback;
  };

  void add_option(Option::Type type, char short_key, Slice long_key, Slice description,
                  std::function<Status(Slice)> callback);

 public:
  void set_description(string description);

  void add_checked_option(char short_key, Slice long_key, Slice description, std::function<Status(Slice)> callback);

  void add_checked_option(char short_key, Slice long_key, Slice description, std::function<Status(void)> callback);

  void add_option(char short_key, Slice long_key, Slice description, std::function<Status(Slice)> callback) = delete;

  void add_option(char short_key, Slice long_key, Slice description, std::function<Status(void)> callback) = delete;

  void add_option(char short_key, Slice long_key, Slice description, std::function<void(Slice)> callback);

  void add_option(char short_key, Slice long_key, Slice description, std::function<void(void)> callback);

  void add_check(std::function<Status()> check);

  // returns found non-option parameters
  Result<vector<char *>> run(int argc, char *argv[], int expected_non_option_count = -1) TD_WARN_UNUSED_RESULT;

  friend StringBuilder &operator<<(StringBuilder &sb, const OptionParser &o);

 private:
  vector<Option> options_;
  vector<std::function<Status()>> checks_;
  string description_;
};

}  // namespace td
