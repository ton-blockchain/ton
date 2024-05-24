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
#include "http-client.hpp"

#include "td/utils/Random.h"

namespace ton {

namespace http {

void HttpClientImpl::create_connection() {
  alarm_timestamp().relax(td::Timestamp::in(td::Random::fast(10.0, 20.0)));

  if (domain_.size() > 0) {
    auto S = addr_.init_host_port(domain_);
    if (S.is_error()) {
      LOG(INFO) << "failed domain '" << domain_ << "': " << S;
      return;
    }
  }

  auto fd = td::SocketFd::open(addr_);
  if (fd.is_error()) {
    LOG(INFO) << "failed to connect to " << addr_ << ": " << fd.move_as_error();
    return;
  }

  class Cb : public HttpClient::Callback {
   public:
    Cb(td::actor::ActorId<HttpClientImpl> id) : id_(id) {
    }

    void on_ready() override {
      td::actor::send_closure(id_, &HttpClientImpl::client_ready, true);
    }

    void on_stop_ready() override {
      td::actor::send_closure(id_, &HttpClientImpl::client_ready, false);
    }

   private:
    td::actor::ActorId<HttpClientImpl> id_;
  };
  conn_ = td::actor::create_actor<HttpOutboundConnection>(td::actor::ActorOptions().with_name("outconn").with_poll(),
                                                          fd.move_as_ok(), std::make_shared<Cb>(actor_id(this)));
}

void HttpClientImpl::send_request(
    std::unique_ptr<HttpRequest> request, std::shared_ptr<HttpPayload> payload, td::Timestamp timeout,
    td::Promise<std::pair<std::unique_ptr<HttpResponse>, std::shared_ptr<HttpPayload>>> promise) {
  td::actor::send_closure(conn_, &HttpOutboundConnection::send_query, std::move(request), std::move(payload), timeout,
                          std::move(promise));
}

void HttpMultiClientImpl::send_request(
    std::unique_ptr<HttpRequest> request, std::shared_ptr<HttpPayload> payload, td::Timestamp timeout,
    td::Promise<std::pair<std::unique_ptr<HttpResponse>, std::shared_ptr<HttpPayload>>> promise) {
  if (domain_.size() > 0) {
    auto S = addr_.init_host_port(domain_);
    if (S.is_error()) {
      return answer_error(HttpStatusCode::status_bad_gateway, "", std::move(promise));
    }
  }

  auto fd = td::SocketFd::open(addr_);
  if (fd.is_error()) {
    return answer_error(HttpStatusCode::status_bad_gateway, "", std::move(promise));
  }

  class Cb : public HttpClient::Callback {
   public:
    Cb(td::actor::ActorId<HttpMultiClientImpl> id) : id_(id) {
    }

    void on_ready() override {
    }

    void on_stop_ready() override {
    }

   private:
    td::actor::ActorId<HttpMultiClientImpl> id_;
  };
  auto conn =
      td::actor::create_actor<HttpOutboundConnection>(td::actor::ActorOptions().with_name("outconn").with_poll(),
                                                      fd.move_as_ok(), std::make_shared<Cb>(actor_id(this)))
          .release();
  request->set_keep_alive(false);
  td::actor::send_closure(conn, &HttpOutboundConnection::send_query, std::move(request), std::move(payload), timeout,
                          std::move(promise));
}

td::actor::ActorOwn<HttpClient> HttpClient::create(std::string domain, td::IPAddress addr,
                                                   std::shared_ptr<HttpClient::Callback> callback) {
  return td::actor::create_actor<HttpClientImpl>("httpclient", std::move(domain), addr, std::move(callback));
}

td::actor::ActorOwn<HttpClient> HttpClient::create_multi(std::string domain, td::IPAddress addr,
                                                         td::uint32 max_connections,
                                                         td::uint32 max_requests_per_connect,
                                                         std::shared_ptr<Callback> callback) {
  return td::actor::create_actor<HttpMultiClientImpl>("httpmclient", std::move(domain), addr, max_connections,
                                                      max_requests_per_connect, std::move(callback));
}

}  // namespace http

}  // namespace ton
