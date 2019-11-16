#pragma once

#include "td/actor/actor.h"
#include "td/utils/port/FileFd.h"
#include "td/utils/buffer.h"

namespace ton {

class Package {
 public:
  static td::Result<Package> open(std::string path, bool read_only = false, bool create = false);

  Package(td::FileFd fd);

  td::Status truncate(td::uint64 size);

  td::uint64 append(std::string filename, td::Slice data, bool sync = true);
  void sync();
  td::uint64 size() const;
  td::Result<std::pair<std::string, td::BufferSlice>> read(td::uint64 offset) const;

  td::Result<td::uint64> advance(td::uint64 offset);
  void iterate(std::function<bool(std::string, td::BufferSlice, td::uint64)> func);

  td::FileFd &fd() {
    return fd_;
  }

 private:
  td::FileFd fd_;
};

}  // namespace ton
