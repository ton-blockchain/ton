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

#include "td/actor/actor.h"
#include "td/utils/port/SocketFd.h"
#include "td/utils/buffer.h"
#include "td/utils/BufferedFd.h"
#include "common/errorcode.h"

#include "http.h"

namespace ton {

namespace http {

class HttpConnection : public td::actor::Actor, public td::ObserverBase {
 public:
  class Callback {
   public:
    virtual ~Callback() = default;
    virtual void on_close(td::actor::ActorId<HttpConnection> conn) = 0;
    virtual void on_ready(td::actor::ActorId<HttpConnection> conn) = 0;
  };

  HttpConnection(td::SocketFd fd, std::unique_ptr<Callback> callback, bool is_client)
      : buffered_fd_(std::move(fd)), callback_(std::move(callback)), is_client_(is_client) {
  }
  virtual td::Status receive(td::ChainBufferReader &input) = 0;
  virtual td::Status receive_eof() = 0;
  td::Status receive_payload(td::ChainBufferReader &input);
  bool check_ready() const {
    return !td::can_close(buffered_fd_);
  }
  void check_ready_async(td::Promise<td::Unit> promise) {
    if (check_ready()) {
      promise.set_value(td::Unit());
    } else {
      promise.set_error(td::Status::Error(ErrorCode::notready, "not ready"));
    }
  }
  void send_ready() {
    if (check_ready() && !sent_ready_ && callback_) {
      callback_->on_ready(actor_id(this));
      sent_ready_ = true;
    }
  }
  void send_error(std::unique_ptr<HttpResponse> response);
  void send_request(std::unique_ptr<HttpRequest> request, std::shared_ptr<HttpPayload> payload);
  void send_response(std::unique_ptr<HttpResponse> response, std::shared_ptr<HttpPayload> payload);
  void write_payload(std::shared_ptr<HttpPayload> payload);
  bool continue_payload_write();
  td::Status read_payload(HttpRequest *request);
  td::Status read_payload(HttpResponse *response);
  td::Status read_payload(std::shared_ptr<HttpPayload> payload);
  td::Status continue_payload_read(td::ChainBufferReader &input);

  virtual void payload_read() = 0;
  virtual void payload_written() = 0;

  virtual ~HttpConnection() = default;

 protected:
  td::BufferedFd<td::SocketFd> buffered_fd_;
  td::actor::ActorId<HttpConnection> self_;
  std::unique_ptr<Callback> callback_;
  bool sent_ready_ = false;

  bool is_client_;
  bool close_after_write_ = false;
  bool close_after_read_ = false;
  bool found_eof_ = false;
  bool in_loop_ = false;
  bool allow_read_ = true;

  std::shared_ptr<HttpPayload> reading_payload_;
  std::shared_ptr<HttpPayload> writing_payload_;

  void notify() override {
    // NB: Interface will be changed
    td::actor::send_closure_later(self_, &HttpConnection::on_net);
  }

  void start_up() override {
    self_ = actor_id(this);
    // Subscribe for socket updates
    // NB: Interface will be changed
    td::actor::SchedulerContext::get()->get_poll().subscribe(buffered_fd_.get_poll_info().extract_pollable_fd(this),
                                                             td::PollFlags::ReadWrite() | td::PollFlags::Close());
    notify();
  }

  void loop() override;

 private:
  static constexpr size_t fd_low_watermark() {
    return 1 << 14;
  }
  static constexpr size_t fd_high_watermark() {
    return 1 << 16;
  }
  static constexpr size_t chunk_size() {
    return 1 << 10;
  }

  void on_net() {
    loop();
  }

  void tear_down() override {
    if (callback_) {
      callback_->on_close(actor_id(this));
      callback_ = nullptr;
    }
    // unsubscribe from socket updates
    // nb: interface will be changed
    td::actor::SchedulerContext::get()->get_poll().unsubscribe(buffered_fd_.get_poll_info().get_pollable_fd_ref());
    buffered_fd_.close();
  }
};

}  // namespace http

}  // namespace ton
