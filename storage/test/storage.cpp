/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/

#include "td/utils/benchmark.h"
#include "td/utils/crypto.h"
#include "td/utils/Container.h"
#include "td/utils/misc.h"
#include "td/utils/optional.h"
#include "td/utils/overloaded.h"
#include "td/utils/Status.h"
#include "td/utils/Span.h"
#include "td/utils/tests.h"
#include "td/utils/Timer.h"
#include "td/utils/Time.h"
#include "td/utils/tl_helpers.h"
#include "td/utils/UInt.h"
#include "td/utils/VectorQueue.h"
#include "td/utils/ThreadSafeCounter.h"

#include "td/utils/filesystem.h"
#include "td/utils/port/path.h"

#include "tl-utils/tl-utils.hpp"

#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api.hpp"

#include "td/actor/actor.h"

#include "td/db/utils/CyclicBuffer.h"

#include "vm/boc.h"
#include "vm/cells.h"
#include "vm/cellslice.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cells/CellString.h"

#include "fec/fec.h"

#include "rldp2/RldpConnection.h"
#include "rldp2/LossSender.h"

#include "Bitset.h"
#include "PeerState.h"
#include "Torrent.h"
#include "TorrentCreator.h"

#include "NodeActor.h"
#include "PeerActor.h"

#include "MerkleTree.h"

constexpr td::uint64 Byte = 1;
constexpr td::uint64 KiloByte = (1 << 10) * Byte;
constexpr td::uint64 MegaByte = (1 << 10) * KiloByte;

using namespace ton::rldp2;

extern "C" {
double ndtri(double y0);
double ndtri(double y0);
double nbdtr(int k, int n, double p);
double bdtr(int k, int n, double p);
double pdtr(int k, double y);
double pdtri(int k, double y);
}

BENCH(Loss, "Loss") {
  LossSender sender(0.5, 1e-10);
  td::uint64 res = 0;
  for (int i = 0; i < n; i++) {
    res += sender.send_n(100000);
  }
  td::do_not_optimize_away(res);
}

TEST(Rldp, Loss) {
  td::bench(LossBench());
  ASSERT_EQ(96, LossSender(0.1, 1e-10).send_n_exact(64));
  ASSERT_EQ(86, LossSender(0.05, 1e-10).send_n_exact(64));
  ASSERT_EQ(75, LossSender(0.01, 1e-10).send_n_exact(64));
  ASSERT_EQ(70, LossSender(0.001, 1e-10).send_n_exact(64));

  for (auto p1 : {1e-1, 1e-2, 1e-3, 1e-4, 1e-5, 1e-6, 1e-7, 1e-8, 1e-9, 1e-10}) {
    //CHECK(fabs(ndtri_fast(p1) - ndtri(1 - p1)) < 1e-6);
    for (auto loss : {0.5, 0.1, 0.01, 0.001, 0.0001}) {
      LossSender sender(loss, p1);
      for (auto n : {1, 10, 20, 50, 100, 250, 500, 1000, 2000, 4000, 8000, 16000, 32000}) {
        auto exact_m = sender.send_n_exact(n);
        auto approx_m = sender.send_n_approx_nbd(n);
        CHECK(!sender.has_good_approx() || std::abs(exact_m - approx_m) <= 1);
        //std::cerr << "p=" << loss << "\tS1=" << p1 << "\tn=" << n << "\tdiff=" << exact_m - approx_m << "\t" << exact_m
        //<< " " << approx_m << std::endl;
      }
    }
  }
}

TEST(Rldp, sub_or_zero) {
  ASSERT_EQ(10u, sub_or_zero(20, 10));
  ASSERT_EQ(0u, sub_or_zero(10, 20));
}

TEST(Rldp, RttStats) {
  RttStats stats;
  ASSERT_TRUE(stats.smoothed_rtt < 0);

  td::Timestamp now;
  stats.on_rtt_sample(-1, 0, now);
  ASSERT_TRUE(stats.smoothed_rtt < 0);
  stats.on_rtt_sample(1, -1, now);
  ASSERT_TRUE(stats.smoothed_rtt < 0);

  stats.on_rtt_sample(1, 0, now);
  stats.on_rtt_sample(2, 0, now);
  stats.on_rtt_sample(1, 0, now);
  stats.on_rtt_sample(2, 0, now);
  stats.on_rtt_sample(1, 0, now);
  stats.on_rtt_sample(2, 0, now);
  ASSERT_TRUE(fabs(stats.last_rtt - 2) < 1e-9);
  ASSERT_TRUE(fabs(stats.min_rtt - 1) < 1e-9);
  ASSERT_TRUE(1 < stats.smoothed_rtt && stats.smoothed_rtt < 2);
  ASSERT_TRUE(0.1 < stats.rtt_var && stats.rtt_var < 0.9);
}

TEST(Rldp, Ack) {
  Ack ack;
  ASSERT_TRUE(ack.on_got_packet(5));
  ASSERT_TRUE(!ack.on_got_packet(5));
  ASSERT_EQ(5u, ack.max_seqno);
  ASSERT_EQ(1u, ack.received_count);
  ASSERT_EQ(1u, ack.received_mask);

  ASSERT_TRUE(ack.on_got_packet(3));
  ASSERT_TRUE(!ack.on_got_packet(3));
  ASSERT_EQ(5u, ack.max_seqno);
  ASSERT_EQ(2u, ack.received_count);
  ASSERT_EQ(5u, ack.received_mask);

  ASSERT_TRUE(ack.on_got_packet(7));
  ASSERT_TRUE(!ack.on_got_packet(7));
  ASSERT_EQ(7u, ack.max_seqno);
  ASSERT_EQ(3u, ack.received_count);
  ASSERT_EQ(21u, ack.received_mask);

  ASSERT_TRUE(ack.on_got_packet(100));
  ASSERT_TRUE(!ack.on_got_packet(100));
  ASSERT_TRUE(!ack.on_got_packet(8));
  ASSERT_TRUE(!ack.on_got_packet(7));
  ASSERT_EQ(4u, ack.received_count);
  ASSERT_EQ(1u, ack.received_mask);
}

TEST(Rldp, SenderPackets) {
  td::Random::Xorshift128plus rnd(123);

  for (int test_i = 0; test_i < 100; test_i++) {
    Ack ack;
    SenderPackets sender;
    std::vector<td::uint32> in_flight;
    std::set<td::uint32> in_flight_set;
    std::set<td::uint32> received;
    std::set<td::uint32> dropped;
    std::set<td::uint32> no_ack;

    td::int32 now = 0;
    td::uint32 last_seqno = 0;

    td::uint32 window = rnd.fast(1, 100);

    auto send_query = [&]() {
      if (sender.in_flight_count() > window) {
        return;
      }
      last_seqno++;
      auto seqno = sender.next_seqno();
      CHECK(seqno == last_seqno);
      SenderPackets::Packet packet;
      packet.is_in_flight = true;
      packet.sent_at = td::Timestamp::at(now);
      packet.seqno = seqno;
      packet.size = 0;
      sender.send(packet);

      in_flight.push_back(seqno);
      in_flight_set.insert(seqno);
    };

    auto extract_in_flight_query = [&]() -> td::optional<td::uint32> {
      if (in_flight_set.empty()) {
        return {};
      }
      while (true) {
        auto position = rnd.fast(0, (int)in_flight.size() - 1);
        std::swap(in_flight[position], in_flight.back());
        auto seqno = in_flight.back();
        in_flight.pop_back();
        if (!in_flight_set.count(seqno)) {
          continue;
        }
        in_flight_set.erase(seqno);
        return seqno;
      }
    };

    auto receive_query = [&]() {
      auto o_seqno = extract_in_flight_query();
      if (!o_seqno) {
        return;
      }
      auto seqno = o_seqno.unwrap();
      if (ack.on_got_packet(seqno)) {
        received.insert(seqno);
      }
      no_ack.insert(seqno);
    };

    auto drop_query = [&]() {
      auto o_seqno = extract_in_flight_query();
      if (!o_seqno) {
        return;
      }
      auto seqno = o_seqno.unwrap();
      dropped.insert(seqno);
    };

    auto send_ack = [&]() {
      sender.on_ack(ack);
      no_ack.clear();
      ASSERT_EQ(received.size(), sender.received_count());
      //ASSERT_EQ(no_ack.size() + in_flight_set.size() + dropped.size(), sender.in_flight_count());
      if (!received.empty()) {
        ASSERT_EQ(*received.rbegin(), sender.max_packet().seqno);
      }
    };

    auto apply_limits = [&]() {
      auto till_seqno = sub_or_zero(sender.max_packet().seqno, rnd.fast(3, 31));
      SenderPackets::Limits limits;
      limits.sent_at = td::Timestamp::at(0);
      limits.seqno = till_seqno;
      //ASSERT_EQ(no_ack.size() + in_flight_set.size() + dropped.size(), sender.in_flight_count());

      in_flight_set.erase(in_flight_set.begin(), in_flight_set.lower_bound(till_seqno));
      dropped.erase(dropped.begin(), dropped.lower_bound(till_seqno));
      no_ack.erase(no_ack.begin(), no_ack.lower_bound(till_seqno));

      sender.drop_packets(limits);
      //LOG(ERROR) << td::tag("max_seqno", sender.max_packet().seqno);
      //LOG(ERROR) << td::tag("till_seqno", till_seqno);
      //LOG(ERROR) << td::tag("no_ack", no_ack);
      //LOG(ERROR) << td::tag("in_flight", in_flight);
      //LOG(ERROR) << td::tag("dropped", dropped);
      ASSERT_EQ(no_ack.size() + in_flight_set.size() + dropped.size(), sender.in_flight_count());
    };

    std::vector<td::RandomSteps::Step> steps_vec{
        {send_query, 0}, {receive_query, 0}, {drop_query, 0}, {send_ack, 0}, {apply_limits, 0}};
    for (auto &step : steps_vec) {
      step.weight = rnd.fast(1, 10);
    }
    td::RandomSteps steps{std::move(steps_vec)};
    for (int i = 0; i < 1000; i++) {
      steps.step(rnd);
    }
  }
}

TEST(Rldp, FecHelper) {
  FecHelper helper;
  td::uint32 x = 5;
  td::uint32 y = 5;
  td::uint32 n = 10;
  helper.symbols_count = n;
  ASSERT_EQ(n + x, helper.get_fec_symbols_count());
  ASSERT_EQ(n + x, helper.get_left_fec_symbols_count());
  helper.received_symbols_count = n + 1;
  ASSERT_EQ(n + x, helper.get_fec_symbols_count());
  ASSERT_EQ(x - 1, helper.get_left_fec_symbols_count());
  helper.received_symbols_count = n + x;
  ASSERT_EQ(n + x + y, helper.get_fec_symbols_count());
  ASSERT_EQ(y, helper.get_left_fec_symbols_count());
  helper.received_symbols_count = n + x + 1;
  ASSERT_EQ(n + x + y, helper.get_fec_symbols_count());
  ASSERT_EQ(y - 1, helper.get_left_fec_symbols_count());
  helper.received_symbols_count = n + x + y;
  ASSERT_EQ(n + x + 2 * y, helper.get_fec_symbols_count());
  ASSERT_EQ(y, helper.get_left_fec_symbols_count());
};

TEST(Rldp2, Pacer) {
  Pacer::Options options;
  options.initial_capacity = 0;
  options.initial_speed = 100;
  options.max_capacity = 1;
  options.time_granularity = 0.1;
  CHECK(options.initial_speed * options.time_granularity > options.max_capacity * 4);

  Pacer pacer(options);

  // send 1000 packets
  auto now = td::Timestamp::at(123);
  auto start = now;
  for (auto it = 0; it < 1000; it++) {
    CHECK(pacer.wakeup_at().is_in_past(now));
    auto o_wakeup_at = pacer.send(1, now);
    if (o_wakeup_at) {
      now = td::Timestamp::in(td::Random::fast(0.001, 0.1), o_wakeup_at.unwrap());
    }
  }
  double passed = now.at() - start.at();
  LOG_CHECK(passed > 9.9 && passed < 10.1) << passed;
}

class Sleep : public td::actor::Actor {
 public:
  static void put_to_sleep(td::actor::ActorId<Sleep> sleep, td::Timestamp till, td::Promise<td::Unit> promise) {
    send_closure(sleep, &Sleep::do_put_to_sleep, till, std::move(promise));
  }

  static TD_WARN_UNUSED_RESULT auto create() {
    return td::actor::create_actor<Sleep>("Sleep");
  }

 private:
  std::multimap<td::Timestamp, td::Promise<td::Unit>> pending_;

  void do_put_to_sleep(td::Timestamp till, td::Promise<td::Unit> promise) {
    pending_.emplace(till, std::move(promise));
    alarm_timestamp() = pending_.begin()->first;
  }

  void loop() override {
    while (!pending_.empty() && pending_.begin()->first.is_in_past()) {
      pending_.begin()->second.set_value(td::Unit());
      pending_.erase(pending_.begin());
    }

    if (!pending_.empty()) {
      alarm_timestamp() = pending_.begin()->first;
    }
  }
};

class NetChannel : public td::actor::Actor {
 public:
  struct Options {
    double loss{0};
    double rtt{0.1};

    double buffer{128 * KiloByte};
    double speed{1 * MegaByte};

    double alive_begin = -1;
    double sleep_step = 0;
    double alive_step = 1;

    static constexpr double eps = 1e-9;

    bool is_sleeping(double now) {
      if (sleep_step < eps) {
        return false;
      }
      return alive_begin > now + eps;
    }

    double calc_data(double l, double r) {
      if (sleep_step < eps) {
        return (r - l) * speed;
      }

      if (alive_begin < 0) {
        alive_begin = l;
      }
      double res = 0;
      while (true) {
        double alive_end = alive_begin + alive_step;
        if (l < alive_begin) {
          l = alive_begin;
        }
        if (l + eps > r) {
          break;
        } else if (r < alive_begin + eps) {
          break;
        } else if (l > alive_end - eps) {
          alive_begin += alive_step + sleep_step;
          alive_end = alive_begin + alive_step;
        } else {
          double new_l = td::min(alive_end, r);
          res += (new_l - l) * speed;
          l = new_l;
        }
      }
      return res;
    }

    double calc_wait(double need, double now) {
      constexpr double eps = 1e-9;
      if (sleep_step < eps) {
        return need / speed;
      }
      if (now < alive_begin) {
        return alive_begin - now;
      }
      return need / speed;
    }

    Options with_loss(double loss) {
      this->loss = loss;
      return *this;
    }
    Options with_rtt(double rtt) {
      this->rtt = rtt;
      return *this;
    }
    Options with_speed(double speed) {
      this->speed = speed;
      return *this;
    }
    Options with_buffer(double buffer) {
      this->buffer = buffer;
      return *this;
    }
    Options with_sleep_alive(double sleep, double alive) {
      this->sleep_step = sleep;
      this->alive_step = alive;
      return *this;
    }

    static Options perfect_net() {
      return NetChannel::Options().with_buffer(300 * MegaByte).with_loss(0).with_rtt(0.01).with_speed(100 * MegaByte);
    }
    static Options lossy_perfect_net() {
      return perfect_net().with_loss(0.1);
    }
    static Options bad_net() {
      return NetChannel::Options().with_buffer(128 * KiloByte).with_loss(0.1).with_rtt(0.2).with_speed(128 * KiloByte);
    }
  };

  static TD_WARN_UNUSED_RESULT td::actor::ActorOwn<NetChannel> create(Options options,
                                                                      td::actor::ActorId<Sleep> sleep) {
    return td::actor::create_actor<NetChannel>("NetChannel", options, std::move(sleep));
  }

  NetChannel(Options options, td::actor::ActorId<Sleep> sleep) : options_(options), sleep_(std::move(sleep)) {
  }

  td::uint64 total_sent() const {
    return total_sent_;
  }

  void send(size_t size, td::Promise<td::Unit> promise) {
    total_sent_ += size;
    if (total_size_ + (double)size > options_.buffer) {
      LOG(ERROR) << "OVERFLOW";
      promise.set_error(td::Status::Error("buffer overflow"));
      return;
    }
    if (td::Random::fast(0.0, 1.0) < options_.loss) {
      //LOG(ERROR) << "LOST";
      promise.set_error(td::Status::Error("lost"));
      return;
    }
    in_cnt_++;
    queue_.push(Query{size, std::move(promise)});
    total_size_ += (double)size;
    //auto span = queue_.as_mutable_span();
    //std::swap(span[td::Random::fast(0, (int)span.size() - 1)], span.back());
    yield();
  }

 private:
  struct Query {
    size_t size;
    td::Promise<td::Unit> promise;
  };
  Options options_;
  td::VectorQueue<Query> queue_;
  double total_size_{0};

  td::uint64 total_sent_{0};

  td::uint64 in_cnt_{0};
  td::uint64 out_cnt_{0};

  double got_{0};
  td::Timestamp got_at_{};

  td::actor::ActorId<Sleep> sleep_;

  void loop() override {
    auto now = td::Timestamp::now();
    if (got_at_) {
      got_ += options_.calc_data(got_at_.at(), now.at());
    }
    got_at_ = now;

    if (options_.is_sleeping(now.at())) {
      queue_ = {};
    }

    bool ok = false;
    while (!queue_.empty() && (double)queue_.front().size < got_) {
      ok = true;
      auto query = queue_.pop();
      got_ -= (double)query.size;
      total_size_ -= (double)query.size;
      out_cnt_++;
      Sleep::put_to_sleep(sleep_, td::Timestamp::in(options_.rtt), std::move(query.promise));
    }

    if (queue_.empty()) {
      got_at_ = {};
      got_ = 0;
      return;
    }

    auto wait_bytes = ((double)queue_.front().size - got_);
    auto wait_duration = options_.calc_wait(wait_bytes, now.at());
    //LOG(ERROR) << "Wait " << td::format::as_size((td::size_t)wait_bytes) << " " << td::format::as_time(wait_duration)
    //<< " " << in_cnt_ << " " << out_cnt_ << " " << ok;
    alarm_timestamp() = td::Timestamp::in(wait_duration);
  }
};

class Rldp : public td::actor::Actor, public ConnectionCallback {
 public:
  struct Stats {
    td::uint64 received_bytes{0};
    td::uint64 sent_bytes{0};
    td::Timestamp last_received_packet_at{};
    td::Timestamp last_sent_packet_at{};
  };

  void receive_raw(td::BufferSlice raw) {
    stats_->received_bytes += raw.size();
    connection_.receive_raw(std::move(raw));
    yield();
  }

  void send(td::BufferSlice data, td::Promise<td::Unit> promise) {
    TransferId transfer_id;
    td::Random::secure_bytes(as_slice(transfer_id));
    connection_.send(transfer_id, std::move(data));
    queries_[transfer_id] = std::move(promise);
    yield();
  }

  void add_peer(td::actor::ActorId<Rldp> peer) {
    peer_ = peer;
    yield();
  }

  void send_raw(td::BufferSlice data) override {
    auto size = data.size();
    stats_->sent_bytes += size;
    send_closure(net_channel_, &NetChannel::send, size,
                 [data = std::move(data), peer = peer_](td::Result<td::Unit> res) mutable {
                   if (res.is_ok()) {
                     send_closure(peer, &Rldp::receive_raw, std::move(data));
                   }
                 });
  }
  void receive(TransferId, td::Result<td::BufferSlice> data) override {
    CHECK(data.is_ok());
    stats_->last_received_packet_at = td::Timestamp::now();
    //LOG(ERROR) << "GOT ";
  }

  void on_sent(TransferId query_id, td::Result<td::Unit> state) override {
    stats_->last_sent_packet_at = td::Timestamp::now();
    //LOG(ERROR) << "SENT " << query_id;
    auto it = queries_.find(query_id);
    CHECK(queries_.end() != it);
    it->second.set_result(std::move(state));
    queries_.erase(it);
  }

  explicit Rldp(td::actor::ActorOwn<NetChannel> net_channel, Stats *stats)
      : net_channel_(std::move(net_channel)), stats_(stats) {
    CHECK(stats_);
    connection_.set_default_mtu(1 << 31);
  }

 private:
  RldpConnection connection_;
  td::actor::ActorOwn<NetChannel> net_channel_;
  td::actor::ActorId<Rldp> peer_;
  std::map<TransferId, td::Promise<td::Unit>> queries_;
  Stats *stats_;

  void loop() override {
    alarm_timestamp() = connection_.run(*this);
  }
};

struct RldpBasicTest {
  struct Options {
    size_t count{10};
    size_t query_size{1000 * Byte};
    NetChannel::Options net_options;
    size_t concurrent_queries{1};

    Options with_concurrent_queries(size_t concurrent_queries) {
      this->concurrent_queries = concurrent_queries;
      return *this;
    }

    static Options create(size_t count, size_t query_size, NetChannel::Options net_options) {
      Options options;
      options.count = count;
      options.query_size = query_size;
      options.net_options = net_options;
      return options;
    }
  };

  class Test : public td::actor::Actor {
   public:
    Test(Options options, td::actor::ActorOwn<Rldp> alice, td::actor::ActorOwn<Rldp> bob,
         td::actor::ActorOwn<Sleep> sleep, Rldp::Stats *alice_stats, Rldp::Stats *bob_stats)
        : options_(options)
        , alice_(std::move(alice))
        , bob_(std::move(bob))
        , sleep_(std::move(sleep))
        , alice_stats_(alice_stats)
        , bob_stats_(bob_stats) {
    }

   private:
    Options options_;
    td::actor::ActorOwn<Rldp> alice_;
    td::actor::ActorOwn<Rldp> bob_;
    td::actor::ActorOwn<Sleep> sleep_;

    Rldp::Stats *alice_stats_;
    Rldp::Stats *bob_stats_;
    td::Timestamp start_at_;
    td::Timestamp last_query_at_;

    size_t query_id_{0};
    size_t got_query_id_{0};

    int cnt_{0};
    void close(td::actor::ActorOwn<td::actor::Actor> actor) {
      auto actor_copy = actor.get();
      actor.reset();
      send_lambda(actor_copy,
                  [x = td::create_destructor([self = actor_id(this)] { send_closure(self, &Test::on_closed); })]() {});
    }
    void on_closed() {
      cnt_--;
      if (cnt_ == 0) {
        td::actor::SchedulerContext::get()->stop();
        //LOG(ERROR) << "STOP";
        stop();
      }
    }

    void start_up() override {
      start_at_ = td::Timestamp::now();
      for (size_t i = 0; i < options_.concurrent_queries; i++) {
        try_send_query();
      }
    }

    void tear_down() override {
      td::StringBuilder sb;
      sb << "\n";
      sb << "Sent " << options_.count << " * " << td::format::as_size(options_.query_size) << " = "
         << td::format::as_size(options_.query_size * options_.count) << "\n";
      sb << "Time: " << td::format::as_time(alice_stats_->last_sent_packet_at.at() - start_at_.at()) << "\n";
      sb << "Extra time: "
         << td::format::as_time(alice_stats_->last_sent_packet_at.at() - bob_stats_->last_received_packet_at.at())
         << "\n";
      sb << "Data overhead: " << alice_stats_->sent_bytes - (options_.query_size * options_.count) << "\n";
      sb << "Data overhead: " << (double)alice_stats_->sent_bytes / (double)(options_.query_size * options_.count)
         << "\n";
      LOG(ERROR) << sb.as_cslice();
    }

    void try_send_query(td::Result<td::Unit> = {}) {
      if (query_id_ >= options_.count) {
        return;
      }
      query_id_++;
      //LOG(ERROR) << "Create " << query_id_;
      last_query_at_ = td::Timestamp::now();
      td::BufferSlice query(options_.query_size);
      query.as_slice().fill('A');
      //LOG(ERROR) << "SEND";
      send_closure(alice_, &Rldp::send, std::move(query),
                   [self = actor_id(this)](auto x) { send_closure(self, &Test::on_query_finished); });
    }
    void on_query_finished() {
      try_send_query();
      //Sleep::put_to_sleep(sleep_.get(), td::Timestamp::in(20),
      //td::promise_send_closure(actor_id(this), &Test::try_send_query));
      got_query_id_++;
      //LOG(ERROR) << "Finished " << got_query_id_;
      if (got_query_id_ < options_.count) {
        return;
      }
      if (cnt_ == 0) {
        cnt_ = 3;
        close(std::move(alice_));
        close(std::move(bob_));
        close(std::move(sleep_));
      }
      return;
    }
  };

  static void run(Options options) {
    td::actor::Scheduler scheduler({0}, true);
    auto alice_stats = std::make_unique<Rldp::Stats>();
    auto bob_stats = std::make_unique<Rldp::Stats>();

    scheduler.run_in_context([&] {
      auto sleep = Sleep::create();
      auto alice_to_bob = NetChannel::create(options.net_options, sleep.get());
      auto bob_to_alice = NetChannel::create(options.net_options, sleep.get());

      auto alice = td::actor::create_actor<Rldp>("Alice", std::move(alice_to_bob), alice_stats.get());
      auto bob = td::actor::create_actor<Rldp>("Bob", std::move(bob_to_alice), bob_stats.get());
      send_closure(alice, &Rldp::add_peer, bob.get());
      send_closure(bob, &Rldp::add_peer, alice.get());
      td::actor::create_actor<Test>("Test", options, std::move(alice), std::move(bob), std::move(sleep),
                                    alice_stats.get(), bob_stats.get())
          .release();
    });
    scheduler.run();
  }
};

TEST(Rldp, Main) {
  using Options = RldpBasicTest::Options;
  RldpBasicTest::run(Options::create(10, 10 * MegaByte, NetChannel::Options::perfect_net()));
  RldpBasicTest::run(Options::create(10 * 80, 10 * MegaByte / 80, NetChannel::Options::perfect_net()));
  RldpBasicTest::run(
      Options::create(10 * 80, 10 * MegaByte / 80, NetChannel::Options::perfect_net()).with_concurrent_queries(20));
  return;

  RldpBasicTest::run(
      Options::create(10, 10 * MegaByte, NetChannel::Options::perfect_net()).with_concurrent_queries(10));
  RldpBasicTest::run(Options::create(10, 10 * MegaByte, NetChannel::Options::perfect_net()));
  return;
  RldpBasicTest::run(Options::create(10, 10 * MegaByte, NetChannel::Options::bad_net()));
  RldpBasicTest::run(Options::create(10, 10 * MegaByte, NetChannel::Options::bad_net()).with_concurrent_queries(10));
  //RldpBasicTest::run(Options::create(10, 100 * MegaByte, NetChannel::Options::perfect_net().with_sleep_alive(10, 1)));
  return;

  RldpBasicTest::run(Options::create(1000, 1 * Byte, NetChannel::Options::lossy_perfect_net()));
  RldpBasicTest::run(Options::create(1, 100 * MegaByte, NetChannel::Options::lossy_perfect_net()));

  RldpBasicTest::run(Options::create(100, 1 * MegaByte, NetChannel::Options::bad_net()));

  RldpBasicTest::run(Options::create(1, 1 * Byte, NetChannel::Options::perfect_net()));
  RldpBasicTest::run(Options::create(1, 1 * MegaByte, NetChannel::Options::perfect_net()));

  RldpBasicTest::run(Options::create(1, 100 * MegaByte, NetChannel::Options::perfect_net()));
}
/*
TEST(MerkleTree, Manual) {
  td::Random::Xorshift128plus rnd(123);
  // create big random file
  size_t chunk_size = 768;
  // for simplicity numer of chunks in a file is a power of two
  size_t chunks_count = (1 << 16) + 1;
  size_t file_size = chunk_size * chunks_count;
  td::Timer timer;
  LOG(INFO) << "Generate random string";
  const auto file = td::rand_string('a', 'z', td::narrow_cast<int>(file_size));
  LOG(INFO) << timer;

  timer = {};
  LOG(INFO) << "Calculate all hashes";
  std::vector<td::Bits256> hashes(chunks_count);
  td::Bits256 bad_hash{};
  for (size_t i = 0; i < chunks_count; i++) {
    td::sha256(td::Slice(file).substr(i * chunk_size, chunk_size), hashes[i].as_slice());
  }
  LOG(INFO) << timer;

  timer = {};
  LOG(INFO) << "Init merkle tree";
  size_t i = 0;
  ton::MerkleTree tree(td::transform(hashes, [&i](auto &x) { return ton::MerkleTree::Piece{i++, x}; }));
  LOG(INFO) << timer;

  auto root_proof = tree.gen_proof(0, chunks_count - 1).move_as_ok();
  auto root_hash = tree.get_root_hash();

  // first download each chunk one by one

  for (size_t stride : {1 << 6, 1}) {
    timer = {};
    LOG(INFO) << "Gen all proofs, stride = " << stride;
    for (size_t i = 0; i < chunks_count; i += stride) {
      tree.gen_proof(i, i + stride - 1).move_as_ok();
    }
    LOG(INFO) << timer;
    timer = {};
    LOG(INFO) << "Proof size: " << vm::std_boc_serialize(tree.gen_proof(0, stride - 1).move_as_ok()).ok().size();
    LOG(INFO) << "Download file, stride = " << stride;
    {
      ton::MerkleTree new_tree(chunks_count, root_hash);
      ton::MerkleTree other_new_tree(chunks_count, root_hash);
      for (size_t i = 0; i < chunks_count; i += stride) {
        new_tree.gen_proof(i, i + stride - 1).ignore();
        new_tree.add_proof(tree.gen_proof(i, i + stride - 1).move_as_ok()).ensure();
        other_new_tree.add_proof(tree.gen_proof(i, i + stride - 1).move_as_ok()).ensure();
        other_new_tree.gen_proof(i, i + stride - 1).ensure();
        other_new_tree.get_root(2);
        std::vector<ton::MerkleTree::Piece> chunks;
        for (size_t j = 0; j < stride && i + j < chunks_count; j++) {
          chunks.push_back({i + j, hashes.at(i + j)});
        }
        new_tree.try_add_pieces(chunks).ensure();
      }

      if (stride == 1) {
        std::vector<ton::MerkleTree::Piece> chunks;

        for (size_t i = 0; i < chunks_count; i++) {
          if (rnd.fast(0, 1) == 1) {
            chunks.push_back({i, hashes[i]});
          } else {
            chunks.push_back({i, bad_hash});
          }
        }
        td::Bitset bitmask;
        other_new_tree.add_pieces(chunks, bitmask);
        for (size_t i = 0; i < chunks_count; i++) {
          auto expected = chunks[i].hash == hashes[i];
          auto got = bitmask.get(i);
          LOG_CHECK(expected == got) << expected << " " << got << " " << i;
        }
      }
    }
    LOG(INFO) << timer;
  }
}

TEST(MerkleTree, Stress) {
  td::Random::Xorshift128plus rnd(123);

  for (int t = 0; t < 100; t++) {
    td::Bits256 bad_hash{};
    size_t chunks_count = rnd.fast(5, 10);
    std::vector<td::Bits256> hashes(chunks_count);
    for (auto &hash : hashes) {
      char x = (char)rnd.fast(0, 255);
      for (auto &c : hash.as_slice()) {
        c = x;
      }
    }
    size_t i = 0;
    ton::MerkleTree tree(td::transform(hashes, [&i](auto &x) { return ton::MerkleTree::Piece{i++, x}; }));
    for (int t2 = 0; t2 < 1000; t2++) {
      std::vector<ton::MerkleTree::Piece> chunks;

      int mask = rnd.fast(0, (1 << chunks_count) - 1);
      for (size_t i = 0; i < chunks_count; i++) {
        if ((mask >> i) & 1) {
          chunks.push_back({i, hashes[i]});
        } else {
          chunks.push_back({i, bad_hash});
        }
      }
      td::Bitset bitmask_strict;
      td::Bitset bitmask;
      ton::MerkleTree new_tree(chunks_count, tree.get_root(rnd.fast(1, 5)));
      tree.add_pieces(chunks, bitmask_strict);
      new_tree.add_pieces(chunks, bitmask);
      for (size_t i = 0; i < chunks_count; i++) {
        auto expected = chunks[i].hash == hashes[i];
        auto strict_got = bitmask_strict.get(i);
        LOG_CHECK(strict_got == expected) << expected << " " << strict_got << " " << i;
        auto got = bitmask.get(i);
        // got => expected
        LOG_CHECK(!got || expected) << expected << " " << got << " " << i;
      }
    }
  }
};*/

struct TorrentMetas {
  td::optional<ton::Torrent> torrent;
  struct File {
    std::string name;
    td::BlobView buffer;
  };
  std::vector<File> files;
};

TorrentMetas create_random_torrent(td::Random::Xorshift128plus &rnd, td::int64 total_size = 0,
                                   td::int32 piece_size = 0) {
  ton::Torrent::Creator::Options options;
  if (piece_size == 0) {
    options.piece_size = rnd.fast(1, 1024);
  } else {
    options.piece_size = piece_size;
  }
  if (total_size == 0) {
    total_size = rnd.fast(100, 40000);
  }
  ton::Torrent::Creator creator{options};

  TorrentMetas res;
  auto files_n = rnd.fast(0, 40);
  for (int i = 0; i < files_n; i++) {
    auto name = PSTRING() << "#" << i << ".txt";
    td::int64 n = 0;
    auto left = files_n - i;
    if (left == 1) {
      n = total_size;
    } else {
      n = rnd.fast64(total_size / (left * 2), 2 * total_size / left);
    }
    total_size -= n;
    LOG(INFO) << i << "/" << files_n << " " << n;
    std::string data;
    size_t len = td::min(n, td::int64(1027));
    data.reserve(len);
    for (size_t i = 0; i < len; i++) {
      data += static_cast<char>(rnd.fast('a', 'z'));
    }
    res.files.emplace_back(TorrentMetas::File{name, td::CycicBlobView::create(td::BufferSlice(data), n).move_as_ok()});
    creator.add_blob(name, td::CycicBlobView::create(td::BufferSlice(data), n).move_as_ok()).ensure();
  }
  LOG(INFO) << "Finalize...";
  res.torrent = creator.finalize().move_as_ok();
  ton::Torrent::GetMetaOptions opt;
  LOG(INFO) << "Meta size (full): " << res.torrent.value().get_meta_str(ton::Torrent::GetMetaOptions()).size();
  LOG(INFO) << "Meta size (only proof): "
            << res.torrent.value().get_meta_str(ton::Torrent::GetMetaOptions().without_header()).size();
  LOG(INFO) << "Meta size (only small proof): "
            << res.torrent.value()
                   .get_meta_str(ton::Torrent::GetMetaOptions().without_header().with_proof_depth_limit(10))
                   .size();
  LOG(INFO) << "Meta size (only header): "
            << res.torrent.value().get_meta_str(ton::Torrent::GetMetaOptions().without_proof()).size();
  LOG(INFO) << "Meta size (min): "
            << res.torrent.value().get_meta_str(ton::Torrent::GetMetaOptions().without_proof().without_header()).size();
  return res;
}

TEST(Torrent, Meta) {
  td::Random::Xorshift128plus rnd(123);
  for (int test_i = 0; test_i < 100; test_i++) {
    auto torrent_files = create_random_torrent(rnd);
    auto torrent = torrent_files.torrent.unwrap();
    auto files = std::move(torrent_files.files);

    // test TorrentMeta
    auto torrent_str = torrent.get_meta_str();

    auto torrent_file = ton::TorrentMeta::deserialize(torrent_str).move_as_ok();
    CHECK(torrent_file.serialize() == torrent_str);
    torrent_str.back()++;
    ton::TorrentMeta::deserialize(torrent_str).ensure_error();
    CHECK(torrent.get_info().get_hash() == torrent_file.info.get_hash());

    ton::Torrent::Options options;
    options.in_memory = true;
    torrent_file.header = {};
    torrent_file.root_proof = {};
    auto new_torrent = ton::Torrent::open(options, torrent_file).move_as_ok();
    new_torrent.enable_write_to_files();

    std::vector<size_t> order;
    for (size_t i = 0; i < torrent.get_info().pieces_count(); i++) {
      order.push_back(i);
    }
    CHECK(!new_torrent.is_completed());
    auto header_parts =
        (torrent.get_info().header_size + torrent.get_info().piece_size - 1) / torrent.get_info().piece_size;
    random_shuffle(td::MutableSpan<size_t>(order).substr(header_parts), rnd);
    random_shuffle(td::MutableSpan<size_t>(order).truncate(header_parts + 10), rnd);
    for (auto piece_i : order) {
      auto piece_data = torrent.get_piece_data(piece_i).move_as_ok();
      auto piece_proof = torrent.get_piece_proof(piece_i).move_as_ok();
      new_torrent.add_piece(piece_i, std::move(piece_data), std::move(piece_proof)).ensure();
    }
    CHECK(new_torrent.is_completed());
    new_torrent.validate();
    CHECK(new_torrent.is_completed());
    for (auto &name_data : files) {
      ASSERT_EQ(name_data.buffer.to_buffer_slice().move_as_ok(),
                new_torrent.read_file(name_data.name).move_as_ok().as_slice());
    }
  }
};

TEST(Torrent, OneFile) {
  td::rmrf("first").ignore();
  td::rmrf("second").ignore();

  td::mkdir("first").ensure();
  td::mkdir("second").ensure();

  td::write_file("first/hello.txt", "Hello world!").ensure();
  ton::Torrent::Creator::Options options;
  //options.dir_name = "first/";
  options.piece_size = 1024;
  auto torrent = ton::Torrent::Creator::create_from_path(options, "first/hello.txt").move_as_ok();
  auto meta = ton::TorrentMeta::deserialize(torrent.get_meta().serialize()).move_as_ok();
  CHECK(torrent.is_completed());

  {
    ton::Torrent::Options options;
    options.root_dir = "first/";
    auto other_torrent = ton::Torrent::open(options, meta).move_as_ok();
    CHECK(!other_torrent.is_completed());
    other_torrent.validate();
    CHECK(other_torrent.is_completed());
    CHECK(td::read_file("first/hello.txt").move_as_ok() == "Hello world!");
  }

  {
    ton::Torrent::Options options;
    options.root_dir = "second/";
    auto other_torrent = ton::Torrent::open(options, meta).move_as_ok();
    other_torrent.enable_write_to_files();
    CHECK(!other_torrent.is_completed());
    other_torrent.add_piece(0, torrent.get_piece_data(0).move_as_ok(), torrent.get_piece_proof(0).move_as_ok())
        .ensure();
    CHECK(other_torrent.is_completed());
    CHECK(td::read_file("second/hello.txt").move_as_ok() == "Hello world!");
  }
};

TEST(Torrent, PartsHelper) {
  int parts_count = 100;
  ton::PartsHelper parts(parts_count);

  auto a_token = parts.register_peer(1);
  auto b_token = parts.register_peer(2);
  auto c_token = parts.register_peer(3);

  parts.on_peer_part_ready(a_token, 1);
  parts.on_peer_part_ready(a_token, 2);
  parts.on_peer_part_ready(a_token, 3);
  parts.on_peer_part_ready(b_token, 1);
  parts.on_peer_part_ready(b_token, 2);
  parts.on_peer_part_ready(c_token, 1);
  ASSERT_EQ(0u, parts.get_rarest_parts(10).size());

  parts.set_peer_limit(a_token, 1);
  ASSERT_EQ(1u, parts.get_rarest_parts(10).size());
  parts.set_peer_limit(a_token, 2);
  ASSERT_EQ(2u, parts.get_rarest_parts(10).size());
  parts.set_peer_limit(a_token, 3);
  ASSERT_EQ(3u, parts.get_rarest_parts(10).size());
}

void print_debug(ton::Torrent *torrent) {
  LOG(ERROR) << torrent->get_stats_str();
}

TEST(Torrent, Peer) {
  class PeerManager : public td::actor::Actor {
   public:
    void send_query(ton::PeerId src, ton::PeerId dst, td::BufferSlice query, td::Promise<td::BufferSlice> promise) {
      send_closure(get_outbound_channel(src), &NetChannel::send, query.size(),
                   promise.send_closure(actor_id(this), &PeerManager::do_send_query, src, dst, std::move(query)));
    }

    void do_send_query(ton::PeerId src, ton::PeerId dst, td::BufferSlice query, td::Result<td::Unit> res,
                       td::Promise<td::BufferSlice> promise) {
      TRY_RESULT_PROMISE(promise, x, std::move(res));
      (void)x;
      send_closure(get_inbound_channel(dst), &NetChannel::send, query.size(),
                   promise.send_closure(actor_id(this), &PeerManager::execute_query, src, dst, std::move(query)));
    }

    void execute_query(ton::PeerId src, ton::PeerId dst, td::BufferSlice query, td::Result<td::Unit> res,
                       td::Promise<td::BufferSlice> promise) {
      TRY_RESULT_PROMISE(promise, x, std::move(res));
      (void)x;
      promise = promise.send_closure(actor_id(this), &PeerManager::send_response, src, dst);
      auto it = peers_.find(std::make_pair(dst, src));
      if (it == peers_.end()) {
        LOG(ERROR) << "No such peer";
        auto node_it = nodes_.find(dst);
        if (node_it == nodes_.end()) {
          LOG(ERROR) << "Unknown query destination";
          promise.set_error(td::Status::Error("Unknown query destination"));
          return;
        }
        send_closure(node_it->second, &ton::NodeActor::start_peer, src,
                     [promise = std::move(promise),
                      query = std::move(query)](td::Result<td::actor::ActorId<ton::PeerActor>> r_peer) mutable {
                       TRY_RESULT_PROMISE(promise, peer, std::move(r_peer));
                       send_closure(peer, &ton::PeerActor::execute_query, std::move(query), std::move(promise));
                     });
        return;
      }
      send_closure(it->second, &ton::PeerActor::execute_query, std::move(query), std::move(promise));
    }

    void send_response(ton::PeerId src, ton::PeerId dst, td::Result<td::BufferSlice> r_response,
                       td::Promise<td::BufferSlice> promise) {
      TRY_RESULT_PROMISE(promise, response, std::move(r_response));
      send_closure(get_outbound_channel(dst), &NetChannel::send, response.size(),
                   promise.send_closure(actor_id(this), &PeerManager::do_send_response, src, dst, std::move(response)));
    }

    void do_send_response(ton::PeerId src, ton::PeerId dst, td::BufferSlice response, td::Result<td::Unit> res,
                          td::Promise<td::BufferSlice> promise) {
      TRY_RESULT_PROMISE(promise, x, std::move(res));
      (void)x;
      send_closure(
          get_inbound_channel(src), &NetChannel::send, response.size(),
          promise.send_closure(actor_id(this), &PeerManager::do_execute_response, src, dst, std::move(response)));
    }

    void do_execute_response(ton::PeerId src, ton::PeerId dst, td::BufferSlice response, td::Result<td::Unit> res,
                             td::Promise<td::BufferSlice> promise) {
      TRY_RESULT_PROMISE(promise, x, std::move(res));
      (void)x;
      promise.set_value(std::move(response));
    }

    void register_peer(ton::PeerId src, ton::PeerId dst, td::actor::ActorId<ton::PeerActor> peer) {
      peers_[std::make_pair(src, dst)] = std::move(peer);
    }

    void register_node(ton::PeerId src, td::actor::ActorId<ton::NodeActor> node) {
      nodes_[src] = std::move(node);
    }
    ~PeerManager() {
      for (auto &it : inbound_channel_) {
        LOG(ERROR) << it.first << " received " << td::format::as_size(it.second.get_actor_unsafe().total_sent());
      }
      for (auto &it : outbound_channel_) {
        LOG(ERROR) << it.first << " sent " << td::format::as_size(it.second.get_actor_unsafe().total_sent());
      }
    }

   private:
    std::map<std::pair<ton::PeerId, ton::PeerId>, td::actor::ActorId<ton::PeerActor>> peers_;
    std::map<ton::PeerId, td::actor::ActorId<ton::NodeActor>> nodes_;
    std::map<ton::PeerId, td::actor::ActorOwn<NetChannel>> inbound_channel_;
    std::map<ton::PeerId, td::actor::ActorOwn<NetChannel>> outbound_channel_;

    td::actor::ActorOwn<Sleep> sleep_;
    void start_up() override {
      sleep_ = Sleep::create();
    }

    td::actor::ActorId<NetChannel> get_outbound_channel(ton::PeerId peer_id) {
      auto &res = outbound_channel_[peer_id];
      if (res.empty()) {
        NetChannel::Options options;
        options.speed = 1000 * MegaByte;
        options.buffer = 1000 * MegaByte;
        options.rtt = 0;
        res = NetChannel::create(options, sleep_.get());
      }
      return res.get();
    }
    td::actor::ActorId<NetChannel> get_inbound_channel(ton::PeerId peer_id) {
      auto &res = inbound_channel_[peer_id];
      if (res.empty()) {
        NetChannel::Options options;
        options.speed = 1000 * MegaByte;
        options.buffer = 1000 * MegaByte;
        options.rtt = 0;
        res = NetChannel::create(options, sleep_.get());
      }
      return res.get();
    }
  };

  class PeerCreator : public ton::NodeActor::NodeCallback {
   public:
    PeerCreator(td::actor::ActorId<PeerManager> peer_manager, ton::PeerId self_id, std::vector<ton::PeerId> peers)
        : peer_manager_(std::move(peer_manager)), peers_(std::move(peers)), self_id_(self_id) {
    }
    void get_peers(ton::PeerId src, td::Promise<std::vector<ton::PeerId>> promise) override {
      auto peers = peers_;
      promise.set_value(std::move(peers));
    }
    void register_self(td::actor::ActorId<ton::NodeActor> self) override {
      self_ = self;
      send_closure(peer_manager_, &PeerManager::register_node, self_id_, self_);
    }
    td::actor::ActorOwn<ton::PeerActor> create_peer(ton::PeerId self_id, ton::PeerId peer_id,
                                                    std::shared_ptr<ton::PeerState> state) override {
      class PeerCallback : public ton::PeerActor::Callback {
       public:
        PeerCallback(ton::PeerId self_id, ton::PeerId peer_id, td::actor::ActorId<PeerManager> peer_manager)
            : self_id_{self_id}, peer_id_{peer_id}, peer_manager_(peer_manager) {
        }
        void register_self(td::actor::ActorId<ton::PeerActor> self) override {
          self_ = std::move(self);
          send_closure(peer_manager_, &PeerManager::register_peer, self_id_, peer_id_, self_);
        }
        void send_query(td::uint64 query_id, td::BufferSlice query) override {
          CHECK(!self_.empty());
          class X : public td::actor::Actor {
           public:
            void start_up() override {
              //LOG(ERROR) << "start";
              alarm_timestamp() = td::Timestamp::in(4);
            }
            void tear_down() override {
              //LOG(ERROR) << "finish";
            }
            void alarm() override {
              //LOG(FATAL) << "WTF?";
              alarm_timestamp() = td::Timestamp::in(4);
            }
          };
          send_closure(
              peer_manager_, &PeerManager::send_query, self_id_, peer_id_, std::move(query),
              [self = self_, query_id,
               tmp = td::actor::create_actor<X>(PSLICE() << self_id_ << "->" << peer_id_ << " : " << query_id)](
                  auto x) { promise_send_closure(self, &ton::PeerActor::on_query_result, query_id)(std::move(x)); });
        }

       private:
        ton::PeerId self_id_;
        ton::PeerId peer_id_;
        td::actor::ActorId<ton::PeerActor> self_;
        td::actor::ActorId<PeerManager> peer_manager_;
      };

      return td::actor::create_actor<ton::PeerActor>(PSLICE() << "ton::PeerActor " << self_id << "->" << peer_id,
                                                     td::make_unique<PeerCallback>(self_id, peer_id, peer_manager_),
                                                     std::move(state));
    }

   private:
    td::actor::ActorId<PeerManager> peer_manager_;
    std::vector<ton::PeerId> peers_;
    ton::PeerId self_id_;
    td::actor::ActorId<ton::NodeActor> self_;
  };

  class TorrentCallback : public ton::NodeActor::Callback {
   public:
    TorrentCallback(std::shared_ptr<td::Destructor> stop_watcher, std::shared_ptr<td::Destructor> complete_watcher)
        : stop_watcher_(stop_watcher), complete_watcher_(complete_watcher) {
    }

    void on_completed() override {
      complete_watcher_.reset();
    }

    void on_closed(ton::Torrent torrent) override {
      CHECK(torrent.is_completed());
      //TODO: validate torrent
      stop_watcher_.reset();
    }

   private:
    std::shared_ptr<td::Destructor> stop_watcher_;
    std::shared_ptr<td::Destructor> complete_watcher_;
  };

  size_t peers_n = 20;
  td::uint64 file_size = 200 * MegaByte;
  td::Random::Xorshift128plus rnd(123);
  LOG(INFO) << "Start create random_torrent of size " << file_size;
  auto torrent = create_random_torrent(rnd, file_size, 128 * KiloByte).torrent.unwrap();
  LOG(INFO) << "Random torrent is created";

  std::vector<ton::PeerId> peers;
  for (size_t i = 1; i <= peers_n; i++) {
    peers.push_back(i);
  }
  auto gen_peers = [&](size_t self_id, size_t n) {
    std::vector<ton::PeerId> peers;
    if (n > peers_n - 1) {
      n = peers_n - 1;
    }
    while (n != 0) {
      size_t id = rnd.fast(1, td::narrow_cast<int>(peers_n));
      if (id == self_id) {
        continue;
      }
      if (std::find(peers.begin(), peers.end(), id) != peers.end()) {
        continue;
      }
      n--;
      peers.push_back(id);
    }
    return peers;
  };

  struct StatsActor : public td::actor::Actor {
   public:
    StatsActor(td::actor::ActorId<ton::NodeActor> node_actor) : node_actor_(node_actor) {
    }

   private:
    td::actor::ActorId<ton::NodeActor> node_actor_;
    void start_up() override {
      alarm_timestamp() = td::Timestamp::in(1);
    }
    void alarm() override {
      send_closure(node_actor_, &ton::NodeActor::with_torrent, [](td::Result<ton::NodeActor::NodeState> r_state) {
        if (r_state.is_error()) {
          return;
        }
        print_debug(&r_state.ok().torrent);
      });
      alarm_timestamp() = td::Timestamp::in(4);
    }
  };

  auto info = torrent.get_info();

  auto stop_watcher = td::create_shared_destructor([] { td::actor::SchedulerContext::get()->stop(); });
  auto guard = std::make_shared<std::vector<td::actor::ActorOwn<>>>();
  auto complete_watcher = td::create_shared_destructor([guard] {});

  td::actor::Scheduler scheduler({0}, true);

  scheduler.run_in_context([&] {
    auto peer_manager = td::actor::create_actor<PeerManager>("PeerManager");
    guard->push_back(td::actor::create_actor<ton::NodeActor>(
        "Node#1", 1, std::move(torrent),
        td::make_unique<TorrentCallback>(stop_watcher, complete_watcher),
        td::make_unique<PeerCreator>(peer_manager.get(), 1, gen_peers(1, 2)), nullptr));
    for (size_t i = 2; i <= peers_n; i++) {
      ton::Torrent::Options options;
      options.in_memory = true;
      auto other_torrent = ton::Torrent::open(options, ton::TorrentMeta(info)).move_as_ok();
      auto node_actor = td::actor::create_actor<ton::NodeActor>(
          PSLICE() << "Node#" << i, i, std::move(other_torrent),
          td::make_unique<TorrentCallback>(stop_watcher, complete_watcher),
          td::make_unique<PeerCreator>(peer_manager.get(), i, gen_peers(i, 2)),
          nullptr);

      if (i == 3) {
        td::actor::create_actor<StatsActor>("StatsActor", node_actor.get()).release();
      }
      guard->push_back(std::move(node_actor));
    }
    guard->push_back(std::move(peer_manager));
  });
  stop_watcher.reset();
  guard.reset();
  complete_watcher.reset();
  scheduler.run();
}
