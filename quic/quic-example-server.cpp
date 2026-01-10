#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "crypto/Ed25519.h"
#include "td/actor/actor.h"
#include "td/utils/OptionParser.h"
#include "td/utils/base64.h"

#include "quic-server.h"

class QuicHttpServer : public td::actor::Actor {
 public:
  class ServerCallback final : public ton::quic::QuicServer::Callback {
   public:
    explicit ServerCallback(td::actor::ActorId<QuicHttpServer> server) : server_(std::move(server)) {
    }

    td::Status on_connected(ton::quic::QuicConnectionId cid, td::SecureString public_key) override {
      td::actor::send_closure(server_, &QuicHttpServer::on_connected, cid, std::move(public_key));
      return td::Status::OK();
    }

    void on_stream_data(ton::quic::QuicConnectionId cid, ton::quic::QuicStreamID sid, td::BufferSlice data) override {
      td::actor::send_closure(server_, &QuicHttpServer::on_stream_data, cid, sid, std::move(data));
    }

    void on_stream_end(ton::quic::QuicConnectionId cid, ton::quic::QuicStreamID sid) override {
      td::actor::send_closure(server_, &QuicHttpServer::on_stream_end, cid, sid);
    }

   private:
    td::actor::ActorId<QuicHttpServer> server_;
  };

  QuicHttpServer(int port, td::Ed25519::PrivateKey server_key, td::Slice alpn, td::Slice bind_host)
      : port_(port), server_key_(std::move(server_key)), alpn_(alpn), bind_host_(bind_host) {
  }

  void start_up() override {
    auto public_key_r = server_key_.get_public_key();
    if (public_key_r.is_error()) {
      LOG(ERROR) << "failed to get public key: " << public_key_r.error();
      std::exit(1);
    }
    auto public_key_b64 = td::base64_encode(public_key_r.ok().as_octet_string().as_slice());

    auto cb = std::make_unique<ServerCallback>(actor_id(this));
    auto R = ton::quic::QuicServer::create(port_, std::move(server_key_), std::move(cb), alpn_.as_slice(),
                                           bind_host_.as_slice());
    if (R.is_error()) {
      LOG(ERROR) << "failed to start QUIC server: " << R.error();
      std::exit(1);
    }
    server_ = R.move_as_ok();

    LOG(INFO) << "listening on " << bind_host_.as_slice() << ':' << port_ << " (ALPN: " << alpn_.as_slice() << ")";
    LOG(INFO) << "server public key: " << public_key_b64;
  }

 private:
  void on_connected(ton::quic::QuicConnectionId cid, td::SecureString public_key) {
    auto public_key_b64 = td::base64_encode(public_key.as_slice());
    LOG(INFO) << "connected: CID, peer public key: " << public_key_b64;
  }

  void on_stream_data(ton::quic::QuicConnectionId cid, ton::quic::QuicStreamID sid, td::BufferSlice data) {
    auto &buf = request_buf_[cid][sid];
    auto s = data.as_slice();
    buf.append(s.data(), s.size());
  }

  void on_stream_end(ton::quic::QuicConnectionId cid, ton::quic::QuicStreamID sid) {
    auto it = request_buf_.find(cid);
    std::string req;
    if (it != request_buf_.end()) {
      auto stream_it = it->second.find(sid);
      if (stream_it != it->second.end()) {
        req = std::move(stream_it->second);
        it->second.erase(stream_it);
      }
    }

    std::string first_line;
    if (!req.empty()) {
      auto pos = req.find("\r\n");
      if (pos == std::string::npos) {
        pos = req.find('\n');
      }
      if (pos == std::string::npos) {
        first_line = req;
      } else {
        first_line = req.substr(0, pos);
      }
    }
    if (first_line.empty()) {
      first_line = "<empty request>";
    }

    std::string body = PSTRING() << "Hello from quic-example-server\n"
                                 << "Request: " << first_line << "\n";

    std::string resp = PSTRING() << "HTTP/1.1 200 OK\r\n"
                                 << "Content-Type: text/plain\r\n"
                                 << "Content-Length: " << body.size() << "\r\n"
                                 << "Connection: close\r\n"
                                 << "\r\n"
                                 << body;

    responses_.push_back(std::move(resp));
    const auto &stored = responses_.back();

    LOG(INFO) << "request finished, replying on stream " << sid;
    td::actor::send_closure(server_.get(), &ton::quic::QuicServer::send_stream_data, cid, sid, td::BufferSlice(stored));
    td::actor::send_closure(server_.get(), &ton::quic::QuicServer::send_stream_end, cid, sid);

    while (responses_.size() > 1024) {
      responses_.pop_front();
    }
  }

  int port_;
  td::Ed25519::PrivateKey server_key_;
  td::BufferSlice alpn_;
  td::BufferSlice bind_host_;

  td::actor::ActorOwn<ton::quic::QuicServer> server_;

  std::map<ton::quic::QuicConnectionId, std::map<ton::quic::QuicStreamID, std::string>> request_buf_;

  std::deque<std::string> responses_;
};

int main(int argc, char **argv) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  std::optional<td::BufferSlice> alpn;
  std::optional<td::BufferSlice> bind_host;
  std::optional<int> port;

  td::OptionParser p;
  p.set_description("HTTP/1.1-over-QUIC demo server (hq-interop) using RPK");
  p.add_option('a', "alpn", "ALPN (default: hq-interop)", [&](td::Slice arg) { alpn = td::BufferSlice(arg); });
  p.add_option('b', "bind", "bind host (default: 0.0.0.0)", [&](td::Slice arg) { bind_host = td::BufferSlice(arg); });
  p.add_checked_option('p', "port", "UDP port to listen on", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(port, td::to_integer_safe<int>(arg));
    return td::Status::OK();
  });
  p.run(argc, argv).ensure();

  if (!alpn.has_value()) {
    alpn = td::BufferSlice("hq-interop");
  }
  if (!bind_host.has_value()) {
    bind_host = td::BufferSlice("0.0.0.0");
  }
  if (!port.has_value()) {
    LOG(ERROR) << "no --port provided";
    std::exit(1);
  }

  auto server_key_r = td::Ed25519::generate_private_key();
  if (server_key_r.is_error()) {
    LOG(ERROR) << "failed to generate server key: " << server_key_r.error();
    std::exit(1);
  }

  td::actor::Scheduler scheduler({1});
  td::actor::ActorOwn<QuicHttpServer> server;
  scheduler.run_in_context([&] {
    server = td::actor::create_actor<QuicHttpServer>(
        PSTRING() << "quic-http-server@" << bind_host.value().as_slice() << ':' << port.value(), port.value(),
        server_key_r.move_as_ok(), alpn.value().as_slice(), bind_host.value().as_slice());
  });
  scheduler.run();
}
