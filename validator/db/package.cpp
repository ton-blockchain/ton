#include "package.hpp"
#include "common/errorcode.h"

namespace ton {

namespace {

constexpr td::uint32 header_size() {
  return 4;
}

constexpr td::uint32 max_data_size() {
  return (1u << 31) - 1;
}

constexpr td::uint32 max_filename_size() {
  return (1u << 16) - 1;
}

constexpr td::uint16 entry_header_magic() {
  return 0x1e8b;
}

constexpr td::uint32 package_header_magic() {
  return 0xae8fdd01;
}
}  // namespace

Package::Package(td::FileFd fd) : fd_(std::move(fd)) {
}

td::Status Package::truncate(td::uint64 size) {
  TRY_STATUS(fd_.seek(size + header_size()));
  return fd_.truncate_to_current_position(size + header_size());
}

td::uint64 Package::append(std::string filename, td::Slice data, bool sync) {
  CHECK(data.size() <= max_data_size());
  CHECK(filename.size() <= max_filename_size());
  auto size = fd_.get_size().move_as_ok();
  auto orig_size = size;
  td::uint32 header[2];
  header[0] = entry_header_magic() + (td::narrow_cast<td::uint32>(filename.size()) << 16);
  header[1] = td::narrow_cast<td::uint32>(data.size());
  CHECK(fd_.pwrite(td::Slice(reinterpret_cast<const td::uint8*>(header), 8), size).move_as_ok() == 8);
  size += 8;
  CHECK(fd_.pwrite(filename, size).move_as_ok() == filename.size());
  size += filename.size();
  CHECK(fd_.pwrite(data, size).move_as_ok() == data.size());
  size += data.size();
  if (sync) {
    fd_.sync().ensure();
  }
  return orig_size - header_size();
}

void Package::sync() {
  fd_.sync().ensure();
}

td::uint64 Package::size() const {
  return fd_.get_size().move_as_ok() - header_size();
}

td::Result<std::pair<std::string, td::BufferSlice>> Package::read(td::uint64 offset) const {
  offset += header_size();

  td::uint32 header[2];
  TRY_RESULT(s1, fd_.pread(td::MutableSlice(reinterpret_cast<td::uint8*>(header), 8), offset));
  if (s1 != 8) {
    return td::Status::Error(ErrorCode::notready, "too short read");
  }
  if ((header[0] & 0xffff) != entry_header_magic()) {
    return td::Status::Error(ErrorCode::notready, "bad entry magic");
  }
  offset += 8;
  auto fname_size = header[0] >> 16;
  auto data_size = header[1];

  std::string fname(fname_size, '\0');
  TRY_RESULT(s2, fd_.pread(fname, offset));
  if (s2 != fname_size) {
    return td::Status::Error(ErrorCode::notready, "too short read (filename)");
  }
  offset += fname_size;

  td::BufferSlice data{data_size};
  TRY_RESULT(s3, fd_.pread(data.as_slice(), offset));
  if (s3 != data_size) {
    return td::Status::Error(ErrorCode::notready, "too short read (data)");
  }
  return std::pair<std::string, td::BufferSlice>{std::move(fname), std::move(data)};
}

td::Result<td::uint64> Package::advance(td::uint64 offset) {
  offset += header_size();

  td::uint32 header[2];
  TRY_RESULT(s1, fd_.pread(td::MutableSlice(reinterpret_cast<td::uint8*>(header), 8), offset));
  if (s1 != 8) {
    return td::Status::Error(ErrorCode::notready, "too short read");
  }
  if ((header[0] & 0xffff) != entry_header_magic()) {
    return td::Status::Error(ErrorCode::notready, "bad entry magic");
  }

  offset += 8 + (header[0] >> 16) + header[1];
  if (offset > static_cast<td::uint64>(fd_.get_size().move_as_ok())) {
    return td::Status::Error(ErrorCode::notready, "truncated read");
  }
  return offset - header_size();
}

td::Result<Package> Package::open(std::string path, bool read_only, bool create) {
  td::uint32 flags = td::FileFd::Flags::Read;
  if (!read_only) {
    flags |= td::FileFd::Write;
  }
  if (create) {
    flags |= td::FileFd::Create;
  }

  TRY_RESULT(fd, td::FileFd::open(path, flags));
  TRY_RESULT(size, fd.get_size());

  if (size < header_size()) {
    if (!create) {
      return td::Status::Error(ErrorCode::notready, "db is too short");
    }
    td::uint32 header[1];
    header[0] = package_header_magic();
    TRY_RESULT(s, fd.pwrite(td::Slice(reinterpret_cast<const td::uint8*>(header), header_size()), size));
    if (s != header_size()) {
      return td::Status::Error(ErrorCode::notready, "db write is short");
    }
  } else {
    td::uint32 header[1];
    TRY_RESULT(s, fd.pread(td::MutableSlice(reinterpret_cast<td::uint8*>(header), header_size()), 0));
    if (s != header_size()) {
      return td::Status::Error(ErrorCode::notready, "db read failed");
    }
    if (header[0] != package_header_magic()) {
      return td::Status::Error(ErrorCode::notready, "magic mismatch");
    }
  }
  return Package{std::move(fd)};
}

void Package::iterate(std::function<bool(std::string, td::BufferSlice, td::uint64)> func) {
  td::uint64 p = 0;

  td::uint64 size = fd_.get_size().move_as_ok();
  if (size < header_size()) {
    LOG(ERROR) << "too short archive";
    return;
  }
  size -= header_size();
  while (p != size) {
    auto R = read(p);
    if (R.is_error()) {
      LOG(ERROR) << "broken archive: " << R.move_as_error();
      return;
    }
    auto q = R.move_as_ok();
    if (!func(q.first, q.second.clone(), p)) {
      break;
    }

    p = advance(p).move_as_ok();
  }
}

}  // namespace ton
