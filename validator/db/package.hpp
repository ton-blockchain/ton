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

  td::uint64 append(std::string filename, td::Slice data);
  td::uint64 size() const;
  td::Result<std::pair<std::string, td::BufferSlice>> read(td::uint64 offset) const;
  td::Result<td::uint64> advance(td::uint64 offset);

  struct Iterator {
    td::uint64 offset;
    Package &package;

    Iterator operator++(int);
    const Iterator operator++(int) const;
    td::Result<std::pair<std::string, td::BufferSlice>> read() const;
  };

  Iterator begin();
  const Iterator begin() const;
  Iterator end();
  const Iterator end() const;

 private:
  td::FileFd fd_;
};

}  // namespace ton
