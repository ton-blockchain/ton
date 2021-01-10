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
#include "http-client.h"
#include "http-outbound-connection.h"

#include "td/utils/Random.h"

namespace ton {

namespace http {

class HttpClientImpl : public HttpClient {
 public:
  HttpClientImpl(std::string domain, td::IPAddress addr, std::shared_ptr<Callback> callback)
      : domain_(std::move(domain)), addr_(addr), callback_(std::move(callback)) {
  }

  void start_up() override {
    create_connection();
  }
  void check_ready(td::Promise<td::Unit> promise) override {
    if (ready_) {
      promise.set_value(td::Unit());
    } else {
      promise.set_error(td::Status::Error(ErrorCode::notready, "connection not ready"));
    }
  }

  void client_ready(bool value) {
    if (ready_ == value) {
      return;
    }
    ready_ = value;
    if (ready_) {
      callback_->on_ready();
    } else {
      callback_->on_stop_ready();
      conn_.reset();
      if (next_create_at_.is_in_past()) {
        create_connection();
      } else {
        alarm_timestamp().relax(next_create_at_);
      }
    }
  }

  void alarm() override {
    create_connection();
  }
  void send_request(
      std::unique_ptr<HttpRequest> request, std::shared_ptr<HttpPayload> payload, td::Timestamp timeout,
      td::Promise<std::pair<std::unique_ptr<HttpResponse>, std::shared_ptr<HttpPayload>>> promise) override;

  void create_connection();

 private:
  bool ready_ = false;
  std::string domain_;
  td::IPAddress addr_;
  td::Timestamp next_create_at_;

  std::shared_ptr<Callback> callback_;
  td::actor::ActorOwn<HttpOutboundConnection> conn_;
};

class HttpMultiClientImpl : public HttpClient {
 public:
  HttpMultiClientImpl(std::string domain, td::IPAddress addr, td::uint32 max_connections,
                      td::uint32 max_requests_per_connect, std::shared_ptr<Callback> callback)
      : domain_(std::move(domain))
      , addr_(addr)
      , max_connections_(max_connections)
      , max_requests_per_connect_(max_requests_per_connect)
      , callback_(std::move(callback)) {
  }

  void start_up() override {
    callback_->on_ready();
  }
  void check_ready(td::Promise<td::Unit> promise) override {
    promise.set_value(td::Unit());
  }

  void send_request(
      std::unique_ptr<HttpRequest> request, std::shared_ptr<HttpPayload> payload, td::Timestamp timeout,
      td::Promise<std::pair<std::unique_ptr<HttpResponse>, std::shared_ptr<HttpPayload>>> promise) override;

 private:
  std::string domain_;
  td::IPAddress addr_;

  size_t max_connections_;
  td::uint32 max_requests_per_connect_;

  td::Timestamp next_create_at_;

  std::shared_ptr<Callback> callback_;
};

}  // namespace http

}  // namespace ton
