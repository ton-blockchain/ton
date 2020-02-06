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

#include "td/utils/port/IPAddress.h"
#include "td/actor/actor.h"

namespace ton {

namespace http {

class HttpOutboundConnection;

class HttpClient : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual ~Callback() = default;
    virtual void on_ready() = 0;
    virtual void on_stop_ready() = 0;
  };

  virtual void check_ready(td::Promise<td::Unit> promise) = 0;

  virtual void send_request(
      std::unique_ptr<HttpRequest> request, std::shared_ptr<HttpPayload> payload, td::Timestamp timeout,
      td::Promise<std::pair<std::unique_ptr<HttpResponse>, std::shared_ptr<HttpPayload>>> promise) = 0;

  static td::actor::ActorOwn<HttpClient> create(std::string domain, td::IPAddress addr,
                                                std::shared_ptr<Callback> callback);
  static td::actor::ActorOwn<HttpClient> create_multi(std::string domain, td::IPAddress addr,
                                                      td::uint32 max_connections, td::uint32 max_requests_per_connect,
                                                      std::shared_ptr<Callback> callback);
};

}  // namespace http

}  // namespace ton
