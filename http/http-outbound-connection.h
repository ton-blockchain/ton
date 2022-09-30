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
#include "http-client.h"

#include <list>

namespace ton {

namespace http {

class HttpOutboundConnection : public HttpConnection {
 public:
  struct Query {
    std::unique_ptr<HttpRequest> request;
    std::shared_ptr<HttpPayload> payload;
    td::Timestamp timeout;
    td::Promise<std::pair<std::unique_ptr<HttpResponse>, std::shared_ptr<HttpPayload>>> promise;
  };

  HttpOutboundConnection(td::SocketFd fd, std::shared_ptr<HttpClient::Callback> http_callback)
      : HttpConnection(std::move(fd), nullptr, false), http_callback_(std::move(http_callback)) {
  }

  td::Status receive_eof() override {
    found_eof_ = true;
    if (reading_payload_) {
      if (reading_payload_->payload_type() != HttpPayload::PayloadType::pt_eof &&
          reading_payload_->payload_type() != HttpPayload::PayloadType::pt_tunnel) {
        return td::Status::Error("unexpected EOF");
      } else {
        LOG(INFO) << "stopping (EOF payload)";
        reading_payload_->complete_parse();
        stop();
        return td::Status::OK();
      }
    } else {
      LOG(INFO) << "stopping (no req)";
      stop();
      return td::Status::OK();
    }
  }

  void alarm() override {
    LOG(INFO) << "closing outbound HTTP connection because of request timeout";
    if (promise_) {
      answer_error(HttpStatusCode::status_gateway_timeout, "", std::move(promise_));
    }
    stop();
  }

  void start_up() override {
    class Cb : public HttpConnection::Callback {
     public:
      Cb(std::shared_ptr<HttpClient::Callback> callback) : callback_(std::move(callback)) {
      }
      void on_ready(td::actor::ActorId<HttpConnection> conn) {
        callback_->on_ready();
      }
      void on_close(td::actor::ActorId<HttpConnection> conn) {
        callback_->on_stop_ready();
      }

     private:
      std::shared_ptr<HttpClient::Callback> callback_;
    };

    callback_ = std::make_unique<Cb>(std::move(http_callback_));

    HttpConnection::start_up();
  }

  td::Status receive(td::ChainBufferReader &input) override;
  void send_query(std::unique_ptr<HttpRequest> request, std::shared_ptr<HttpPayload> payload, td::Timestamp timeout,
                  td::Promise<std::pair<std::unique_ptr<HttpResponse>, std::shared_ptr<HttpPayload>>> promise);

  void send_next_query();

  void payload_read() override {
    reading_payload_ = nullptr;

    if (!close_after_read_) {
      alarm_timestamp() = td::Timestamp::never();
      send_next_query();
    } else {
      stop();
    }
  }
  void payload_written() override {
    writing_payload_ = nullptr;
  }

 private:
  std::shared_ptr<HttpClient::Callback> http_callback_;

  td::Promise<std::pair<std::unique_ptr<HttpResponse>, std::shared_ptr<HttpPayload>>> promise_;
  bool force_no_payload_;
  bool keep_alive_;

  std::unique_ptr<HttpResponse> cur_response_;
  std::string cur_line_;

  std::list<Query> next_;
};

}  // namespace http

}  // namespace ton
