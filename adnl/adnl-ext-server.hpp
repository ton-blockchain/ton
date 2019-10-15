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

    Copyright 2017-2019 Telegram Systems LLP
*/
#pragma once

#include "adnl-peer-table.h"
#include "td/net/TcpListener.h"
#include "td/utils/crypto.h"
#include "td/utils/BufferedFd.h"
#include "adnl-ext-connection.hpp"
#include "adnl-ext-server.h"

#include <map>
#include <set>

namespace ton {

namespace adnl {

class TcpInfiniteListener : public td::actor::Actor {
 public:
  TcpInfiniteListener(td::int32 port, std::unique_ptr<td::TcpListener::Callback> callback)
      : port_(port), callback_(std::move(callback)) {
  }

 private:
  td::int32 port_;
  std::unique_ptr<td::TcpListener::Callback> callback_;
  td::actor::ActorOwn<td::TcpListener> tcp_listener_;
  td::int32 refcnt_{0};
  bool close_flag_{false};

  void start_up() override {
    loop();
  }

  void hangup() override {
    close_flag_ = true;
    tcp_listener_.reset();
    if (refcnt_ == 0) {
      stop();
    }
  }

  void loop() override {
    if (!tcp_listener_.empty()) {
      return;
    }
    class Callback : public td::TcpListener::Callback {
     public:
      Callback(td::actor::ActorShared<TcpInfiniteListener> parent) : parent_(std::move(parent)) {
      }
      void accept(td::SocketFd fd) override {
        td::actor::send_closure(parent_, &TcpInfiniteListener::accept, std::move(fd));
      }

     private:
      td::actor::ActorShared<TcpInfiniteListener> parent_;
    };
    refcnt_++;
    tcp_listener_ = td::actor::create_actor<td::TcpListener>(
        td::actor::ActorOptions().with_name(PSLICE() << "TcpListener" << td::tag("port", port_)).with_poll(), port_,
        std::make_unique<Callback>(actor_shared(this)));
  }

  void accept(td::SocketFd fd) {
    callback_->accept(std::move(fd));
  }

  void hangup_shared() override {
    refcnt_--;
    tcp_listener_.reset();
    if (close_flag_) {
      if (refcnt_ == 0) {
        stop();
      }
    } else {
      alarm_timestamp() = td::Timestamp::in(5 /*5 seconds*/);
    }
  }
};

class AdnlExtServerImpl;

class AdnlInboundConnection : public AdnlExtConnection {
 public:
  AdnlInboundConnection(td::SocketFd fd, td::actor::ActorId<AdnlPeerTable> peer_table,
                        td::actor::ActorId<AdnlExtServerImpl> ext_server)
      : AdnlExtConnection(std::move(fd), nullptr, false), peer_table_(peer_table), ext_server_(ext_server) {
  }

  td::Status process_packet(td::BufferSlice data) override;
  td::Status process_init_packet(td::BufferSlice data) override;
  td::Status process_custom_packet(td::BufferSlice &data, bool &processed) override;
  void inited_crypto(td::Result<td::BufferSlice> R);

 private:
  td::actor::ActorId<AdnlPeerTable> peer_table_;
  td::actor::ActorId<AdnlExtServerImpl> ext_server_;
  AdnlNodeIdShort local_id_;

  td::SecureString nonce_;
  AdnlNodeIdShort remote_id_ = AdnlNodeIdShort::zero();
};

class AdnlExtServerImpl : public AdnlExtServer {
 public:
  void add_tcp_port(td::uint16 port) override;
  void add_local_id(AdnlNodeIdShort id) override;
  void accepted(td::SocketFd fd);
  void decrypt_init_packet(AdnlNodeIdShort dst, td::BufferSlice data, td::Promise<td::BufferSlice> promise);

  void start_up() override {
    for (auto &port : ports_) {
      add_tcp_port(port);
    }
    ports_.clear();
  }

  void reopen_port() {
  }

  AdnlExtServerImpl(td::actor::ActorId<AdnlPeerTable> adnl, std::vector<AdnlNodeIdShort> ids,
                    std::vector<td::uint16> ports)
      : peer_table_(adnl) {
    for (auto &id : ids) {
      add_local_id(id);
    }
    for (auto &port : ports) {
      ports_.insert(port);
    }
  }

 private:
  td::actor::ActorId<AdnlPeerTable> peer_table_;
  std::set<AdnlNodeIdShort> local_ids_;
  std::set<td::uint16> ports_;
  std::map<td::uint16, td::actor::ActorOwn<TcpInfiniteListener>> listeners_;
};

}  // namespace adnl

}  // namespace ton
