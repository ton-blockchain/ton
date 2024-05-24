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
#include "http-server.h"
#include "http-inbound-connection.h"

namespace ton {

namespace http {

void HttpServer::start_up() {
  class Callback : public td::TcpListener::Callback {
   private:
    td::actor::ActorId<HttpServer> id_;

   public:
    Callback(td::actor::ActorId<HttpServer> id) : id_(id) {
    }
    void accept(td::SocketFd fd) override {
      td::actor::send_closure(id_, &HttpServer::accepted, std::move(fd));
    }
  };

  listener_ = td::actor::create_actor<td::TcpInfiniteListener>(
      td::actor::ActorOptions().with_name("listener").with_poll(), port_, std::make_unique<Callback>(actor_id(this)));
}

void HttpServer::accepted(td::SocketFd fd) {
  td::actor::create_actor<HttpInboundConnection>(td::actor::ActorOptions().with_name("inhttpconn").with_poll(),
                                                 std::move(fd), callback_)
      .release();
}

}  // namespace http

}  // namespace ton
