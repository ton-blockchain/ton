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
#include <array>
#include <memory>

#include "http/http-outbound-connection.h"
#include "td/utils/tests.h"

namespace {

struct HttpQueueState {
  std::array<int, 3> answered{};
};

class QueueClientCallback final : public ton::http::HttpClient::Callback {
 public:
  void on_ready() override {
  }
  void on_stop_ready() override {
  }
};

class QueuedHttpConnection final : public ton::http::HttpOutboundConnection {
 public:
  explicit QueuedHttpConnection(std::shared_ptr<HttpQueueState> state)
      : HttpOutboundConnection(td::SocketFd(), std::make_shared<QueueClientCallback>()), state_(std::move(state)) {
  }

  void start_up() override {
    for (int id = 0; id < 3; ++id) {
      auto request = ton::http::HttpRequest::create("GET", "/", "HTTP/1.1").move_as_ok();
      request->add_header({"Host", "localhost"}).ensure();
      request->set_keep_alive(true);
      request->complete_parse_header().ensure();
      auto payload = request->create_empty_payload().move_as_ok();
      ton::http::ResponsePromise promise([state = state_, id](td::Result<ton::http::ResponsePair> result) {
        ASSERT_TRUE(result.is_ok());
        ASSERT_EQ(result.ok().first->code(), static_cast<td::uint32>(201 + id));
        ++state->answered[id];
      });
      send_query(std::move(request), std::move(payload), td::Timestamp::in(10), std::move(promise));
    }
    for (auto answered : state_->answered) {
      ASSERT_EQ(answered, 0);
    }
    for (int id = 0; id < 3; ++id) {
      td::ChainBufferWriter buffer;
      auto response =
          "HTTP/1.1 " + std::to_string(201 + id) + " OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
      buffer.append(td::Slice(response));
      auto input = buffer.extract_reader();
      receive(input).ensure();
      ASSERT_EQ(input.size(), 0u);
      for (int query = 0; query < 3; ++query) {
        ASSERT_EQ(state_->answered[query], query <= id ? 1 : 0);
      }
    }
    stop();
    td::actor::SchedulerContext::get().stop();
  }

  void loop() override {
  }
  void tear_down() override {
  }

 private:
  std::shared_ptr<HttpQueueState> state_;
};

}  // namespace

TEST(HttpConnection, QueuedEmptyRequestsPreservePromises) {
  auto state = std::make_shared<HttpQueueState>();
  td::actor::Scheduler scheduler({1});
  scheduler.run_in_context([&] { td::actor::create_actor<QueuedHttpConnection>("http-queue", state).release(); });
  while (scheduler.run(0.05)) {
  }
  for (auto answered : state->answered) {
    ASSERT_EQ(answered, 1);
  }
}
