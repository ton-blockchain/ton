#pragma once

#include <string>

#include "metrics-collectors.h"
#include "tl-traffic-bucket.h"

namespace ton::metrics {

class AppTrafficMetrics final : public Collector {
 public:
  using Ptr = std::shared_ptr<AppTrafficMetrics>;
  using CounterPtr = std::shared_ptr<AtomicCounter<td::uint64>>;

  static Ptr make(std::string send_tl_base = "app_send", std::string deliver_tl_base = "app_deliver");

  CounterPtr send_bytes(std::string kind);
  CounterPtr send_messages(std::string kind);
  CounterPtr deliver_bytes(std::string kind);
  CounterPtr deliver_messages(std::string kind);

  void account_send_tl(std::string kind, td::Slice payload);
  void account_send_tl_with_magic(std::string kind, td::int32 magic, td::uint64 size);
  void account_deliver_tl(std::string kind, td::Slice payload);
  void account_deliver_tl_with_magic(std::string kind, td::int32 magic, td::uint64 size);

  void record_send(std::string kind, td::Slice payload);
  void record_send_with_magic(std::string kind, td::int32 magic, td::uint64 size);
  void record_deliver(std::string kind, td::Slice payload);
  void record_deliver_with_magic(std::string kind, td::int32 magic, td::uint64 size);

  MetricSet collect() override;

 private:
  explicit AppTrafficMetrics(std::string send_tl_base, std::string deliver_tl_base);

  Labeled<std::string, AtomicCounter<td::uint64>>::Ptr send_bytes_;
  Labeled<std::string, AtomicCounter<td::uint64>>::Ptr send_messages_;
  Labeled<std::string, AtomicCounter<td::uint64>>::Ptr deliver_bytes_;
  Labeled<std::string, AtomicCounter<td::uint64>>::Ptr deliver_messages_;
  Labeled<std::string, TlTrafficBucket>::Ptr send_by_tl_;
  Labeled<std::string, TlTrafficBucket>::Ptr deliver_by_tl_;
};

}  // namespace ton::metrics
