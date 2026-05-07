#include "app-traffic-metrics.h"

namespace ton::metrics {

AppTrafficMetrics::Ptr AppTrafficMetrics::make(std::string send_tl_base, std::string deliver_tl_base) {
  return std::shared_ptr<AppTrafficMetrics>(new AppTrafficMetrics(std::move(send_tl_base), std::move(deliver_tl_base)));
}

AppTrafficMetrics::AppTrafficMetrics(std::string send_tl_base, std::string deliver_tl_base)
    : send_bytes_(Labeled<std::string, AtomicCounter<td::uint64>>::make(
          "kind", "app_send_bytes_total", "Bytes the application asked to send (raw payload, by kind)."))
    , send_messages_(
          Labeled<std::string, AtomicCounter<td::uint64>>::make("kind", "app_send_messages_total",
                                                                "Messages the application asked to send (by kind)."))
    , deliver_bytes_(Labeled<std::string, AtomicCounter<td::uint64>>::make(
          "kind", "app_deliver_bytes_total", "Bytes delivered to the application (raw payload, by kind)."))
    , deliver_messages_(Labeled<std::string, AtomicCounter<td::uint64>>::make(
          "kind", "app_deliver_messages_total", "Messages delivered to the application (by kind)."))
    , send_by_tl_(Labeled<std::string, TlTrafficBucket>::make(
          "kind", std::move(send_tl_base), "Bytes sent by inner TL constructor.", "Messages sent by inner TL constructor."))
    , deliver_by_tl_(
          Labeled<std::string, TlTrafficBucket>::make("kind", std::move(deliver_tl_base),
                                                      "Bytes delivered by inner TL constructor.",
                                                      "Messages delivered by inner TL constructor.")) {
}

AppTrafficMetrics::CounterPtr AppTrafficMetrics::send_bytes(std::string kind) {
  return send_bytes_->label(std::move(kind));
}

AppTrafficMetrics::CounterPtr AppTrafficMetrics::send_messages(std::string kind) {
  return send_messages_->label(std::move(kind));
}

AppTrafficMetrics::CounterPtr AppTrafficMetrics::deliver_bytes(std::string kind) {
  return deliver_bytes_->label(std::move(kind));
}

AppTrafficMetrics::CounterPtr AppTrafficMetrics::deliver_messages(std::string kind) {
  return deliver_messages_->label(std::move(kind));
}

void AppTrafficMetrics::account_send_tl(std::string kind, td::Slice payload) {
  send_by_tl_->label(std::move(kind))->account(payload);
}

void AppTrafficMetrics::account_send_tl_with_magic(std::string kind, td::int32 magic, td::uint64 size) {
  send_by_tl_->label(std::move(kind))->account_with_magic(magic, size);
}

void AppTrafficMetrics::account_deliver_tl(std::string kind, td::Slice payload) {
  deliver_by_tl_->label(std::move(kind))->account(payload);
}

void AppTrafficMetrics::account_deliver_tl_with_magic(std::string kind, td::int32 magic, td::uint64 size) {
  deliver_by_tl_->label(std::move(kind))->account_with_magic(magic, size);
}

void AppTrafficMetrics::record_send(std::string kind, td::Slice payload) {
  send_bytes(kind)->add(payload.size());
  send_messages(kind)->add(1);
  account_send_tl(std::move(kind), payload);
}

void AppTrafficMetrics::record_send_with_magic(std::string kind, td::int32 magic, td::uint64 size) {
  send_bytes(kind)->add(size);
  send_messages(kind)->add(1);
  account_send_tl_with_magic(std::move(kind), magic, size);
}

void AppTrafficMetrics::record_deliver(std::string kind, td::Slice payload) {
  deliver_bytes(kind)->add(payload.size());
  deliver_messages(kind)->add(1);
  account_deliver_tl(std::move(kind), payload);
}

void AppTrafficMetrics::record_deliver_with_magic(std::string kind, td::int32 magic, td::uint64 size) {
  deliver_bytes(kind)->add(size);
  deliver_messages(kind)->add(1);
  account_deliver_tl_with_magic(std::move(kind), magic, size);
}

MetricSet AppTrafficMetrics::collect() {
  MetricSet set = {};
  set = std::move(set).join(send_bytes_->collect());
  set = std::move(set).join(send_messages_->collect());
  set = std::move(set).join(deliver_bytes_->collect());
  set = std::move(set).join(deliver_messages_->collect());
  set = std::move(set).join(send_by_tl_->collect());
  set = std::move(set).join(deliver_by_tl_->collect());
  return set;
}

}  // namespace ton::metrics
