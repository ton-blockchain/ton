/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <atomic>
#include <cstring>
#include <functional>
#include <vector>

#include "rldp-http-proxy/ProxyTransportDispatcher.h"
#include "td/actor/coro_utils.h"
#include "td/utils/tests.h"

namespace {

struct Trace {
  std::atomic<td::uint32> rldp_queries{0};
  std::atomic<td::uint32> quic_queries{0};
  std::atomic<bool> fail_quic{false};
};

class TestSender final : public ton::adnl::AdnlSenderInterface {
 public:
  enum class Kind { Rldp, Quic, Discovery };

  TestSender(Kind kind, std::shared_ptr<Trace> trace) : kind_(kind), trace_(std::move(trace)) {
  }

  void send_message(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice) override {
    UNREACHABLE();
  }

  void send_query(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, std::string name,
                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) override {
    send_query_ex(src, dst, std::move(name), std::move(promise), timeout, std::move(data), 1024);
  }

  void send_query_ex(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, std::string,
                     td::Promise<td::BufferSlice> promise, td::Timestamp, td::BufferSlice data, td::uint64) override {
    if (kind_ == Kind::Discovery) {
      auto query = ton::fetch_tl_object<ton::ton_api::http_proxy_getCapabilities>(data, true).move_as_ok();
      ASSERT_EQ(query->capabilities_, static_cast<td::int64>(ProxyTransportDispatcher::CAPABILITIES));
      ++probe_count_;
      pending_.push_back(std::move(promise));
      return;
    }
    if (kind_ == Kind::Quic) {
      ++trace_->quic_queries;
      if (trace_->fail_quic) {
        promise.set_error(td::Status::Error("QUIC failed"));
        return;
      }
    } else {
      ++trace_->rldp_queries;
    }
    promise.set_result(td::BufferSlice("ok"));
  }

  void get_conn_ip_str(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort,
                       td::Promise<std::string> promise) override {
    promise.set_result("test");
  }

  td::uint32 probe_count() {
    return probe_count_;
  }

  void answer_probe(td::uint64 capabilities) {
    ASSERT_EQ(pending_.size(), 1u);
    auto promise = std::move(pending_.front());
    pending_.clear();
    promise.set_result(ton::create_serialize_tl_object<ton::ton_api::http_proxy_capabilities>(capabilities));
  }

 private:
  Kind kind_;
  std::shared_ptr<Trace> trace_;
  td::uint32 probe_count_ = 0;
  std::vector<td::Promise<td::BufferSlice>> pending_;
};

class TestRunner final : public td::actor::Actor {
 public:
  using Test = std::function<td::actor::Task<>(TestRunner &)>;
  explicit TestRunner(Test test) : test_(std::move(test)) {
  }

  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(10.0);
    rldp_ = td::actor::create_actor<TestSender>("rldp", TestSender::Kind::Rldp, trace);
    quic_ = td::actor::create_actor<TestSender>("quic", TestSender::Kind::Quic, trace);
    discovery = td::actor::create_actor<TestSender>("discovery", TestSender::Kind::Discovery, trace);
    dispatcher =
        td::actor::create_actor<ProxyTransportDispatcher>("dispatcher", rldp_.get(), quic_.get(), discovery.get());
    run().start_immediate().detach("dispatcher test");
  }

  void alarm() override {
    LOG(FATAL) << "Dispatcher test timed out";
  }

  td::actor::Task<td::BufferSlice> query(ton::adnl::AdnlNodeIdShort peer) {
    auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::send_closure(dispatcher, &ProxyTransportDispatcher::send_query_ex, id(0), peer, "payload part",
                            std::move(promise), td::Timestamp::in(5.0), td::BufferSlice("request"), 1024);
    co_return co_await std::move(future);
  }

  static ton::adnl::AdnlNodeIdShort id(int value) {
    auto bits = td::Bits256::zero();
    std::memcpy(bits.as_slice().data(), &value, sizeof(value));
    return ton::adnl::AdnlNodeIdShort{bits};
  }

  std::shared_ptr<Trace> trace = std::make_shared<Trace>();
  td::actor::ActorOwn<TestSender> discovery;
  td::actor::ActorOwn<ProxyTransportDispatcher> dispatcher;

 private:
  Test test_;
  td::actor::ActorOwn<TestSender> rldp_;
  td::actor::ActorOwn<TestSender> quic_;

  td::actor::Task<> run() {
    (co_await test_(*this).wrap()).ensure();
    td::actor::SchedulerContext::get().stop();
    co_return {};
  }
};

void run_test(TestRunner::Test test) {
  td::actor::Scheduler scheduler({1});
  scheduler.run_in_context([&] { td::actor::create_actor<TestRunner>("dispatcher-test", std::move(test)).release(); });
  scheduler.run();
}

}  // namespace

TEST(HttpProxyDispatcher, EvictedPeerIsDiscoveredAgain) {
  run_test([](TestRunner &test) -> td::actor::Task<> {
    auto local_id = TestRunner::id(0);
    for (int i = 1; i <= 10000; ++i) {
      td::actor::send_closure(test.dispatcher, &ProxyTransportDispatcher::update_peer_capabilities, local_id,
                              TestRunner::id(i), ProxyTransportDispatcher::CAPABILITIES);
    }
    auto answer = co_await test.query(TestRunner::id(1));
    ASSERT_EQ(answer.as_slice(), td::Slice("ok"));
    ASSERT_EQ(test.trace->quic_queries.load(), 1u);
    ASSERT_EQ(test.trace->rldp_queries.load(), 0u);

    co_await td::actor::ask(test.dispatcher, &ProxyTransportDispatcher::update_peer_capabilities, local_id,
                            TestRunner::id(10001), ProxyTransportDispatcher::CAPABILITIES);
    (void)co_await test.query(TestRunner::id(2));
    ASSERT_EQ(test.trace->rldp_queries.load(), 1u);
    ASSERT_EQ(co_await td::actor::ask(test.discovery, &TestSender::probe_count), 1u);
    (void)co_await test.query(TestRunner::id(2));
    ASSERT_EQ(co_await td::actor::ask(test.discovery, &TestSender::probe_count), 1u);

    co_await td::actor::ask(test.discovery, &TestSender::answer_probe, ProxyTransportDispatcher::CAPABILITIES);
    (void)co_await test.query(TestRunner::id(2));
    ASSERT_EQ(test.trace->quic_queries.load(), 2u);
    ASSERT_EQ(test.trace->rldp_queries.load(), 2u);

    co_await td::actor::ask(test.dispatcher, &ProxyTransportDispatcher::update_peer_capabilities, local_id,
                            TestRunner::id(2), ProxyTransportDispatcher::CAPABILITY_RLDP2);
    (void)co_await test.query(TestRunner::id(2));
    ASSERT_EQ(test.trace->rldp_queries.load(), 3u);
    ASSERT_EQ(co_await td::actor::ask(test.discovery, &TestSender::probe_count), 1u);
    co_return {};
  });
}

TEST(HttpProxyDispatcher, QuicFailureDoesNotRetryRldp2) {
  run_test([](TestRunner &test) -> td::actor::Task<> {
    test.trace->fail_quic = true;
    co_await td::actor::ask(test.dispatcher, &ProxyTransportDispatcher::update_peer_capabilities, TestRunner::id(0),
                            TestRunner::id(1), ProxyTransportDispatcher::CAPABILITIES);
    auto first = co_await test.query(TestRunner::id(1)).wrap();
    ASSERT_TRUE(first.is_error());
    ASSERT_EQ(first.error().message(), td::Slice("QUIC failed"));
    auto second = co_await test.query(TestRunner::id(1)).wrap();
    ASSERT_TRUE(second.is_error());
    ASSERT_EQ(test.trace->quic_queries.load(), 2u);
    ASSERT_EQ(test.trace->rldp_queries.load(), 0u);
    ASSERT_EQ(co_await td::actor::ask(test.discovery, &TestSender::probe_count), 0u);
    co_return {};
  });
}
