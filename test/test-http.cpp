/* 
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission 
    to link the code of portions of this program with the OpenSSL library. 
    You must obey the GNU General Public License in all respects for all 
    of the code used other than OpenSSL. If you modify file(s) with this 
    exception, you may extend this exception to your version of the file(s), 
    but you are not obligated to do so. If you do not wish to do so, delete this 
    exception statement from your version. If you delete this exception statement 
    from all source files in the program, then also delete it here.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "td/utils/OptionsParser.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/port/path.h"
#include "td/utils/Random.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/FileFd.h"
#include "td/utils/overloaded.h"
#include "common/errorlog.h"
#include "http/http.h"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif
#include <iostream>
#include <sstream>

#include <set>

void dump_reader(td::ChainBufferReader &reader) {
  auto b = reader.move_as_buffer_slice();
  LOG(INFO) << b.as_slice();
}

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  {
    const auto request =
        "GET /pub/WWW/TheProject.html HTTP/1.1\r\n"
        "Host: www.example.org:8080\r\n"
        "xopt: opt12345\r\n"
        "\r\n";
    td::ChainBufferWriter w;
    w.init(0);
    w.append(td::Slice(request, std::strlen(request)));
    auto r = w.extract_reader();

    bool exit_loop = false;
    std::string cur_line = "";
    auto R = ton::http::HttpRequest::parse(nullptr, cur_line, exit_loop, r);
    R.ensure();
    auto req = R.move_as_ok();

    td::ChainBufferWriter wo;
    wo.init(0);
    CHECK(req);
    CHECK(!exit_loop);
    req->store_http(wo);
    auto ro = wo.extract_reader();
    dump_reader(ro);

    CHECK(req->method() == "GET");
    CHECK(req->url() == "/pub/WWW/TheProject.html");
    CHECK(req->proto_version() == "HTTP/1.1");
    CHECK(req->host() == "www.example.org:8080");
    CHECK(req->check_parse_header_completed());
    CHECK(!req->need_payload());
  }

  {
    const auto request =
        "GET /pub/WWW/TheProject.html HTTP/1.1\r\n"
        "Host: www.example.org:8080\r\n"
        "xopt: opt12345\r\n"
        "Content-Length: opt12345\r\n"
        "\r\n";
    td::ChainBufferWriter w;
    w.init(0);
    w.append(td::Slice(request, std::strlen(request)));
    auto r = w.extract_reader();

    bool exit_loop = false;
    std::string cur_line = "";
    auto R = ton::http::HttpRequest::parse(nullptr, cur_line, exit_loop, r);
    R.ensure_error();
  }

  {
    const auto request =
        "GET /pub/WWW/TheProject.html HTTP/1.1\r\n"
        "Host: www.example.org:8080\r\n"
        "Content-Length: 123456789\r\n"
        "\r\n";
    td::ChainBufferWriter w;
    w.init(0);
    w.append(td::Slice(request, std::strlen(request)));
    auto r = w.extract_reader();

    bool exit_loop = false;
    std::string cur_line = "";
    auto R = ton::http::HttpRequest::parse(nullptr, cur_line, exit_loop, r);
    R.ensure_error();
  }

  {
    const auto request =
        "GET /pub/WWW/TheProject.html HTTP/1.1\r\n"
        "Host: www.example.org:8080\r\n"
        "Content-Length: 16\r\n"
        "\r\n";
    td::ChainBufferWriter w;
    w.init(0);
    w.append(td::Slice(request, std::strlen(request)));
    auto r = w.extract_reader();

    bool exit_loop = false;
    std::string cur_line = "";
    auto R = ton::http::HttpRequest::parse(nullptr, cur_line, exit_loop, r);
    R.ensure();
    auto req = R.move_as_ok();

    td::ChainBufferWriter wo;
    wo.init(0);
    CHECK(req);
    CHECK(!exit_loop);
    req->store_http(wo);
    auto ro = wo.extract_reader();
    dump_reader(ro);

    CHECK(req->method() == "GET");
    CHECK(req->url() == "/pub/WWW/TheProject.html");
    CHECK(req->proto_version() == "HTTP/1.1");
    CHECK(req->host() == "www.example.org:8080");
    CHECK(req->check_parse_header_completed());
    CHECK(req->need_payload());
    CHECK(req->keep_alive());

    auto payload = req->create_empty_payload().move_as_ok();
    CHECK(payload);
    CHECK(!payload->parse_completed());
    payload->parse(r).ensure();
    CHECK(!payload->parse_completed());
    w.append("1234567890abcdef");
    r.advance_end(16);
    payload->parse(r).ensure();
    CHECK(payload->parse_completed());

    wo.init(0);
    payload->store_http(wo, 1 << 20, payload->payload_type());
    ro = wo.extract_reader();
    dump_reader(ro);
  }

  {
    const auto request =
        "GET /pub/WWW/TheProject.html HTTP/1.1\r\n"
        "Host: www.example.org:8080\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    td::ChainBufferWriter w;
    w.init(0);
    w.append(td::Slice(request, std::strlen(request)));
    auto r = w.extract_reader();

    bool exit_loop = false;
    std::string cur_line = "";
    auto R = ton::http::HttpRequest::parse(nullptr, cur_line, exit_loop, r);
    R.ensure();
    auto req = R.move_as_ok();

    td::ChainBufferWriter wo;
    wo.init(0);
    CHECK(req);
    CHECK(!exit_loop);
    req->store_http(wo);
    auto ro = wo.extract_reader();
    dump_reader(ro);

    CHECK(req->method() == "GET");
    CHECK(req->url() == "/pub/WWW/TheProject.html");
    CHECK(req->proto_version() == "HTTP/1.1");
    CHECK(req->host() == "www.example.org:8080");
    CHECK(req->check_parse_header_completed());
    CHECK(req->need_payload());
    CHECK(req->keep_alive());

    auto payload = req->create_empty_payload().move_as_ok();
    CHECK(payload);
    CHECK(!payload->parse_completed());
    payload->parse(r).ensure();
    CHECK(!payload->parse_completed());
    w.append("10\r\n1234567890abcdef\r\n");
    r.advance_end(22);
    payload->parse(r).ensure();
    CHECK(!payload->parse_completed());
    w.append("10\r\n1234567890ABCDEF\r\n");
    r.advance_end(22);
    payload->parse(r).ensure();
    CHECK(!payload->parse_completed());
    w.append("0\r\n");
    r.advance_end(5);
    payload->parse(r).ensure();
    CHECK(!payload->parse_completed());

    w.append("X-tail: value\r\n\r\n");
    r.advance_end(17);
    payload->parse(r).ensure();
    CHECK(payload->parse_completed());

    wo.init(0);
    payload->store_http(wo, 1 << 20, payload->payload_type());
    ro = wo.extract_reader();
    dump_reader(ro);
  }

  {
    const auto request =
        "GET /pub/WWW/TheProject.html HTTP/1.1\r\n"
        "Host: www.example.org:8080\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "10\r\n"
        "0123456789abcdef\r\n"
        "10\r\n"
        "0123456789ABCDEF\r\n"
        "0\r\n"
        "x-1: a\r\n"
        "x-2: b\r\n"
        "\r\n";
    td::ChainBufferWriter w;
    w.init(0);
    auto r = w.extract_reader();
    w.append(td::Slice(request, std::strlen(request)));

    std::unique_ptr<ton::http::HttpRequest> req;
    std::shared_ptr<ton::http::HttpPayload> payload;

    auto l = strlen(request);
    std::string cur_line;
    bool exit_loop;
    for (size_t i = 0; i < l; i++) {
      r.advance_end(1);
      if (!req || !req->check_parse_header_completed()) {
        req = ton::http::HttpRequest::parse(std::move(req), cur_line, exit_loop, r).move_as_ok();
      } else {
        if (!payload) {
          payload = req->create_empty_payload().move_as_ok();
        }
        CHECK(!payload->parse_completed());
        payload->parse(r).ensure();
      }
    }
    CHECK(payload->parse_completed());
    td::ChainBufferWriter wo;
    wo.init(0);
    req->store_http(wo);
    payload->store_http(wo, 1 << 20, payload->payload_type());
    auto ro = wo.extract_reader();
    dump_reader(ro);
  }

  {
    const auto response =
        "HTTP/1.1 200 Ok\r\n"
        "\r\n";
    td::ChainBufferWriter w;
    w.init(0);
    w.append(td::Slice(response, std::strlen(response)));
    auto r = w.extract_reader();

    bool exit_loop = false;
    std::string cur_line = "";
    auto R = ton::http::HttpResponse::parse(nullptr, cur_line, false, false, exit_loop, r);
    R.ensure();
    auto res = R.move_as_ok();
    CHECK(res);
    CHECK(res->check_parse_header_completed());

    td::ChainBufferWriter wo;
    wo.init(0);
    res->store_http(wo);
    auto ro = wo.extract_reader();
    dump_reader(ro);
  }

  std::_Exit(0);
  return 0;
}
