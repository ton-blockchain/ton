#include <iostream>
#include <optional>

#include "crypto/Ed25519.h"
#include "td/actor/actor.h"
#include "td/utils/OptionParser.h"
#include "td/utils/base64.h"

#include "quic-client.h"

class QuicTester : public td::actor::Actor {
 public:
  class Callback : public ton::quic::QuicClient::Callback {
   public:
    explicit Callback(QuicTester& tester) : tester_(tester) {
    }

    td::Status on_connected(td::SecureString public_key) override {
      auto public_key_b64 = td::base64_encode(public_key.as_slice());
      LOG(INFO) << "connected to " << tester_.host_ << ':' << tester_.port_;
      LOG(INFO) << "server public key: " << public_key_b64;
      td::Promise<ton::quic::QuicStreamID> P = td::make_promise([this](td::Result<ton::quic::QuicStreamID> R) {
        auto sid = R.move_as_ok();
        td::actor::send_closure(tester_.connection_.get(), &ton::quic::QuicClient::send_stream_data, sid,
                                td::BufferSlice("GET /\r\n"));
        td::actor::send_closure(tester_.connection_.get(), &ton::quic::QuicClient::send_stream_end, sid);
      });
      td::actor::send_closure(tester_.connection_.get(), &ton::quic::QuicClient::open_stream, std::move(P));
      return td::Status::OK();
    }

    void on_stream_data(ton::quic::QuicStreamID, td::BufferSlice data) override {
      std::cout.flush();
      std::cout.write(data.data(), static_cast<std::streamsize>(data.size()));
      std::cout.flush();
    }

    void on_stream_end(ton::quic::QuicStreamID) override {
      LOG(INFO) << "disconnected from " << tester_.host_ << ':' << tester_.port_;
      std::exit(0);
    }

   private:
    QuicTester& tester_;
  };
  friend Callback;

  QuicTester(td::Slice host, int port, td::Ed25519::PrivateKey client_key, td::Slice alpn)
      : alpn_(alpn), host_(host), port_(port), client_key_(std::move(client_key)) {
  }

  void start_up() override {
    auto public_key_r = client_key_.get_public_key();
    if (public_key_r.is_ok()) {
      auto public_key_b64 = td::base64_encode(public_key_r.ok().as_octet_string().as_slice());
      LOG(INFO) << "client public key: " << public_key_b64;
    }

    [this] {
      TRY_RESULT_ASSIGN(connection_, ton::quic::QuicClient::connect_rpk(host_, port_, std::move(client_key_),
                                                                        std::make_unique<Callback>(*this), alpn_));
      return td::Status::OK();
    }()
        .ensure();
  }

 private:
  td::Slice alpn_;
  td::Slice host_;
  int port_;
  td::Ed25519::PrivateKey client_key_;

  td::actor::ActorOwn<ton::quic::QuicClient> connection_ = {};
};

int main(int argc, char** argv) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  std::optional<td::BufferSlice> alpn;
  std::optional<td::BufferSlice> host;
  std::optional<int> port;

  td::OptionParser p;
  p.set_description("HTTP/0.9 over QUIC tester using RPK");
  p.add_option('h', "host", "server hostname", [&](td::Slice arg) { host = td::BufferSlice(arg); });
  p.add_checked_option('p', "port", "server port", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(port, td::to_integer_safe<int>(arg));
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

  auto client_key_r = td::Ed25519::generate_private_key();
  if (client_key_r.is_error()) {
    LOG(ERROR) << "failed to generate client key: " << client_key_r.error();
    std::exit(1);
  }

  td::actor::ActorOwn<QuicTester> tester;
  td::actor::Scheduler scheduler({1});
  scheduler.run_in_context([&] {
    tester = td::actor::create_actor<QuicTester>(PSTRING() << "tester", td::CSlice(host.value().as_slice()),
                                                 port.value(), client_key_r.move_as_ok(), alpn.value().as_slice());
  });
  scheduler.run();
}
