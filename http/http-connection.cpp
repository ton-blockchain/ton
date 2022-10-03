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
#include "http-connection.h"

namespace ton {

namespace http {

void HttpConnection::loop() {
  if (in_loop_) {
    return;
  }
  in_loop_ = true;
  auto status = [&] {
    while (true) {
      LOG(DEBUG) << "loop(): in=" << buffered_fd_.left_unread() << " out=" << buffered_fd_.left_unwritten();
      bool is_eof = td::can_close(buffered_fd_);
      bool read_eof = false;

      bool written = false;
      bool read = false;
      if (is_eof || buffered_fd_.left_unread() <= fd_low_watermark()) {
        allow_read_ = true;
      }
      if (allow_read_ && buffered_fd_.left_unread() < fd_high_watermark()) {
        TRY_RESULT(r, buffered_fd_.flush_read(fd_high_watermark() - buffered_fd_.left_unread()));
        if (r == 0 && is_eof) {
          read_eof = true;
        }
      }
      if (buffered_fd_.left_unread() >= fd_high_watermark()) {
        allow_read_ = false;
      }
      {
        auto &input = buffered_fd_.input_buffer();
        auto s = input.size();
        TRY_STATUS(receive(input));
        read = input.size() < s;
      }
      if (buffered_fd_.left_unread() == 0 && read_eof) {
        TRY_STATUS(receive_eof());
      }
      TRY_STATUS(buffered_fd_.flush_write());
      if (writing_payload_ && buffered_fd_.left_unwritten() < fd_high_watermark()) {
        written = continue_payload_write();
      }
      if (close_after_write_ && !writing_payload_ && !buffered_fd_.left_unwritten()) {
        LOG(INFO) << "close after write";
        stop();
        break;
      }
      if (close_after_read_ && !reading_payload_ && !buffered_fd_.left_unread()) {
        LOG(INFO) << "close after read";
        stop();
        break;
      }
      if (!written && !read) {
        break;
      }
    }
    return td::Status::OK();
  }();
  in_loop_ = false;
  if (status.is_error()) {
    LOG(ERROR) << "loop() failed: " << status;
    stop();
  } else {
    send_ready();
  }
}

void HttpConnection::send_error(std::unique_ptr<HttpResponse> response) {
  CHECK(!writing_payload_);
  auto payload = response->create_empty_payload().move_as_ok();
  CHECK(payload->parse_completed());
  send_response(std::move(response), std::move(payload));
}

void HttpConnection::send_request(std::unique_ptr<HttpRequest> request, std::shared_ptr<HttpPayload> payload) {
  CHECK(!writing_payload_);
  request->store_http(buffered_fd_.output_buffer());

  write_payload(std::move(payload));
}

void HttpConnection::send_response(std::unique_ptr<HttpResponse> response, std::shared_ptr<HttpPayload> payload) {
  CHECK(!writing_payload_);
  response->store_http(buffered_fd_.output_buffer());

  write_payload(std::move(payload));
}

void HttpConnection::write_payload(std::shared_ptr<HttpPayload> payload) {
  CHECK(!writing_payload_);

  writing_payload_ = std::move(payload);

  if (writing_payload_->parse_completed()) {
    continue_payload_write();
    return;
  }

  class Cb : public HttpPayload::Callback {
   public:
    Cb(td::actor::ActorId<HttpConnection> conn, size_t watermark) : conn_(conn), watermark_(watermark) {
    }
    void run(size_t ready_bytes) override {
      if (!reached_ && ready_bytes >= watermark_) {
        td::actor::send_closure(conn_, &HttpConnection::loop);
        reached_ = true;
      } else if (reached_ && ready_bytes < watermark_) {
        reached_ = false;
      }
    }
    void completed() override {
      td::actor::send_closure(conn_, &HttpConnection::loop);
    }

   private:
    td::actor::ActorId<HttpConnection> conn_;
    size_t watermark_;
    bool reached_ = false;
  };

  writing_payload_->add_callback(std::make_unique<Cb>(
      actor_id(this), writing_payload_->payload_type() == HttpPayload::PayloadType::pt_tunnel ? 1 : chunk_size()));
  continue_payload_write();
}

bool HttpConnection::continue_payload_write() {
  if (!writing_payload_) {
    return false;
  }
  if (writing_payload_->is_error()) {
    stop();
    return false;
  }

  auto t = writing_payload_->payload_type();
  if (t == HttpPayload::PayloadType::pt_eof) {
    t = HttpPayload::PayloadType::pt_chunked;
  }

  bool wrote = false;
  while (!writing_payload_->written()) {
    if (buffered_fd_.left_unwritten() > fd_high_watermark()) {
      return wrote;
    }
    bool is_tunnel = writing_payload_->payload_type() == HttpPayload::PayloadType::pt_tunnel;
    if (!is_tunnel && !writing_payload_->parse_completed() && writing_payload_->ready_bytes() < chunk_size()) {
      return wrote;
    }
    if (is_tunnel && writing_payload_->ready_bytes() == 0) {
      return wrote;
    }
    wrote |= writing_payload_->store_http(buffered_fd_.output_buffer(), chunk_size(), t);
  }
  if (writing_payload_->parse_completed() && writing_payload_->written()) {
    payload_written();
  }
  return wrote;
}

td::Status HttpConnection::read_payload(HttpResponse *response) {
  CHECK(!reading_payload_);

  if (!response->keep_alive()) {
    close_after_read_ = true;
  }

  return read_payload(response->create_empty_payload().move_as_ok());
}

td::Status HttpConnection::read_payload(HttpRequest *request) {
  CHECK(!reading_payload_);

  return read_payload(request->create_empty_payload().move_as_ok());
}

td::Status HttpConnection::read_payload(std::shared_ptr<HttpPayload> payload) {
  CHECK(!reading_payload_);

  reading_payload_ = std::move(payload);

  if (reading_payload_->parse_completed()) {
    payload_read();
    return td::Status::OK();
  }

  class Cb : public HttpPayload::Callback {
   public:
    Cb(td::actor::ActorId<HttpConnection> conn) : conn_(conn) {
    }
    void run(size_t ready_bytes) override {
      if (!reached_ && ready_bytes < watermark_) {
        reached_ = true;
        td::actor::send_closure(conn_, &HttpConnection::loop);
      } else if (reached_ && ready_bytes >= watermark_) {
        reached_ = false;
      }
    }
    void completed() override {
      td::actor::send_closure(conn_, &HttpConnection::loop);
    }

   private:
    size_t watermark_ = HttpRequest::low_watermark();
    bool reached_ = false;

    td::actor::ActorId<HttpConnection> conn_;
  };

  reading_payload_->add_callback(std::make_unique<Cb>(actor_id(this)));
  auto &input = buffered_fd_.input_buffer();
  return continue_payload_read(input);
}

td::Status HttpConnection::continue_payload_read(td::ChainBufferReader &input) {
  if (!reading_payload_) {
    return td::Status::OK();
  }
  while (!reading_payload_->parse_completed()) {
    if (reading_payload_->ready_bytes() > fd_high_watermark()) {
      return td::Status::OK();
    }
    auto s = input.size();
    auto R = reading_payload_->parse(input);
    if (R.is_error()) {
      reading_payload_->set_error();
      return R.move_as_error();
    }
    if (input.size() == s) {
      return td::Status::OK();
    }
  }
  if (reading_payload_->parse_completed()) {
    payload_read();
    return td::Status::OK();
  }
  return td::Status::OK();
}

td::Status HttpConnection::receive_payload(td::ChainBufferReader &input) {
  CHECK(reading_payload_);
  continue_payload_read(input);
  return td::Status::OK();
}

}  // namespace http

}  // namespace ton
