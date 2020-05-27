#pragma once

#include "td/utils/optional.h"

#include "fec/fec.h"

#include "RldpReceiver.h"

#include <map>

namespace ton {
namespace rldp2 {
struct InboundTransfer {
  struct Part {
    std::unique_ptr<td::fec::Decoder> decoder;
    RldpReceiver receiver;
    size_t offset;
  };

  explicit InboundTransfer(size_t total_size) : data_(total_size) {
  }

  size_t total_size() const;
  std::map<td::uint32, Part> &parts();
  bool is_part_completed(td::uint32 part_i);
  td::Result<Part *> get_part(td::uint32 part_i, const ton::fec::FecType &fec_type);
  void finish_part(td::uint32 part_i, td::Slice data);
  td::optional<td::Result<td::BufferSlice>> try_finish();

 private:
  std::map<td::uint32, Part> parts_;
  td::uint32 next_part_{0};
  size_t offset_{0};
  td::BufferSlice data_;
};
}  // namespace rldp2
}  // namespace ton
