#pragma once

#include "td/utils/Slice.h"
#include "td/utils/buffer.h"

namespace ton {
// fec_info_none#c82a1964 = FecInfo;
//
// torrent_header#9128aab7
//   files_count:uint32
//   tot_name_size:uint64
//   tot_data_size:uint64
//   fec:FecInfo
//   dir_name_size:uint32
//   dir_name:(dir_name_size * [uint8])
//   name_index:(files_count * [uint64])
//   data_index:(files_count * [uint64])
//   names:(file_names_size * [uint8])
//   data:(tot_data_size * [uint8])
//     = TorrentHeader;

struct TorrentHeader {
  td::uint32 files_count{0};
  td::uint64 tot_names_size{0};
  td::uint64 tot_data_size{0};
  //fec_none
  std::string dir_name;
  std::vector<td::uint64> name_index;
  std::vector<td::uint64> data_index;
  std::string names;

  td::uint64 get_data_begin(td::uint64 file_i) const;
  td::uint64 get_data_end(td::uint64 file_i) const;
  td::uint64 get_data_offset(td::uint64 offset_i) const;
  td::uint64 get_data_size(td::uint64 file_i) const;
  td::Slice get_name(td::uint64 file_i) const;
  td::CSlice get_dir_name() const;
  td::uint32 get_files_count() const;

  td::uint64 serialization_size() const;
  td::BufferSlice serialize() const;

  static constexpr td::uint32 type = 0x9128aab7;
  template <class StorerT>
  void store(StorerT &storer) const;
  template <class ParserT>
  void parse(ParserT &parser);
};
}  // namespace ton
