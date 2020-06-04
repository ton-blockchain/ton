#pragma once

#include "LossSender.h"

#include "td/utils/int_types.h"

namespace ton {
namespace rldp2 {
struct LossStats {
  void on_update(td::uint32 ack, td::uint32 lost);
  double loss = 0.1;
  LossSender prob{0.1, 1e-9};

 private:
  td::uint32 ack_{0};
  td::uint32 lost_{0};
};
}  // namespace rldp2
}  // namespace ton
