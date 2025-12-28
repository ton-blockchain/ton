#include <iostream>
#include <optional>

#include "td/actor/actor.h"
#include "td/utils/OptionParser.h"

#include "quic-client.h"

class QuicTester : public td::actor::Actor {
 public:
  class Callback : public ton::quic::QuicClient::Callback {
   public:
    explicit Callback(QuicTester& tester) : tester_(tester) {
    }

    void on_connected() override {
      LOG(INFO) << "connected to " << tester_.host_ << ':' << tester_.port_;
      td::Promise<ton::quic::QuicStreamID> P = td::make_promise([this](td::Result<ton::quic::QuicStreamID> R) {
        auto sid = R.move_as_ok();
        td::actor::send_closure(tester_.connection_.get(), &ton::quic::QuicClient::send_stream_data, sid,
                                td::Slice("GET /\r\n"));
        td::actor::send_closure(tester_.connection_.get(), &ton::quic::QuicClient::send_stream_end, sid);
      });
      td::actor::send_closure(tester_.connection_.get(), &ton::quic::QuicClient::open_stream, std::move(P));
    }

    void on_stream_data(ton::quic::QuicStreamID, td::Slice data) override {
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

  QuicTester(td::Slice host, int port, td::Slice alpn) : alpn_(alpn), host_(host), port_(port) {
  }

  void start_up() override {
    [this] {
      TRY_RESULT_ASSIGN(connection_,
                        ton::quic::QuicClient::connect(host_, port_, std::make_unique<Callback>(*this), alpn_));
      return td::Status::OK();
    }()
        .ensure();
  }

 private:
  td::Slice alpn_;
  td::Slice host_;
  int port_;

  td::actor::ActorOwn<ton::quic::QuicClient> connection_ = {};
};

int main(int argc, char** argv) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  std::optional<td::BufferSlice> alpn;
  std::optional<td::BufferSlice> host;
  std::optional<int> port;

  td::OptionParser p;
  p.set_description("HTTP/0.9 over QUIC tester");
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

  td::actor::ActorOwn<QuicTester> tester;
  td::actor::Scheduler scheduler({1});
  scheduler.run_in_context([&] {
    tester = td::actor::create_actor<QuicTester>(PSTRING() << "tester", td::CSlice(host.value().as_slice()),
                                                 port.value(), alpn.value().as_slice());
  });
  scheduler.run();
}