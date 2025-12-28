#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "td/actor/actor.h"
#include "td/utils/OptionParser.h"

#include "quic-server.h"

class QuicHttpServer : public td::actor::Actor {
 public:
  class ServerCallback final : public ton::quic::QuicServer::Callback {
   public:
    explicit ServerCallback(td::actor::ActorId<QuicHttpServer> server) : server_(std::move(server)) {
    }

    void on_connected(const td::IPAddress &peer) override {
      td::actor::send_closure(server_, &QuicHttpServer::on_connected, peer);
    }

    void on_stream_data(const td::IPAddress &peer, ton::quic::QuicStreamID sid, td::Slice data) override {
      td::actor::send_closure(server_, &QuicHttpServer::on_stream_data, peer, td::BufferSlice(data));
    }

    void on_stream_end(const td::IPAddress &peer, ton::quic::QuicStreamID sid) override {
      td::actor::send_closure(server_, &QuicHttpServer::on_stream_end, peer);
    }

   private:
    td::actor::ActorId<QuicHttpServer> server_;
  };

  QuicHttpServer(int port, td::Slice cert_file, td::Slice key_file, td::Slice alpn, ton::quic::QuicStreamID sid,
                 td::Slice bind_host)
      : port_(port)
      , sid_(sid)
      , cert_file_(cert_file)
      , key_file_(key_file)
      , alpn_(alpn)
      , bind_host_(bind_host) {
  }

  void start_up() override {
    auto cb = std::make_unique<ServerCallback>(actor_id(this));
    auto R = ton::quic::QuicServer::listen(port_, cert_file_.as_slice(), key_file_.as_slice(), std::move(cb),
                                          alpn_.as_slice(), bind_host_.as_slice());
    if (R.is_error()) {
      LOG(ERROR) << "failed to start QUIC server: " << R.error();
      std::exit(1);
    }
    server_ = R.move_as_ok();

    LOG(INFO) << "listening on " << bind_host_.as_slice() << ':' << port_ << " (ALPN: " << alpn_.as_slice() << ")";
  }

 private:
  void on_connected(const td::IPAddress &peer) {
    LOG(INFO) << "connected: " << peer;
  }

  void on_stream_data(const td::IPAddress &peer, td::BufferSlice data) {
    auto &buf = request_buf_[peer];
    auto s = data.as_slice();
    buf.append(s.data(), s.size());
  }

  void on_stream_end(const td::IPAddress &peer) {
    auto it = request_buf_.find(peer);
    std::string req;
    if (it != request_buf_.end()) {
      req = std::move(it->second);
      request_buf_.erase(it);
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
                                 << "Peer: " << peer << "\n"
                                 << "Request: " << first_line << "\n";

    std::string resp = PSTRING() << "HTTP/1.1 200 OK\r\n"
                                 << "Content-Type: text/plain\r\n"
                                 << "Content-Length: " << body.size() << "\r\n"
                                 << "Connection: close\r\n"
                                 << "\r\n"
                                 << body;

    responses_.push_back(std::move(resp));
    const auto &stored = responses_.back();

    LOG(INFO) << "request finished from " << peer << ", replying on stream " << sid_;
    td::actor::send_closure(server_.get(), &ton::quic::QuicServer::send_stream_data, peer, sid_, td::Slice(stored));
    td::actor::send_closure(server_.get(), &ton::quic::QuicServer::send_stream_end, peer, sid_);

    while (responses_.size() > 1024) {
      responses_.pop_front();
    }
  }

  int port_;
  ton::quic::QuicStreamID sid_;
  td::BufferSlice cert_file_;
  td::BufferSlice key_file_;
  td::BufferSlice alpn_;
  td::BufferSlice bind_host_;

  td::actor::ActorOwn<ton::quic::QuicServer> server_;

  std::map<td::IPAddress, std::string> request_buf_;

  std::deque<std::string> responses_;
};

int main(int argc, char **argv) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  std::optional<td::BufferSlice> cert_file;
  std::optional<td::BufferSlice> key_file;
  std::optional<td::BufferSlice> alpn;
  std::optional<td::BufferSlice> bind_host;
  std::optional<int> port;
  std::optional<ton::quic::QuicStreamID> sid;

  td::OptionParser p;
  p.set_description("HTTP/1.1-over-QUIC demo server (hq-interop)");
  p.add_option('c', "cert", "TLS certificate file (PEM)", [&](td::Slice arg) { cert_file = td::BufferSlice(arg); });
  p.add_option('k', "key", "TLS private key file (PEM)", [&](td::Slice arg) { key_file = td::BufferSlice(arg); });
  p.add_option('a', "alpn", "ALPN (default: hq-interop)", [&](td::Slice arg) { alpn = td::BufferSlice(arg); });
  p.add_option('b', "bind", "bind host (default: 0.0.0.0)", [&](td::Slice arg) { bind_host = td::BufferSlice(arg); });
  p.add_checked_option('p', "port", "UDP port to listen on", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(port, td::to_integer_safe<int>(arg));
    return td::Status::OK();
  });
  p.add_checked_option('s', "sid", "stream id to reply on (default: 0)", [&](td::Slice arg) {
    int64_t v;
    TRY_RESULT_ASSIGN(v, td::to_integer_safe<int64_t>(arg));
    sid = v;
    return td::Status::OK();
  });
  p.run(argc, argv).ensure();

  if (!alpn.has_value()) {
    alpn = td::BufferSlice("hq-interop");
  }
  if (!bind_host.has_value()) {
    bind_host = td::BufferSlice("0.0.0.0");
  }
  if (!sid.has_value()) {
    sid = 0;
  }
  if (!cert_file.has_value()) {
    LOG(ERROR) << "no --cert provided";
    std::exit(1);
  }
  if (!key_file.has_value()) {
    LOG(ERROR) << "no --key provided";
    std::exit(1);
  }
  if (!port.has_value()) {
    LOG(ERROR) << "no --port provided";
    std::exit(1);
  }

  td::actor::Scheduler scheduler({1});
  td::actor::ActorOwn<QuicHttpServer> server;
  scheduler.run_in_context([&] {
    server = td::actor::create_actor<QuicHttpServer>(
        PSTRING() << "quic-http-server@" << bind_host.value().as_slice() << ':' << port.value(), port.value(),
        cert_file.value().as_slice(), key_file.value().as_slice(), alpn.value().as_slice(), sid.value(),
        bind_host.value().as_slice());
  });
  scheduler.run();
}