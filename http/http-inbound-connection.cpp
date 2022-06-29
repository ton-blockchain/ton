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

    Copyright 2019-2020 Telegram Systems LLP
*/
#include "http-inbound-connection.h"
#include "td/utils/misc.h"

namespace ton {

namespace http {

void HttpInboundConnection::send_client_error() {
  static const auto s =
      "HTTP/1.0 400 Bad Request\r\n"
      "Connection: Close\r\n"
      "Content-length: 0\r\n"
      "\r\n";
  buffered_fd_.output_buffer().append(td::Slice(s, strlen(s)));
  close_after_write_ = true;
  loop();
}

void HttpInboundConnection::send_server_error() {
  static const auto s =
      "HTTP/1.1 502 Bad Gateway\r\n"
      "Connection: keep-alive\r\n"
      "Content-length: 0\r\n"
      "\r\n";
  buffered_fd_.output_buffer().append(td::Slice(s, strlen(s)));
  loop();
}

void HttpInboundConnection::send_proxy_error(td::Status error) {
  if (error.code() == ErrorCode::timeout) {
    static const auto s =
        "HTTP/1.1 504 Gateway Timeout\r\n"
        "Connection: keep-alive\r\n"
        "Content-length: 0\r\n"
        "\r\n";
    buffered_fd_.output_buffer().append(td::Slice(s, strlen(s)));
  } else {
    static const auto s =
        "HTTP/1.1 502 Bad Gateway\r\n"
        "Connection: keep-alive\r\n"
        "Content-length: 0\r\n"
        "\r\n";
    buffered_fd_.output_buffer().append(td::Slice(s, strlen(s)));
  }
  loop();
}

td::Status HttpInboundConnection::receive(td::ChainBufferReader &input) {
  if (reading_payload_) {
    return receive_payload(input);
  }

  if (!cur_request_ && !read_next_request_) {
    return td::Status::OK();
  }

  while (!cur_request_ || !cur_request_->check_parse_header_completed()) {
    bool exit_loop;
    auto R = HttpRequest::parse(std::move(cur_request_), cur_line_, exit_loop, input);
    if (R.is_error()) {
      send_client_error();
      return td::Status::OK();
    }
    if (exit_loop) {
      return td::Status::OK();
    }
    cur_request_ = R.move_as_ok();
  }

  auto payload = cur_request_->create_empty_payload().move_as_ok();
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this)](td::Result<std::pair<std::unique_ptr<HttpResponse>, std::shared_ptr<HttpPayload>>> R) {
        if (R.is_ok()) {
          auto a = R.move_as_ok();
          td::actor::send_closure(SelfId, &HttpInboundConnection::send_answer, std::move(a.first), std::move(a.second));
        } else {
          td::actor::send_closure(SelfId, &HttpInboundConnection::send_proxy_error, R.move_as_error());
        }
      });
  http_callback_->receive_request(std::move(cur_request_), payload, std::move(P));
  read_payload(std::move(payload));

  return td::Status::OK();
}

void HttpInboundConnection::send_answer(std::unique_ptr<HttpResponse> response, std::shared_ptr<HttpPayload> payload) {
  CHECK(payload);
  response->store_http(buffered_fd_.output_buffer());

  write_payload(std::move(payload));
  loop();
}

}  // namespace http

}  // namespace ton
