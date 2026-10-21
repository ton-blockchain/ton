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
#include "http/http-connection.h"
#include "http/http.h"
#include "td/utils/tests.h"

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
    td::ChainBufferWriter output;
    while (payload.ready_bytes() != 0) {
      payload.store_http(output, 4096, ton::http::HttpPayload::PayloadType::pt_content_length);
    }
    auto reader = output.extract_reader();
    td::BufferSlice actual(reader.size());
    reader.advance(actual.size(), actual.as_slice());
    ASSERT_EQ(actual.as_slice(), td::Slice(body));
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
