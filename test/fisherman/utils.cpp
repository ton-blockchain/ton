#include "utils.hpp"

#include <fstream>

namespace test::fisherman {

td::Result<td::BufferSlice> read_file_to_buffer(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return td::Status::Error("Cannot open file: " + path);
  }

  in.seekg(0, std::ios::end);
  std::streamoff file_size = in.tellg();
  if (file_size < 0) {
    return td::Status::Error("Failed to get file size: " + path);
  }
  in.seekg(0, std::ios::beg);

  auto size = static_cast<size_t>(file_size);
  td::BufferWriter writer(size);

  td::MutableSlice out_slice = writer.prepare_append();
  if (out_slice.size() < size) {
    return td::Status::Error("Not enough memory allocated in BufferWriter");
  }

  if (!in.read(reinterpret_cast<char *>(out_slice.data()), size)) {
    return td::Status::Error("Failed to read file contents: " + path);
  }
  writer.confirm_append(size);

  return writer.as_buffer_slice();
}

td::Result<ton::BlockIdExt> parse_block_id_from_json(td::JsonValue jv) {
  using td::Result;
  using td::Status;

  if (jv.type() != td::JsonValue::Type::Object) {
    return Status::Error("Root JSON is not an object");
  }
  auto &obj = jv.get_object();

  auto res_wc = td::get_json_object_int_field(obj, PSLICE() << "workchain_id", false);
  if (res_wc.is_error()) {
    return Status::Error("Missing or invalid 'workchain_id'");
  }
  int32_t workchain_id = res_wc.move_as_ok();

  auto res_shard_str = td::get_json_object_string_field(obj, PSLICE() << "shard_id", false);
  if (res_shard_str.is_error()) {
    return Status::Error("Missing or invalid 'shard_id'");
  }
  std::string shard_str = res_shard_str.move_as_ok();
  uint64_t shard_id = 0;
  try {
    if (shard_str.starts_with("0x")) {
      shard_str.erase(0, 2);
    }
    shard_id = std::stoull(shard_str, nullptr, 16);
  } catch (...) {
    return Status::Error("Failed to parse shard_id from: " + shard_str);
  }

  auto res_seqno = td::get_json_object_int_field(obj, PSLICE() << "seqno", false);
  if (res_seqno.is_error()) {
    return Status::Error("Missing or invalid 'seqno'");
  }
  int32_t seqno_signed = res_seqno.move_as_ok();
  if (seqno_signed < 0) {
    return Status::Error("seqno must be non-negative");
  }

  return ton::BlockIdExt{workchain_id, shard_id, static_cast<uint32_t>(seqno_signed), ton::RootHash::zero(),
                         ton::FileHash::zero()};
}

}  // namespace test::fisherman
