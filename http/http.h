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

#include "td/utils/buffer.h"
#include "auto/tl/ton_api.h"
#include "td/actor/PromiseFuture.h"

#include <map>
#include <list>
#include <mutex>

namespace ton {

namespace http {

enum HttpStatusCode : td::uint32 {
  status_ok = 200,
  status_bad_request = 400,
  status_method_not_allowed = 405,
  status_internal_server_error = 500,
  status_bad_gateway = 502,
  status_gateway_timeout = 504
};

struct HttpHeader {
  std::string name;
  std::string value;
  void store_http(td::ChainBufferWriter &output);
  tl_object_ptr<ton_api::http_header> store_tl();

  size_t size() const {
    return 2 + name.size() + value.size();
  }
  bool empty() const {
    return name.size() == 0;
  }

  td::Status basic_check();
};

namespace util {

td::Result<std::string> get_line(td::ChainBufferReader &input, std::string &cur_line, bool &read, size_t max_line_size);
td::Result<HttpHeader> get_header(std::string line);

}  // namespace util

class HttpPayload {
 public:
  enum class PayloadType { pt_empty, pt_eof, pt_chunked, pt_content_length, pt_tunnel };
  HttpPayload(PayloadType t, size_t low_watermark, size_t high_watermark, td::uint64 size)
      : type_(t), low_watermark_(low_watermark), high_watermark_(high_watermark), cur_chunk_size_(size) {
    CHECK(t == PayloadType::pt_content_length);
    state_ = ParseState::reading_chunk_data;
  }
  HttpPayload(PayloadType t, size_t low_watermark, size_t high_watermark)
      : type_(t), low_watermark_(low_watermark), high_watermark_(high_watermark) {
    CHECK(t != PayloadType::pt_content_length);
    CHECK(t != PayloadType::pt_empty);
    switch (t) {
      case PayloadType::pt_eof:
      case PayloadType::pt_tunnel:
        state_ = ParseState::reading_chunk_data;
        break;
      case PayloadType::pt_chunked:
        state_ = ParseState::reading_chunk_header;
        break;
      default:
        UNREACHABLE();
    }
  }
  HttpPayload(PayloadType t) : type_(t) {
    CHECK(t == PayloadType::pt_empty);
    state_ = ParseState::completed;
    written_zero_chunk_ = true;
    written_trailer_ = true;
  }

  class Callback {
   public:
    virtual void run(size_t ready_bytes) = 0;
    virtual void completed() = 0;
    virtual ~Callback() = default;
  };
  void add_callback(std::unique_ptr<Callback> callback);
  void run_callbacks();

  td::Status parse(td::ChainBufferReader &input);
  bool parse_completed() const;
  void complete_parse() {
    state_ = ParseState::completed;
    run_callbacks();
  }
  size_t ready_bytes() const {
    return ready_bytes_;
  }
  bool low_watermark_reached() const {
    return ready_bytes_ <= low_watermark_;
  }
  bool high_watermark_reached() const {
    return ready_bytes_ > high_watermark_;
  }
  bool is_error() const {
    return error_;
  }
  void set_error() {
    error_ = true;
  }
  PayloadType payload_type() const {
    return type_;
  }
  td::MutableSlice get_read_slice();
  void confirm_read(size_t s);
  void add_trailer(HttpHeader header);
  void add_chunk(td::BufferSlice data);
  td::BufferSlice get_slice(size_t max_size);
  void slice_gc();
  HttpHeader get_header();

  bool store_http(td::ChainBufferWriter &output, size_t max_size, HttpPayload::PayloadType store_type);
  tl_object_ptr<ton_api::http_payloadPart> store_tl(size_t max_size);

  bool written() const {
    return ready_bytes_ == 0 && parse_completed() && written_zero_chunk_ && written_trailer_;
  }

 private:
  enum class ParseState { reading_chunk_header, reading_chunk_data, reading_trailer, reading_crlf, completed };
  PayloadType type_{PayloadType::pt_chunked};
  size_t low_watermark_;
  size_t high_watermark_;
  std::string tmp_;
  std::list<td::BufferSlice> chunks_;
  std::list<HttpHeader> trailer_;
  size_t trailer_size_ = 0;
  size_t ready_bytes_ = 0;
  td::uint64 cur_chunk_size_ = 0;
  size_t last_chunk_free_ = 0;
  size_t chunk_size_ = 1 << 14;
  bool written_zero_chunk_ = false;
  bool written_trailer_ = false;
  bool error_ = false;

  std::list<std::unique_ptr<Callback>> callbacks_;

  std::atomic<ParseState> state_{ParseState::reading_chunk_header};
  std::mutex mutex_;
};

class HttpRequest {
 public:
  static constexpr size_t max_header_size() {
    return 16 << 10;
  }

  static constexpr size_t max_one_header_size() {
    return 16 << 10;
  }

  static constexpr size_t max_payload_size() {
    return 1 << 20;
  }

  static constexpr size_t low_watermark() {
    return 1 << 14;
  }
  static constexpr size_t high_watermark() {
    return 1 << 17;
  }

  static td::Result<std::unique_ptr<HttpRequest>> create(std::string method, std::string url,
                                                         std::string proto_version);

  HttpRequest(std::string method, std::string url, std::string proto_version);

  bool check_parse_header_completed() const;
  bool keep_alive() const {
    return keep_alive_;
  }

  td::Status complete_parse_header();
  td::Status add_header(HttpHeader header);
  td::Result<std::shared_ptr<HttpPayload>> create_empty_payload();
  bool need_payload() const;

  const auto &method() const {
    return method_;
  }
  const auto &url() const {
    return url_;
  }
  const auto &proto_version() const {
    return proto_version_;
  }
  const auto &host() const {
    return host_;
  }

  bool no_payload_in_answer() const {
    return method_ == "HEAD";
  }

  void set_keep_alive(bool value) {
    keep_alive_ = value;
  }

  void store_http(td::ChainBufferWriter &output);
  tl_object_ptr<ton_api::http_request> store_tl(td::Bits256 req_id);

  static td::Result<std::unique_ptr<HttpRequest>> parse(std::unique_ptr<HttpRequest> request, std::string &cur_line,
                                                        bool &exit_loop, td::ChainBufferReader &input);

 private:
  std::string method_;
  std::string url_;
  std::string proto_version_;

  std::string host_;
  size_t content_length_ = 0;
  bool found_content_length_ = false;
  bool found_transfer_encoding_ = false;

  bool parse_header_completed_ = false;
  bool keep_alive_ = false;

  std::vector<HttpHeader> options_;
};

class HttpResponse {
 public:
  static constexpr size_t max_header_size() {
    return 16 << 10;
  }

  static constexpr size_t max_one_header_size() {
    return 16 << 10;
  }

  static constexpr size_t max_payload_size() {
    return 1 << 20;
  }

  static constexpr size_t low_watermark() {
    return 1 << 14;
  }
  static constexpr size_t high_watermark() {
    return 1 << 17;
  }

  static td::Result<std::unique_ptr<HttpResponse>> create(std::string proto_version, td::uint32 code,
                                                          std::string reason, bool force_no_payload, bool keep_alive,
                                                          bool is_tunnel = false);

  HttpResponse(std::string proto_version, td::uint32 code, std::string reason, bool force_no_payload, bool keep_alive,
               bool is_tunnel = false);

  bool check_parse_header_completed() const;
  bool keep_alive() const {
    return !force_no_payload_ && keep_alive_;
  }

  td::Status complete_parse_header();
  td::Status add_header(HttpHeader header);
  td::Result<std::shared_ptr<HttpPayload>> create_empty_payload();
  bool need_payload() const;

  auto code() const {
    return code_;
  }
  const auto &proto_version() const {
    return proto_version_;
  }
  void set_keep_alive(bool value) {
    keep_alive_ = value;
  }

  void store_http(td::ChainBufferWriter &output);
  tl_object_ptr<ton_api::http_response> store_tl();

  static td::Result<std::unique_ptr<HttpResponse>> parse(std::unique_ptr<HttpResponse> request, std::string &cur_line,
                                                         bool force_no_payload, bool keep_alive, bool &exit_loop,
                                                         td::ChainBufferReader &input);

  static std::unique_ptr<HttpResponse> create_error(HttpStatusCode code, std::string reason);

  bool found_transfer_encoding() const {
    return found_transfer_encoding_;
  }
  bool found_content_length() const {
    return found_content_length_;
  }

 private:
  std::string proto_version_;
  td::uint32 code_;
  std::string reason_;

  bool force_no_payload_ = false;
  bool force_no_keep_alive_ = false;

  size_t content_length_ = 0;
  bool found_content_length_ = false;
  bool found_transfer_encoding_ = false;

  bool parse_header_completed_ = false;
  bool keep_alive_ = false;

  std::vector<HttpHeader> options_;
  bool is_tunnel_ = false;
};

void answer_error(HttpStatusCode code, std::string reason,
                  td::Promise<std::pair<std::unique_ptr<HttpResponse>, std::shared_ptr<HttpPayload>>> promise);

}  // namespace http

}  // namespace ton
