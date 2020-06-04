#include "Pacer.h"
namespace ton {
namespace rldp2 {
Pacer::Pacer(Options options)
    : speed_(options.initial_speed)
    , capacity_(options.initial_capacity)
    , max_capacity_(options.max_capacity)
    , time_granularity_(options.time_granularity) {
}

td::Timestamp Pacer::wakeup_at() const {
  return wakeup_at_;
}

void Pacer::set_speed(double speed) {
  if (speed < 1) {
    speed = 1;
  }
  speed_ = speed;
}

td::optional<td::Timestamp> Pacer::send(double size, td::Timestamp now) {
  update_capacity(now);

  if (size < capacity_) {
    capacity_ -= size;
    return {};
  }

  size -= capacity_;
  capacity_ = 0;
  wakeup_at_ = td::Timestamp::in(size / speed_, now);
  capacity_at_ = wakeup_at_;
  return wakeup_at_;
}

void Pacer::update_capacity(td::Timestamp now) {
  if (capacity_at_ && capacity_at_.is_in_past(now)) {
    capacity_ += (now.at() - capacity_at_.at()) * speed_;
    capacity_ = td::min(capacity_, td::max(max_capacity_, speed_ * time_granularity_));
  }
  capacity_at_ = now;
}
}  // namespace rldp2
}  // namespace ton
