#pragma once

#include "TorrentHeader.h"
#include "TorrentInfo.h"

#include "td/utils/optional.h"

namespace ton {
//
// torrent_file#6a7181e0 flags:(## 32) info_boc_size:uint32
//   root_proof_boc_size:flags.0?uint32
//   info_boc:(info_boc_size * [uint8])
//   root_proof_boc:flags.0?(root_proof_boc_size * [uint8])
//   header:flags.1?TorrentHeader = TorrentMeta;
//
struct TorrentMeta {
  TorrentMeta() = default;
  explicit TorrentMeta(TorrentInfo info, td::Ref<vm::Cell> root_proof = {}, td::optional<TorrentHeader> header = {})
      : info(std::move(info)), root_proof(std::move(root_proof)), header(std::move(header)) {
  }

  TorrentInfo info;
  td::Ref<vm::Cell> root_proof;
  td::optional<TorrentHeader> header;

  static td::Result<TorrentMeta> deserialize(td::Slice data);

  std::string serialize() const;

  static constexpr td::uint64 type = 0x6a7181e0;
  template <class StorerT>
  void store(StorerT &storer) const;

  template <class ParserT>
  void parse(ParserT &parser);
};
}  // namespace ton
