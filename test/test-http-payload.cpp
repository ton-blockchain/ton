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
#include "http/http-client.h"
#include "http/http-connection.h"
#include "http/http.h"
#include "td/utils/tests.h"

TEST(HttpConnection, TunnelEofAfterBufferedData) {
  struct Socket {
    mutable td::PollableFdInfo info;
    size_t remaining = 257;
    bool repeat_eof;
    explicit Socket(bool repeat_eof) : repeat_eof(repeat_eof) {
      info.add_flags(td::PollFlags::Close());
    }
    td::PollableFdInfo &get_poll_info() const {
      return info;
    }
    td::Status flush_read(size_t limit) {
      while (td::can_read(*this) && limit) {
        if (!remaining) {
          info.clear_flags(td::PollFlags::Read());
          if (repeat_eof) {
            info.add_flags(td::PollFlags::Close());
          }
          break;
        }
        auto size = td::min(remaining, limit);
        remaining -= size;
        limit -= size;
      }
      return td::Status::OK();
    }
  };
  for (bool repeat_eof : {false, true}) {
    Socket socket(repeat_eof);
    auto read = [&] {
      return repeat_eof ? ton::http::read_tunnel<Socket, false>(socket, 16).move_as_ok()
                        : ton::http::read_tunnel<Socket, true>(socket, 16).move_as_ok();
    };
    for (int i = 0; i < 16; ++i) {
      ASSERT_TRUE(!read());
    }
    ASSERT_EQ(socket.remaining, 1u);
    ASSERT_TRUE(read());
    ASSERT_EQ(socket.remaining, 0u);
  }
  Socket would_block(false);
  would_block.remaining = 0;
  ASSERT_TRUE(!(ton::http::read_tunnel<Socket, false>(would_block, 16).move_as_ok()));
}

TEST(ProxyHttp, CompletedBodyTerminatesWithoutDataLoss) {
  std::string body(835643, 'x');
  td::ChainBufferWriter writer;
  writer.append(td::Slice(body));
  auto input = writer.extract_reader();
  ton::http::HttpPayload payload(ton::http::HttpPayload::PayloadType::pt_content_length,
                                 ton::http::HttpResponse::low_watermark(), 1 << 20, body.size());
  payload.parse(input).ensure();
  ASSERT_TRUE(payload.parse_completed());
  auto part = payload.store_tl(2 << 20);
  ASSERT_EQ(part->data_.as_slice(), td::Slice(body));
  if (!part->last_) {
    ASSERT_TRUE(!payload.written());
    part = payload.store_tl(2 << 20);
    ASSERT_TRUE(part->data_.empty());
  }
  ASSERT_TRUE(part->last_);
  ASSERT_TRUE(payload.written());
}

TEST(ProxyHttp, ContentLengthCompletionAboveHighWatermark) {
  const size_t high = 1 << 20;
  const std::string body(high + 14 * 1024, 'x');
  for (bool complete : {false, true}) {
    td::ChainBufferWriter writer;
    writer.append(td::Slice(body));
    auto input = writer.extract_reader();
    ton::http::HttpPayload payload(ton::http::HttpPayload::PayloadType::pt_content_length,
                                   ton::http::HttpResponse::low_watermark(), high,
                                   body.size() + (complete ? 0 : 16384));
    payload.parse(input).ensure();
    ASSERT_EQ(input.size(), 0u);
    ASSERT_TRUE(payload.high_watermark_reached());
    ASSERT_EQ(payload.parse_completed(), complete);
    auto part = payload.store_tl(2 << 20);
    ASSERT_EQ(part->data_.as_slice(), td::Slice(body));
    if (complete && !part->last_) {
      ASSERT_TRUE(!payload.written());
      part = payload.store_tl(2 << 20);
      ASSERT_TRUE(part->data_.empty());
    }
    ASSERT_EQ(part->last_, complete);
    ASSERT_EQ(payload.written(), complete);
  }
}

TEST(ProxyHttp, BodyFailureNotifiesCallbacks) {
  struct Callback : ton::http::HttpPayload::Callback {
    explicit Callback(int &calls) : calls(calls) {
    }
    void run(size_t) override {
    }
    void completed() override {
      calls++;
    }
    int &calls;
  };
  ton::http::HttpPayload payload(ton::http::HttpPayload::PayloadType::pt_eof, 16384, 1048576);
  payload.add_chunk(td::BufferSlice("buffered"));
  int producer = 0, consumer = 0, late = 0;
  payload.add_callback(std::make_unique<Callback>(producer));
  payload.add_callback(std::make_unique<Callback>(consumer));
  payload.set_error();
  payload.set_error();
  payload.add_callback(std::make_unique<Callback>(late));
  ASSERT_TRUE(payload.is_error());
  ASSERT_EQ(producer, 1);
  ASSERT_EQ(consumer, 1);
  ASSERT_EQ(late, 1);
  ASSERT_EQ(payload.get_slice(1024).as_slice(), td::Slice("buffered"));
  ASSERT_EQ(late, 1);
}

TEST(ProxyHttp, NoBodyResponseSurvivesTlSerialization) {
  for (auto code : {200u, 204u, 304u}) {
    auto response = ton::http::HttpResponse::create("HTTP/1.1", code, "Test", code == 200, true).move_as_ok();
    response->add_header({"Content-Length", "12345"}).ensure();
    response->complete_parse_header().ensure();
    auto wire = response->store_tl();
    ASSERT_TRUE(wire->no_payload_);
    auto decoded =
        ton::http::HttpResponse::create(wire->http_version_, wire->status_code_, wire->reason_, wire->no_payload_, true)
            .move_as_ok();
    for (auto &header : wire->headers_) {
      decoded->add_header({header->name_, header->value_}).ensure();
    }
    decoded->complete_parse_header().ensure();
    ASSERT_TRUE(decoded->create_empty_payload().move_as_ok()->parse_completed());
  }
  auto response = ton::http::HttpResponse::create("HTTP/1.1", 200, "OK", false, true).move_as_ok();
  ASSERT_TRUE(!response->store_tl()->no_payload_);
}

TEST(HttpPayload, ChunkedFinalPartWaitsForTrailers) {
  const std::string body(65549, 'x');
  for (bool delayed : {false, true}) {
    td::ChainBufferWriter writer;
    writer.append("1000d\r\n");
    writer.append(td::Slice(body));
    writer.append("\r\n0\r\n");
    if (!delayed) {
      writer.append("X-Test: value\r\n\r\n");
    }
    auto input = writer.extract_reader();
    ton::http::HttpPayload payload(ton::http::HttpPayload::PayloadType::pt_chunked,
                                   ton::http::HttpResponse::low_watermark(), ton::http::HttpResponse::high_watermark());
    payload.parse(input).ensure();
    ASSERT_EQ(payload.parse_completed(), !delayed);
    auto first = payload.store_tl(body.size());
    ASSERT_EQ(first->data_.as_slice(), td::Slice(body));
    ASSERT_TRUE(first->trailer_.empty());
    ASSERT_TRUE(!first->last_);
    ASSERT_TRUE(!payload.written());
    if (delayed) {
      ASSERT_TRUE(!payload.store_tl(0)->last_);
      td::ChainBufferWriter remaining;
      remaining.append("X-Test: value\r\n\r\n");
      auto tail = remaining.extract_reader();
      payload.parse(tail).ensure();
      ASSERT_TRUE(payload.parse_completed());
    }
    size_t trailers = 0;
    bool finished = false;
    for (int i = 0; i < 3 && !finished; ++i) {
      auto part = payload.store_tl(4096);
      trailers += part->trailer_.size();
      if (!part->trailer_.empty()) {
        ASSERT_EQ(part->trailer_[0]->name_, "X-Test");
        ASSERT_EQ(part->trailer_[0]->value_, "value");
      }
      finished = part->last_;
    }
    ASSERT_TRUE(finished);
    ASSERT_EQ(trailers, 1u);
    ASSERT_TRUE(payload.written());
  }
}

TEST(ProxyHttp, ExplicitCloseTokens) {
  for (auto name : {"Connection", "Proxy-Connection"}) {
    auto response = ton::http::HttpResponse::create("HTTP/1.1", 200, "OK", false, true).move_as_ok();
    ASSERT_TRUE(!response->closes_connection());
    response->add_header({name, " keep-alive, ClOsE "}).ensure();
    response->add_header({name, "keep-alive"}).ensure();
    ASSERT_TRUE(response->closes_connection());
    ASSERT_TRUE(response->keep_alive());
  }
}

TEST(HttpConnection, EarlyUploadCancellationIsOptIn) {
  struct Callback : ton::http::HttpClient::Callback {
    void on_ready() override {
    }
    void on_stop_ready() override {
    }
  } callback;
  for (auto code : {200u, 413u}) {
    auto response = ton::http::HttpResponse::create("HTTP/1.1", code, "Test", false, true).move_as_ok();
    response->add_header({"Connection", "close"}).ensure();
    ASSERT_TRUE(!callback.should_cancel_upload(*response));
    ASSERT_TRUE(!response->cancel_request_payload());
    response->set_cancel_request_payload(true);
    ASSERT_TRUE(response->cancel_request_payload());
  }
}
