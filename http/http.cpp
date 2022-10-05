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
#include "http.h"

#include "td/utils/misc.h"

#include <algorithm>

namespace ton {

namespace http {

namespace util {

td::Result<std::string> get_line(td::ChainBufferReader &input, std::string &cur_line, bool &read,
                                 size_t max_line_size) {
  while (true) {
    if (input.size() == 0) {
      read = false;
      return "";
    }
    auto S = input.prepare_read();
    auto f = S.find('\n');
    if (f == td::Slice::npos) {
      if (cur_line.size() + S.size() > max_line_size) {
        return td::Status::Error("too big http header");
      }
      cur_line += S.str();
      input.confirm_read(S.size());
      continue;
    }
    if (f > 0) {
      if (S[f - 1] == '\r') {
        cur_line += S.truncate(f - 1).str();
      } else {
        cur_line += S.truncate(f).str();
      }
    } else {
      if (cur_line.size() > 0 && cur_line[cur_line.size() - 1] == '\r') {
        cur_line = cur_line.substr(0, cur_line.size() - 1);
      }
    }
    input.confirm_read(f + 1);
    auto s = std::move(cur_line);
    cur_line = "";
    read = true;
    return s;
  }
}

td::Result<HttpHeader> get_header(std::string line) {
  auto p = line.find(':');
  if (p == std::string::npos) {
    return td::Status::Error("failed to parse header");
  }
  return HttpHeader{line.substr(0, p), td::trim(line.substr(p + 1))};
}

}  // namespace util

void HttpHeader::store_http(td::ChainBufferWriter &output) {
  output.append(name);
  output.append(": ");
  output.append(value);
  output.append("\r\n");
}

tl_object_ptr<ton_api::http_header> HttpHeader::store_tl() {
  return create_tl_object<ton_api::http_header>(name, value);
}

td::Result<std::unique_ptr<HttpRequest>> HttpRequest::parse(std::unique_ptr<HttpRequest> request, std::string &cur_line,
                                                            bool &exit_loop, td::ChainBufferReader &input) {
  exit_loop = false;
  CHECK(!request || !request->check_parse_header_completed());
  while (true) {
    bool read;
    TRY_RESULT(line, util::get_line(input, cur_line, read, HttpRequest::max_one_header_size()));
    if (!read) {
      exit_loop = true;
      break;
    }

    if (!request) {
      auto v = td::full_split(line);
      if (v.size() != 3) {
        return td::Status::Error("expected http header in form ");
      }
      TRY_RESULT_ASSIGN(request, HttpRequest::create(v[0], v[1], v[2]));
    } else {
      if (line.size() == 0) {
        TRY_STATUS(request->complete_parse_header());
        break;
      } else {
        TRY_RESULT(h, util::get_header(std::move(line)));
        TRY_STATUS(request->add_header(std::move(h)));
      }
    }
  }

  return std::move(request);
}

HttpRequest::HttpRequest(std::string method, std::string url, std::string proto_version)
    : method_(std::move(method)), url_(std::move(url)), proto_version_(std::move(proto_version)) {
  if (proto_version_ == "HTTP/1.1") {
    keep_alive_ = true;
  } else {
    keep_alive_ = false;
  }
}

td::Result<std::unique_ptr<HttpRequest>> HttpRequest::create(std::string method, std::string url,
                                                             std::string proto_version) {
  if (proto_version != "HTTP/1.0" && proto_version != "HTTP/1.1") {
    return td::Status::Error(PSTRING() << "unsupported http version '" << proto_version << "'");
  }

  static const std::vector<std::string> supported_methods{"GET",    "HEAD",    "POST",    "PUT",
                                                          "DELETE", "CONNECT", "OPTIONS", "TRACE"};
  bool found = false;
  for (const auto &e : supported_methods) {
    if (e == method) {
      found = true;
      break;
    }
  }

  if (!found) {
    return td::Status::Error(PSTRING() << "unsupported http method '" << method << "'");
  }

  return std::make_unique<HttpRequest>(std::move(method), std::move(url), std::move(proto_version));
}

bool HttpRequest::check_parse_header_completed() const {
  return parse_header_completed_;
}

td::Status HttpRequest::complete_parse_header() {
  CHECK(!parse_header_completed_);
  parse_header_completed_ = true;
  return td::Status::OK();
}

td::Result<std::shared_ptr<HttpPayload>> HttpRequest::create_empty_payload() {
  CHECK(check_parse_header_completed());

  if (!need_payload()) {
    return std::make_shared<HttpPayload>(HttpPayload::PayloadType::pt_empty);
  } else if (method_ == "CONNECT") {
    return std::make_shared<HttpPayload>(HttpPayload::PayloadType::pt_tunnel, low_watermark(), high_watermark());
  } else if (found_content_length_) {
    return std::make_shared<HttpPayload>(HttpPayload::PayloadType::pt_content_length, low_watermark(), high_watermark(),
                                         content_length_);
  } else if (found_transfer_encoding_) {
    return std::make_shared<HttpPayload>(HttpPayload::PayloadType::pt_chunked, low_watermark(), high_watermark());
  } else {
    return td::Status::Error("expected Content-Length/Transfer-Encoding header");
  }
}

bool HttpRequest::need_payload() const {
  return found_content_length_ || found_transfer_encoding_ || method_ == "CONNECT";
}

td::Status HttpRequest::add_header(HttpHeader header) {
  auto lc_name = header.name;
  auto lc_value = header.value;
  std::transform(lc_name.begin(), lc_name.end(), lc_name.begin(), [](unsigned char c) { return std::tolower(c); });
  std::transform(lc_value.begin(), lc_value.end(), lc_value.begin(), [](unsigned char c) { return std::tolower(c); });

  auto S = td::trim(td::Slice(lc_value));

  if (lc_name == "content-length") {
    TRY_RESULT(len, td::to_integer_safe<td::uint32>(S));
    if (found_transfer_encoding_ || found_content_length_) {
      return td::Status::Error("duplicate Content-Length/Transfer-Encoding");
    }
    content_length_ = len;
    found_content_length_ = true;
  } else if (lc_name == "transfer-encoding") {
    // expect chunked, don't event check
    if (found_transfer_encoding_ || found_content_length_) {
      return td::Status::Error("duplicate Content-Length/Transfer-Encoding");
    }
    found_transfer_encoding_ = true;
  } else if (lc_name == "host") {
    if (host_.size() > 0) {
      return td::Status::Error("duplicate Host");
    }
    host_ = S.str();
  } else if (lc_name == "connection" && S == "keep-alive") {
    keep_alive_ = true;
    return td::Status::OK();
  } else if (lc_name == "connection" && S == "close") {
    keep_alive_ = false;
    return td::Status::OK();
  } else if (lc_name == "proxy-connection" && S == "keep-alive") {
    keep_alive_ = true;
    return td::Status::OK();
  } else if (lc_name == "proxy-connection" && S == "close") {
    keep_alive_ = false;
    return td::Status::OK();
  }
  options_.emplace_back(std::move(header));
  return td::Status::OK();
}

void HttpRequest::store_http(td::ChainBufferWriter &output) {
  std::string line = method_ + " " + url_ + " " + proto_version_ + "\r\n";
  output.append(line);
  for (auto &x : options_) {
    x.store_http(output);
  }
  if (keep_alive_) {
    HttpHeader{"Connection", "Keep-Alive"}.store_http(output);
  } else {
    HttpHeader{"Connection", "Close"}.store_http(output);
  }
  output.append(td::Slice("\r\n", 2));
}

tl_object_ptr<ton_api::http_request> HttpRequest::store_tl(td::Bits256 req_id) {
  std::vector<tl_object_ptr<ton_api::http_header>> headers;
  headers.reserve(options_.size());
  for (auto &h : options_) {
    headers.push_back(h.store_tl());
  }
  if (keep_alive_) {
    headers.push_back(HttpHeader{"Connection", "Keep-Alive"}.store_tl());
  } else {
    headers.push_back(HttpHeader{"Connection", "Close"}.store_tl());
  }
  return create_tl_object<ton_api::http_request>(req_id, method_, url_, proto_version_, std::move(headers));
}

td::Status HttpPayload::parse(td::ChainBufferReader &input) {
  CHECK(!parse_completed());
  while (true) {
    if (high_watermark_reached()) {
      return td::Status::OK();
    }
    switch (state_) {
      case ParseState::reading_chunk_header: {
        bool read;
        TRY_RESULT(l, util::get_line(input, tmp_, read, HttpRequest::max_one_header_size()));
        if (!read) {
          return td::Status::OK();
        }
        if (l.size() == 0) {
          return td::Status::Error("expected chunk, found empty line");
        }
        auto v = td::split(l);

        TRY_RESULT(size, td::hex_to_integer_safe<size_t>(v.first));
        if (size == 0) {
          state_ = ParseState::reading_trailer;
          break;
        }
        cur_chunk_size_ = size;
        state_ = ParseState::reading_chunk_data;
      } break;
      case ParseState::reading_chunk_data: {
        if (cur_chunk_size_ == 0) {
          if (type_ == PayloadType::pt_eof || type_ == PayloadType::pt_tunnel) {
            cur_chunk_size_ = 1LL << 60;
          } else if (type_ == PayloadType::pt_chunked) {
            state_ = ParseState::reading_crlf;
            break;
          } else if (type_ == PayloadType::pt_content_length) {
            LOG(INFO) << "payload parse success";
            const std::lock_guard<std::mutex> lock{mutex_};
            state_ = ParseState::completed;
            run_callbacks();
            return td::Status::OK();
          } else {
            UNREACHABLE();
          }
        }
        if (input.size() == 0) {
          return td::Status::OK();
        }
        auto S = get_read_slice();
        auto s = input.size();
        if (S.size() > s) {
          S.truncate(s);
        }
        CHECK(input.advance(S.size(), S) == S.size());
        confirm_read(S.size());
      } break;
      case ParseState::reading_trailer: {
        bool read;
        TRY_RESULT(l, util::get_line(input, tmp_, read, HttpRequest::max_one_header_size()));
        if (!read) {
          return td::Status::OK();
        }
        if (!l.size()) {
          LOG(INFO) << "payload parse success";
          const std::lock_guard<std::mutex> lock{mutex_};
          state_ = ParseState::completed;
          run_callbacks();
          return td::Status::OK();
        }
        TRY_RESULT(h, util::get_header(std::move(l)));
        add_trailer(std::move(h));
        if (trailer_size_ > HttpRequest::max_header_size()) {
          return td::Status::Error("too big trailer part");
        }
      } break;
      case ParseState::reading_crlf: {
        if (input.size() < 2) {
          return td::Status::OK();
        }
        td::uint8 buf[2];
        CHECK(input.advance(2, td::MutableSlice(buf, 2)) == 2);
        if (buf[0] != '\r' || buf[1] != '\n') {
          return td::Status::Error(PSTRING()
                                   << "expected CRLF " << static_cast<int>(buf[0]) << " " << static_cast<int>(buf[1]));
        }
        state_ = ParseState::reading_chunk_header;
      } break;
      case ParseState::completed:
        return td::Status::OK();
    }
  }
}

bool HttpPayload::parse_completed() const {
  return state_.load(std::memory_order_consume) == ParseState::completed;
}

td::MutableSlice HttpPayload::get_read_slice() {
  const std::lock_guard<std::mutex> lock{mutex_};
  if (last_chunk_free_ == 0) {
    auto B = td::BufferSlice{chunk_size_};
    last_chunk_free_ = B.size();
    chunks_.push_back(std::move(B));
  }
  auto b = chunks_.back().as_slice();
  b.remove_prefix(b.size() - last_chunk_free_);
  if (b.size() > cur_chunk_size_) {
    b.truncate(cur_chunk_size_);
  }
  return b;
}

void HttpPayload::confirm_read(size_t s) {
  const std::lock_guard<std::mutex> lock{mutex_};
  last_chunk_free_ -= s;
  cur_chunk_size_ -= s;
  ready_bytes_ += s;
  run_callbacks();
}

void HttpPayload::add_trailer(HttpHeader header) {
  const std::lock_guard<std::mutex> lock{mutex_};
  ready_bytes_ += header.size();
  trailer_size_ += header.size();
  run_callbacks();
  trailer_.push_back(std::move(header));
}

void HttpPayload::add_chunk(td::BufferSlice data) {
  //LOG(INFO) << "payload: added " << data.size() << " bytes";
  while (data.size() > 0) {
    if (!cur_chunk_size_) {
      cur_chunk_size_ = data.size();
    }
    auto S = get_read_slice();
    CHECK(S.size() > 0);
    if (S.size() > data.size()) {
      S.truncate(data.size());
    }
    S.copy_from(data.as_slice().truncate(S.size()));
    data.confirm_read(S.size());
    confirm_read(S.size());
  }
}

void HttpPayload::slice_gc() {
  const std::lock_guard<std::mutex> lock{mutex_};
  while (chunks_.size() > 0) {
    auto &x = chunks_.front();
    if (state_ == ParseState::completed || state_ == ParseState::reading_trailer) {
      if (chunks_.size() == 1) {
        x.truncate(x.size() - last_chunk_free_);
        last_chunk_free_ = 0;
      }
    }
    if (x.size() == 0) {
      CHECK(chunks_.size() > 1 || !last_chunk_free_);
      chunks_.pop_front();
      continue;
    }
    break;
  }
}

td::BufferSlice HttpPayload::get_slice(size_t max_size) {
  const std::lock_guard<std::mutex> lock{mutex_};
  while (chunks_.size() > 0) {
    auto &x = chunks_.front();
    if (x.size() == 0) {
      CHECK(chunks_.size() > 1 || !last_chunk_free_);
      chunks_.pop_front();
      continue;
    }
    td::BufferSlice b;
    if (chunks_.size() > 1 || !last_chunk_free_) {
      if (x.size() <= max_size) {
        b = std::move(x);
        chunks_.pop_front();
      } else {
        b = x.clone();
        b.truncate(max_size);
        x.confirm_read(max_size);
      }
    } else {
      b = x.clone();
      CHECK(b.size() >= last_chunk_free_);
      if (b.size() == last_chunk_free_) {
        return td::BufferSlice{};
      }
      b.truncate(b.size() - last_chunk_free_);
      if (b.size() > max_size) {
        b.truncate(max_size);
      }
      x.confirm_read(b.size());
    }
    ready_bytes_ -= b.size();
    run_callbacks();
    return b;
  }
  return td::BufferSlice{};
}

HttpHeader HttpPayload::get_header() {
  const std::lock_guard<std::mutex> lock{mutex_};
  if (trailer_.size() == 0) {
    return HttpHeader{};
  } else {
    auto h = std::move(trailer_.front());
    auto s = h.size();
    trailer_.pop_front();
    ready_bytes_ -= s;
    run_callbacks();
    return h;
  }
}

void HttpPayload::run_callbacks() {
  for (auto &x : callbacks_) {
    if (state_.load(std::memory_order_relaxed) == ParseState::completed) {
      x->completed();
    } else {
      x->run(ready_bytes_);
    }
  }
}

bool HttpPayload::store_http(td::ChainBufferWriter &output, size_t max_size, HttpPayload::PayloadType store_type) {
  if (store_type == PayloadType::pt_empty) {
    return false;
  }
  slice_gc();
  bool wrote = false;
  while (chunks_.size() > 0 && max_size > 0) {
    auto cur_state = state_.load(std::memory_order_consume);
    auto s = get_slice(max_size);
    if (s.size() == 0) {
      if (cur_state != ParseState::reading_trailer && cur_state != ParseState::completed) {
        return wrote;
      } else {
        break;
      }
    }
    CHECK(s.size() <= max_size);
    max_size -= s.size();
    if (store_type == PayloadType::pt_chunked) {
      char buf[64];
      ::sprintf(buf, "%lx\r\n", s.size());
      auto slice = td::Slice(buf, strlen(buf));
      wrote = true;
      output.append(slice);
    }

    wrote |= !s.empty();
    output.append(std::move(s));

    if (store_type == PayloadType::pt_chunked) {
      output.append(td::Slice("\r\n", 2));
      wrote = true;
    }
  }
  auto cur_state = state_.load(std::memory_order_consume);
  if (chunks_.size() != 0 || (cur_state != ParseState::reading_trailer && cur_state != ParseState::completed)) {
    return wrote;
  }
  if (!written_zero_chunk_) {
    if (store_type == PayloadType::pt_chunked) {
      output.append(td::Slice("0\r\n", 3));
      wrote = true;
    }
    written_zero_chunk_ = true;
  }

  if (store_type != PayloadType::pt_chunked) {
    written_trailer_ = true;
    return wrote;
  }

  while (max_size > 0) {
    cur_state = state_.load(std::memory_order_consume);
    HttpHeader h = get_header();
    if (h.empty()) {
      if (cur_state != ParseState::completed) {
        return wrote;
      } else {
        break;
      }
    }
    auto s = h.size();
    h.store_http(output);
    wrote = true;
    if (max_size <= s) {
      return wrote;
    }
    max_size -= s;
  }

  if (!written_trailer_) {
    output.append(td::Slice("\r\n", 2));
    written_trailer_ = true;
    wrote = true;
  }
  return wrote;
}

tl_object_ptr<ton_api::http_payloadPart> HttpPayload::store_tl(size_t max_size) {
  auto b = ready_bytes();
  if (b > max_size) {
    b = max_size;
  }
  max_size = b;
  td::BufferSlice x{b};
  auto S = x.as_slice();
  auto obj = create_tl_object<ton_api::http_payloadPart>(std::move(x),
                                                         std::vector<tl_object_ptr<ton_api::http_header>>(), false);

  slice_gc();
  while (chunks_.size() > 0 && max_size > 0) {
    auto cur_state = state_.load(std::memory_order_consume);
    auto s = get_slice(max_size);
    if (s.size() == 0) {
      if (cur_state != ParseState::reading_trailer && cur_state != ParseState::completed) {
        LOG(INFO) << "state not trailer/completed";
        obj->data_.truncate(obj->data_.size() - S.size());
        return obj;
      } else {
        break;
      }
    }
    CHECK(s.size() <= max_size);
    S.copy_from(s);
    S.remove_prefix(s.size());
    max_size -= s.size();
  }
  obj->data_.truncate(obj->data_.size() - S.size());
  auto cur_state = state_.load(std::memory_order_consume);
  if (chunks_.size() != 0 || (cur_state != ParseState::reading_trailer && cur_state != ParseState::completed)) {
    return obj;
  }
  if (!written_zero_chunk_) {
    written_zero_chunk_ = true;
  }

  LOG(INFO) << "data completed";

  while (max_size > 0) {
    cur_state = state_.load(std::memory_order_consume);
    HttpHeader h = get_header();
    if (h.empty()) {
      if (cur_state != ParseState::completed) {
        LOG(INFO) << "state not completed";
        return obj;
      } else {
        break;
      }
    }
    auto s = h.size();
    obj->trailer_.push_back(h.store_tl());
    if (max_size <= s) {
      return obj;
    }
    max_size -= s;
  }

  written_trailer_ = true;
  obj->last_ = true;
  return obj;
}

/*tl_object_ptr<ton_api::http_payloadPart> HttpPayload::store_tl(size_t max_size) {
  auto obj = create_tl_object<ton_api::http_payloadPart>(std::vector<td::BufferSlice>(),
                                                         std::vector<tl_object_ptr<ton_api::http_header>>(), false);
  if (type_ == PayloadType::pt_empty) {
    return obj;
  }
  size_t sum = 0;
  while (chunks_.size() > 0) {
    auto &p = chunks_.front();
    size_t s = p.size();
    bool m = true;
    if (chunks_.size() == 1) {
      s -= last_chunk_free_;
      m = false;
    }
    sum += s;

    if (m) {
      obj->data_.push_back(std::move(p));
    } else {
      auto B = p.clone();
      B.truncate(s);
      obj->data_.push_back(std::move(B));
      p.confirm_read(s);
    }
    CHECK(ready_bytes_ >= s);
    ready_bytes_ -= s;
    if (!m) {
      return obj;
    }
    chunks_.pop_front();
    if (sum > max_size) {
      return obj;
    }
  }
  if (state_ != ParseState::reading_trailer && state_ != ParseState::completed) {
    return obj;
  }
  if (!written_zero_chunk_) {
    written_zero_chunk_ = true;
  }
  while (true) {
    if (trailer_.size() == 0) {
      break;
    }
    auto &p = trailer_.front();
    sum += p.name.size() + p.value.size() + 2;
    ready_bytes_ -= p.name.size() + p.value.size() + 2;
    obj->trailer_.push_back(p.store_tl());
    trailer_.pop_front();
    if (sum > max_size) {
      return obj;
    }
  }
  if (state_ != ParseState::completed) {
    return obj;
  }
  obj->last_ = true;
  return obj;
}

tl_object_ptr<ton_api::http_PayloadInfo> HttpPayload::store_info(size_t max_size) {
  if (type_ == PayloadType::pt_empty) {
    return create_tl_object<ton_api::http_payloadInfo>(create_tl_object<ton_api::http_payloadEmpty>());
  }
  if (!parse_completed()) {
    return create_tl_object<ton_api::http_payloadSizeUnknown>();
  }
  if (ready_bytes_ > max_size) {
    return create_tl_object<ton_api::http_payloadBig>(ready_bytes_);
  }
  auto obj = store_tl(max_size);
  CHECK(obj->last_);
  return create_tl_object<ton_api::http_payloadInfo>(
      create_tl_object<ton_api::http_payload>(std::move(obj->data_), std::move(obj->trailer_)));
}*/

void HttpPayload::add_callback(std::unique_ptr<HttpPayload::Callback> callback) {
  const std::lock_guard<std::mutex> lock{mutex_};
  callbacks_.push_back(std::move(callback));
}

td::Result<std::unique_ptr<HttpResponse>> HttpResponse::parse(std::unique_ptr<HttpResponse> response,
                                                              std::string &cur_line, bool force_no_payload,
                                                              bool keep_alive, bool &exit_loop,
                                                              td::ChainBufferReader &input) {
  exit_loop = false;
  CHECK(!response || !response->check_parse_header_completed());
  while (true) {
    bool read;
    TRY_RESULT(line, util::get_line(input, cur_line, read, HttpRequest::max_one_header_size()));
    if (!read) {
      exit_loop = true;
      break;
    }

    if (!response) {
      auto v = td::full_split(line, ' ', 3);
      if (v.size() != 3) {
        return td::Status::Error("expected http header in form ");
      }
      TRY_RESULT(code, td::to_integer_safe<td::uint32>(std::move(v[1])));
      TRY_RESULT_ASSIGN(response, HttpResponse::create(v[0], code, v[2], force_no_payload, keep_alive));
    } else {
      if (line.size() == 0) {
        TRY_STATUS(response->complete_parse_header());
        break;
      } else {
        TRY_RESULT(h, util::get_header(std::move(line)));
        TRY_STATUS(response->add_header(std::move(h)));
      }
    }
  }

  return std::move(response);
}

HttpResponse::HttpResponse(std::string proto_version, td::uint32 code, std::string reason, bool force_no_payload,
                           bool keep_alive, bool is_tunnel)
    : proto_version_(std::move(proto_version))
    , code_(code)
    , reason_(std::move(reason))
    , force_no_payload_(force_no_payload)
    , force_no_keep_alive_(!keep_alive)
    , is_tunnel_(is_tunnel) {
}

td::Result<std::unique_ptr<HttpResponse>> HttpResponse::create(std::string proto_version, td::uint32 code,
                                                               std::string reason, bool force_no_payload,
                                                               bool keep_alive, bool is_tunnel) {
  if (proto_version != "HTTP/1.0" && proto_version != "HTTP/1.1") {
    return td::Status::Error(PSTRING() << "unsupported http version '" << proto_version << "'");
  }

  if (code < 100 || code > 999) {
    return td::Status::Error(PSTRING() << "bad status code '" << code << "'");
  }

  return std::make_unique<HttpResponse>(std::move(proto_version), code, std::move(reason), force_no_payload,
                                        keep_alive, is_tunnel);
}

td::Status HttpResponse::complete_parse_header() {
  CHECK(!parse_header_completed_);
  parse_header_completed_ = true;
  return td::Status::OK();
}

bool HttpResponse::check_parse_header_completed() const {
  return parse_header_completed_;
}

td::Result<std::shared_ptr<HttpPayload>> HttpResponse::create_empty_payload() {
  CHECK(check_parse_header_completed());

  if (!need_payload()) {
    return std::make_shared<HttpPayload>(HttpPayload::PayloadType::pt_empty);
  } else if (is_tunnel_) {
    return std::make_shared<HttpPayload>(HttpPayload::PayloadType::pt_tunnel, low_watermark(), high_watermark());
  } else if (found_content_length_) {
    return std::make_shared<HttpPayload>(HttpPayload::PayloadType::pt_content_length, low_watermark(), high_watermark(),
                                         content_length_);
  } else if (found_transfer_encoding_) {
    return std::make_shared<HttpPayload>(HttpPayload::PayloadType::pt_chunked, low_watermark(), high_watermark());
  } else {
    return std::make_shared<HttpPayload>(HttpPayload::PayloadType::pt_eof, low_watermark(), high_watermark());
  }
}

bool HttpResponse::need_payload() const {
  return !force_no_payload_ && (code_ >= 200) && code_ != 204 && code_ != 304;
}

td::Status HttpResponse::add_header(HttpHeader header) {
  auto lc_name = header.name;
  auto lc_value = header.value;
  std::transform(lc_name.begin(), lc_name.end(), lc_name.begin(), [](unsigned char c) { return std::tolower(c); });
  std::transform(lc_value.begin(), lc_value.end(), lc_value.begin(), [](unsigned char c) { return std::tolower(c); });

  auto S = td::trim(td::Slice(lc_value));

  if (lc_name == "content-length") {
    TRY_RESULT(len, td::to_integer_safe<td::uint32>(S));
    if (found_transfer_encoding_ || found_content_length_) {
      return td::Status::Error("duplicate Content-Length/Transfer-Encoding");
    }
    content_length_ = len;
    found_content_length_ = true;
  } else if (lc_name == "transfer-encoding") {
    // expect chunked, don't event check
    if (found_transfer_encoding_ || found_content_length_) {
      return td::Status::Error("duplicate Content-Length/Transfer-Encoding");
    }
    found_transfer_encoding_ = true;
  } else if (lc_name == "connection" && S == "keep-alive") {
    keep_alive_ = true;
    return td::Status::OK();
  } else if (lc_name == "connection" && S == "close") {
    keep_alive_ = false;
    return td::Status::OK();
  } else if (lc_name == "proxy-connection" && S == "keep-alive") {
    keep_alive_ = true;
    return td::Status::OK();
  } else if (lc_name == "proxy-connection" && S == "close") {
    keep_alive_ = false;
    return td::Status::OK();
  }
  options_.emplace_back(std::move(header));
  return td::Status::OK();
}

void HttpResponse::store_http(td::ChainBufferWriter &output) {
  std::string line = proto_version_ + " " + std::to_string(code_) + " " + reason_ + "\r\n";
  output.append(line);
  for (auto &x : options_) {
    x.store_http(output);
  }
  if (!is_tunnel_) {
    if (keep_alive_) {
      HttpHeader{"Connection", "Keep-Alive"}.store_http(output);
    } else {
      HttpHeader{"Connection", "Close"}.store_http(output);
    }
  }
  output.append(td::Slice("\r\n", 2));
}

tl_object_ptr<ton_api::http_response> HttpResponse::store_tl() {
  std::vector<tl_object_ptr<ton_api::http_header>> headers;
  headers.reserve(options_.size());
  for (auto &h : options_) {
    headers.push_back(h.store_tl());
  }
  if (keep_alive_) {
    headers.push_back(HttpHeader{"Connection", "Keep-Alive"}.store_tl());
  } else {
    headers.push_back(HttpHeader{"Connection", "Close"}.store_tl());
  }
  return create_tl_object<ton_api::http_response>(proto_version_, code_, reason_, std::move(headers), false);
}

td::Status HttpHeader::basic_check() {
  for (auto &c : name) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ':') {
      return td::Status::Error("bad character in header name");
    }
  }
  for (auto &c : value) {
    if (c == '\r' || c == '\n') {
      return td::Status::Error("bad character in header value");
    }
  }
  return td::Status::OK();
}

void answer_error(HttpStatusCode code, std::string reason,
                  td::Promise<std::pair<std::unique_ptr<HttpResponse>, std::shared_ptr<HttpPayload>>> promise) {
  if (reason.empty()) {
    switch (code) {
      case status_ok:
        reason = "OK";
        break;
      case status_bad_request:
        reason = "Bad Request";
        break;
      case status_method_not_allowed:
        reason = "Method Not Allowed";
        break;
      case status_internal_server_error:
        reason = "Internal Server Error";
        break;
      case status_bad_gateway:
        reason = "Bad Gateway";
        break;
      case status_gateway_timeout:
        reason = "Gateway Timeout";
        break;
      default:
        reason = "Unknown";
        break;
    }
  }
  auto response = HttpResponse::create("HTTP/1.0", code, reason, false, false).move_as_ok();
  response->add_header(HttpHeader{"Content-Length", "0"});
  response->complete_parse_header();
  auto payload = response->create_empty_payload().move_as_ok();
  payload->complete_parse();
  CHECK(payload->parse_completed());
  promise.set_value(std::make_pair(std::move(response), std::move(payload)));
}

}  // namespace http

}  // namespace ton
