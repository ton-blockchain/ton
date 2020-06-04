#pragma once

#include "td/utils/StringBuilder.h"
#include "td/utils/Time.h"
#include "td/utils/VectorQueue.h"

namespace ton {
class LoadSpeed {
 public:
  void add(td::size_t size, td::Timestamp now);
  double speed(td::Timestamp now = td::Timestamp::now()) const;
  friend td::StringBuilder &operator<<(td::StringBuilder &sb, const LoadSpeed &speed);

 private:
  struct Event {
    td::size_t size;
    td::Timestamp at;
  };
  mutable td::VectorQueue<Event> events_;
  mutable td::size_t total_size_{0};

  double duration() const;
  void update(td::Timestamp now) const;
};
}  // namespace ton
