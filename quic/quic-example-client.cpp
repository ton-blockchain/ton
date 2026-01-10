#include <iostream>
#include <optional>

#include "crypto/Ed25519.h"
#include "td/actor/actor.h"
#include "td/utils/OptionParser.h"
#include "td/utils/base64.h"

#include "quic-server.h"

class QuicTester : public td::actor::Actor {
 public:
  class Callback : public ton::quic::QuicServer::Callback {
   public:
    explicit Callback(td::actor::ActorId<QuicTester> tester) : tester_(std::move(tester)) {
    }

    td::Status on_connected(ton::quic::QuicConnectionId cid, td::SecureString public_key) override {
      auto public_key_b64 = td::base64_encode(public_key.as_slice());
      LOG(INFO) << "connected";
      LOG(INFO) << "server public key: " << public_key_b64;
      td::actor::send_closure(tester_, &QuicTester::on_connected, cid);
      return td::Status::OK();
    }

    void on_stream_data(ton::quic::QuicConnectionId cid, ton::quic::QuicStreamID sid, td::BufferSlice data) override {
      std::cout.flush();
      std::cout.write(data.data(), static_cast<std::streamsize>(data.size()));
      std::cout.flush();
    }

    void on_stream_end(ton::quic::QuicConnectionId cid, ton::quic::QuicStreamID sid) override {
      LOG(INFO) << "disconnected";
      std::exit(0);
    }

   private:
    td::actor::ActorId<QuicTester> tester_;
  };

  QuicTester(td::Slice host, int port, td::Ed25519::PrivateKey client_key, td::Slice alpn, int local_port)
      : alpn_(alpn), host_(host), port_(port), local_port_(local_port), client_key_(std::move(client_key)) {
  }

  void start_up() override {
    auto public_key_r = client_key_.get_public_key();
    if (public_key_r.is_ok()) {
      auto public_key_b64 = td::base64_encode(public_key_r.ok().as_octet_string().as_slice());
      LOG(INFO) << "client public key: " << public_key_b64;
    }

    auto cb = std::make_unique<Callback>(actor_id(this));
    auto R =
        ton::quic::QuicServer::create(local_port_, std::move(client_key_), std::move(cb), alpn_.as_slice(), "0.0.0.0");
    if (R.is_error()) {
      LOG(ERROR) << "failed to start local QUIC client: " << R.error();
      std::exit(1);
    }
    server_ = R.move_as_ok();

    LOG(INFO) << "connecting to " << host_.as_slice() << ':' << port_;

    auto client_key_copy_r = td::Ed25519::generate_private_key();
    if (client_key_copy_r.is_error()) {
      LOG(ERROR) << "failed to generate connection key: " << client_key_copy_r.error();
      std::exit(1);
    }

    send_closure(server_, &ton::quic::QuicServer::connect, host_.as_slice(), port_, client_key_copy_r.move_as_ok(),
                 alpn_.as_slice(), [](auto R) {
                   if (R.is_error()) {
                     LOG(ERROR) << "connection failed: " << R.error();
                     std::exit(1);
                   }
                 });
  }

  void on_connected(ton::quic::QuicConnectionId cid) {
    td::actor::send_closure(
        server_.get(), &ton::quic::QuicServer::open_stream, cid,
        td::PromiseCreator::lambda([server = server_.get(), cid](td::Result<ton::quic::QuicStreamID> R) {
          if (R.is_error()) {
            LOG(ERROR) << "open_stream failed: " << R.error();
            std::exit(1);
          }
          auto sid = R.move_as_ok();
          td::actor::send_closure(server, &ton::quic::QuicServer::send_stream_data, cid, sid,
                                  td::BufferSlice("GET /\r\n"));
          td::actor::send_closure(server, &ton::quic::QuicServer::send_stream_end, cid, sid);
        }));
  }

 private:
  td::BufferSlice alpn_;
  td::BufferSlice host_;
  int port_;
  int local_port_;
  td::Ed25519::PrivateKey client_key_;

  td::actor::ActorOwn<ton::quic::QuicServer> server_;
};

int main(int argc, char** argv) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  std::optional<td::BufferSlice> alpn;
  std::optional<td::BufferSlice> host;
  std::optional<int> port;
  std::optional<int> local_port;

  td::OptionParser p;
  p.set_description("HTTP/0.9 over QUIC tester using RPK");
  p.add_option('h', "host", "server hostname", [&](td::Slice arg) { host = td::BufferSlice(arg); });
  p.add_checked_option('p', "port", "server port", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(port, td::to_integer_safe<int>(arg));
    return td::Status::OK();
  });
  p.add_checked_option('l', "local-port", "local port (default: 0 = any)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(local_port, td::to_integer_safe<int>(arg));
    return td::Status::OK();
  });
  p.run(argc, argv).ensure();

  if (!alpn.has_value()) {
    alpn = td::BufferSlice("hq-interop");
  }
  if (!host.has_value()) {
    LOG(ERROR) << "no host specified";
    std::exit(1);
  }
  if (!port.has_value()) {
    LOG(ERROR) << "no port specified";
    std::exit(1);
  }
  if (!local_port.has_value()) {
    local_port = 0;
  }

  auto client_key_r = td::Ed25519::generate_private_key();
  if (client_key_r.is_error()) {
    LOG(ERROR) << "failed to generate client key: " << client_key_r.error();
    std::exit(1);
  }

  td::actor::ActorOwn<QuicTester> tester;
  td::actor::Scheduler scheduler({1});
  scheduler.run_in_context([&] {
    tester =
        td::actor::create_actor<QuicTester>(PSTRING() << "tester", host.value().as_slice(), port.value(),
                                            client_key_r.move_as_ok(), alpn.value().as_slice(), local_port.value());
  });
  scheduler.run();
}
