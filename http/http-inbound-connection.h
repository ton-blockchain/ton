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
#pragma once

#include "http.h"
#include "http-connection.h"
#include "http-server.h"

namespace ton {

namespace http {

class HttpInboundConnection : public HttpConnection {
 public:
  HttpInboundConnection(td::SocketFd fd, std::shared_ptr<HttpServer::Callback> http_callback)
      : HttpConnection(std::move(fd), nullptr, false), http_callback_(std::move(http_callback)) {
  }

  td::Status receive_eof() override {
    found_eof_ = true;
    if (reading_payload_) {
      if (reading_payload_->payload_type() != HttpPayload::PayloadType::pt_eof &&
          reading_payload_->payload_type() != HttpPayload::PayloadType::pt_tunnel) {
        return td::Status::Error("unexpected EOF");
      } else {
        reading_payload_->complete_parse();
        payload_read();
        return td::Status::OK();
      }
    } else {
      if (read_next_request_) {
        stop();
        return td::Status::OK();
      }
      return td::Status::OK();
    }
  }

  void send_client_error();
  void send_server_error();
  void send_proxy_error(td::Status error);

  void payload_written() override {
    writing_payload_ = nullptr;
    if (!close_after_write_) {
      read_next_request_ = true;
      if (found_eof_) {
        stop();
        return;
      }
    }
  }
  void payload_read() override {
    reading_payload_ = nullptr;
    read_next_request_ = false;
  }

  td::Status receive(td::ChainBufferReader &input) override;
  void send_answer(std::unique_ptr<HttpResponse> response, std::shared_ptr<HttpPayload> payload);

 private:
  static constexpr size_t chunk_size() {
    return 1 << 14;
  }

  bool read_next_request_ = true;

  std::shared_ptr<HttpServer::Callback> http_callback_;
  std::unique_ptr<HttpRequest> cur_request_;
  std::string cur_line_;
};

}  // namespace http

}  // namespace ton
