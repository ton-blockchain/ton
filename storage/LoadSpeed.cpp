#include "LoadSpeed.h"

#include "td/utils/format.h"

namespace ton {
void LoadSpeed::add(td::size_t size, td::Timestamp now) {
  total_size_ += size;
  events_.push(Event{size, now});
  update(now);
}
double LoadSpeed::speed(td::Timestamp now) const {
  update(now);
  return total_size_ / duration();
}

td::StringBuilder &operator<<(td::StringBuilder &sb, const LoadSpeed &speed) {
  return sb << td::format::as_size(static_cast<td::uint64>(speed.speed())) << "/s";
}

void LoadSpeed::update(td::Timestamp now) const {
  while (duration() > 60) {
    total_size_ -= events_.front().size;
    events_.pop();
  }
}
double LoadSpeed::duration() const {
  double res = 5;
  if (events_.size() > 1) {
    res = std::max(res, events_.back().at.at() - events_.front().at.at());
  }
  return res;
}
}  // namespace ton
