#include <future>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>

#include "td/actor/actor.h"
#include "td/actor/coro.h"
#include "td/actor/coro_utils.h"
#include "td/net/FramedPipe.h"
#include "td/net/Pipe.h"
#include "td/net/TcpListener.h"
#include "td/utils/as.h"
#include "td/utils/buffer.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/SocketFd.h"
#include "td/utils/port/sleep.h"

using namespace td::actor;

Task<td::Unit> example_create() {
  LOG(INFO) << "Detach";
  Task<int> value = []() -> Task<int> {
    LOG(FATAL) << "This line will not be executed";
    co_return 17;
  }();
  value.detach();

  LOG(INFO) << "Simple co_await";
  // Task will be started after co await has been called
  Task<int> value2 = []() -> Task<int> { co_return 17; }();
  CHECK(17 == co_await std::move(value2));

  LOG(INFO) << "start_immediate than co_await";
  // Task is started immediately
  StartedTask<int> value3 = []() -> Task<int> {
    td::usleep_for(1000000);
    co_return 17;
  }()
                                        .start_immediate();
  CHECK(value3.await_ready());
  CHECK(17 == (co_await std::move(value3)));

  LOG(INFO) << "start than co_await";
  // Task is started on scheduler
  StartedTask<int> value4 = []() -> Task<int> {
    td::usleep_for(1000000);
    co_return 17;
  }()
                                        .start();
  CHECK(!value4.await_ready());
  CHECK(17 == (co_await std::move(value4)));

  StartedTask<int> value5 = spawn_actor("worker", []() -> Task<int> {
    // This code will be run on some actor
    // Main reason to use it is actor statistics
    co_return 17;
  }());
  CHECK(17 == (co_await std::move(value5)));
  co_return td::Unit();
}

Task<td::Unit> example_communicate() {
  LOG(INFO) << "Communicate with actor";
  struct Worker : public Actor {
    int square(int x) {
      return x * x;
    }
    Task<int> square_task(int x) {
      co_return square(x);
    }
    void square_promise(int x, td::Promise<int> promise) {
      send_closure(actor_id(this), &Worker::square_task, x, std::move(promise));
    }
  };
  auto worker = create_actor<Worker>("worker");

  StartedTask<int> value6 = ask(worker, &Worker::square, 17);
  CHECK(289 == (co_await std::move(value6)));
  StartedTask<int> value7 = ask(worker, &Worker::square_promise, 17);
  CHECK(289 == (co_await std::move(value7)));
  StartedTask<int> value8 = ask(worker, &Worker::square_task, 17);
  CHECK(289 == (co_await std::move(value8)));
  co_return td::Unit();
}

Task<int> task_error() {
  co_return td::Status::Error("test error");
}
td::Result<int> result_error() {
  return td::Status::Error("test error");
}
Task<int> pass_task_error() {
  co_await task_error();
  co_return 17;
}
Task<int> pass_result_error() {
  co_await result_error();
  co_return 17;
}

Task<td::Unit> example_error_handling() {
  // Error handling
  (co_await pass_task_error().wrap()).ensure_error();
  (co_await pass_result_error().wrap()).ensure_error();
  (co_await result_error().wrap()).ensure_error();

  co_return td::Unit();
}

Task<td::Unit> example_actor() {
  struct TaskActor : public Actor {
    TaskActor(td::Promise<int> promise) : promise_(std::move(promise)) {
    }
    void start_up() override {
      // it is usual actor all coroutines create FROM actor, will be executed ON actor
      run().start().detach();
    }
    Task<td::Unit> run() {
      state_ = 19;
      finish();
      co_return td::Unit();
    }

   private:
    td::Promise<int> promise_;
    int state_{17};

    void finish() {
      promise_.set_result(state_);
      stop();
    }
  };

  auto [task, promise] = StartedTask<int>::make_bridge();
  auto task_actor = create_actor<TaskActor>("task_actor", std::move(promise));
  CHECK(19 == (co_await std::move(task)));
  co_return td::Unit();
}

Task<td::Unit> example_all() {
  std::vector<StartedTask<int>> v;
  int n = std::thread::hardware_concurrency();
  for (int i = 0; i < n; i++) {
    v.push_back([](int i) -> Task<int> {
      td::usleep_for(1000000);
      co_return i* i;
    }(i)
                                 .start());
  }
  auto vv = co_await all(std::move(v));
  for (int i = 0; i < n; i++) {
    CHECK(vv[i] == i * i);
  }
  co_return td::Unit();
}

Task<td::Unit> example_echo_server() {
  LOG(INFO) << "Echo server example using TaskActor, TcpListener, and SocketPipe";

  // Echo server connection handler
  class EchoConnection : public TaskActor<td::Unit> {
   public:
    explicit EchoConnection(td::SocketPipe pipe) : pipe_(std::move(pipe)) {
    }

   private:
    td::SocketPipe pipe_;
    int messages_count_{0};
    bool received_shutdown_{false};

    void start_up() override {
      LOG(INFO) << "Echo server: client connected";
      pipe_.subscribe();
    }

    Task<Action> task_loop_once() override {
      // Read from socket
      co_await pipe_.flush_read();

      // Process all available messages
      while (!received_shutdown_) {
        td::BufferSlice message;
        auto r_needed = td::framed_read(pipe_.input_buffer(), message);

        if (r_needed.is_error()) {
          LOG(ERROR) << "Framing error: " << r_needed.error();
          co_return Action::Finish;
        }

        auto needed = r_needed.move_as_ok();
        if (needed > 0) {
          break;  // Need more data
        }

        messages_count_++;
        LOG(INFO) << "Server received: " << message.as_slice();

        // Check for shutdown signal (empty message)
        if (message.empty()) {
          LOG(INFO) << "Server shutting down, processed " << messages_count_ - 1 << " messages";
          received_shutdown_ = true;
          break;
        }

        // Echo it back
        co_await td::framed_write(pipe_.output_buffer(), message.as_slice());
      }
      // Write responses
      co_await pipe_.flush_write();
      if (received_shutdown_ && pipe_.left_unwritten() == 0) {
        LOG(INFO) << "Server: all messages echoed successfully";
        co_return Action::Finish;
      }

      co_return Action::KeepRunning;
    }

    Task<td::Unit> finish(td::Status status) override {
      if (status.is_error()) {
        LOG(INFO) << "Connection closed with error: " << status;
      } else {
        LOG(INFO) << "Connection closed normally";
      }
      co_return td::Unit();
    }
  };

  // TCP Listener that spawns EchoConnection for each client
  class EchoServer : public td::TcpListener::Callback {
   public:
    void accept(td::SocketFd fd) override {
      LOG(INFO) << "Accepting new connection";
      auto pipe = td::make_socket_pipe(std::move(fd));
      spawn_task_actor<EchoConnection>("echo_conn", std::move(pipe)).detach();
    }
  };

  // Echo client
  class EchoClient : public TaskActor<td::Unit> {
   public:
    EchoClient(td::SocketPipe pipe, int target_messages) : pipe_(std::move(pipe)), target_messages_(target_messages) {
    }

   private:
    td::SocketPipe pipe_;
    int messages_sent_{0};
    int messages_received_{0};
    int target_messages_;

    void start_up() override {
      LOG(INFO) << "Echo client: connected to server";
      pipe_.subscribe();
    }

    Task<Action> task_loop_once() override {
      // Receive echoes
      co_await pipe_.flush_read();

      while (true) {
        td::BufferSlice message;
        auto needed = co_await td::framed_read(pipe_.input_buffer(), message);

        if (needed > 0) {
          break;  // Need more data
        }

        LOG(INFO) << "Client received echo: " << message.as_slice();
        messages_received_++;

        if (messages_received_ >= target_messages_) {
          LOG(INFO) << "Client: all messages echoed successfully";
          co_return Action::Finish;
        }
      }

      if (messages_sent_ < target_messages_ && messages_sent_ < messages_received_ + 2) {
        auto message = td::BufferSlice(PSLICE() << "Hello from client #" << messages_sent_);
        LOG(INFO) << "Client sent: " << message.as_slice();

        co_await td::framed_write(pipe_.output_buffer(), message.as_slice());
        messages_sent_++;
        co_await pipe_.flush_write();
      } else if (messages_sent_ == target_messages_) {
        // Send shutdown signal (empty message)
        LOG(INFO) << "Client sending shutdown signal";
        co_await td::framed_write(pipe_.output_buffer(), td::Slice());
        messages_sent_++;
        co_await pipe_.flush_write();
      }

      co_return Action::KeepRunning;
    }

    Task<td::Unit> finish(td::Status status) override {
      LOG(INFO) << "Echo client: finished: " << status;
      co_await std::move(status);
      co_return td::Unit();
    }
  };

  // Start TCP listener on localhost
  int port = 8895;
  auto listener = td::actor::create_actor<td::TcpInfiniteListener>("TcpListener", port, std::make_unique<EchoServer>());

  co_await td::actor::coro_sleep(td::Timestamp::in(1));

  // Connect client to server
  td::IPAddress server_addr;
  server_addr.init_host_port("127.0.0.1", port).ensure();

  auto socket = co_await td::SocketFd::open(server_addr);

  auto client_pipe = td::make_socket_pipe(std::move(socket));
  auto client_task = spawn_task_actor<EchoClient>("echo_client", std::move(client_pipe), 20);

  // Wait for client to complete
  co_await std::move(client_task);

  LOG(INFO) << "Echo server example completed successfully";
  co_return td::Unit();
}

Task<td::Unit> run_all_examples() {
  co_await example_create();
  co_await example_communicate();
  co_await example_error_handling();
  co_await example_actor();
  co_await example_all();
  co_await example_echo_server();
  co_return td::Unit();
}

Task<td::Unit> example() {
  LOG(INFO) << "Start example coroutine";

  (co_await run_all_examples().wrap()).ensure();

  LOG(INFO) << "Finish example coroutine and stop scheduler";
  td::actor::SchedulerContext::get()->stop();
  co_return td::Unit();
}

int main() {
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(INFO));
  td::actor::Scheduler scheduler({std::thread::hardware_concurrency()});

  scheduler.run_in_context([&] { (void)example().start(); });

  scheduler.run();
  LOG(INFO) << "DONE";
  return 0;
}