#pragma once

#include "RldpSender.h"
#include "fec/fec.h"

#include <map>

namespace ton {
namespace rldp2 {
struct OutboundTransfer {
 public:
  struct Part {
    std::unique_ptr<td::fec::Encoder> encoder;
    RldpSender sender;
    ton::fec::FecType fec_type;
  };

  OutboundTransfer(td::BufferSlice data) : data_(std::move(data)) {
  }

  size_t total_size() const;
  std::map<td::uint32, Part> &parts(const RldpSender::Config &config);
  void drop_part(td::uint32 part_i);
  Part *get_part(td::uint32 part_i);
  bool is_done() const;

 private:
  td::BufferSlice data_;
  std::map<td::uint32, Part> parts_;
  td::uint32 next_part_{0};

  static size_t part_size() {
    return 2000000;
  }
  static size_t symbol_size() {
    return 768;
  }
};
}  // namespace rldp2
}  // namespace ton
