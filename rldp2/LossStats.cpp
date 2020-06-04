#include "LossStats.h"
#include "td/utils/misc.h"

#include <cmath>

namespace ton {
namespace rldp2 {
void LossStats::on_update(td::uint32 ack, td::uint32 lost) {
  ack_ += ack;
  lost_ += lost;

  if (ack_ + lost_ > 1000) {
    auto new_loss = td::clamp((double)lost_ / (ack_ + lost_), 0.001, 0.2);
    if (fabs(new_loss - loss) > 5e-3) {
      prob = LossSender(new_loss, 1e-9);
    }
    loss = new_loss;
    ack_ = 0;
    lost_ = 0;
  }
}
}  // namespace rldp2
}  // namespace ton
