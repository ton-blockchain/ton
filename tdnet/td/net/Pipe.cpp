#include <atomic>

#include "td/actor/coro_utils.h"
#include "td/net/Pipe.h"
#include "td/utils/CancellationToken.h"

namespace td {
namespace {

struct PollFdActor : public td::actor::Actor {
  static td::actor::ActorOwn<PollFdActor> create(td::PollableFd fd, std::shared_ptr<void> parent) {
    auto options = td::actor::ActorOptions().with_name("PollFdActor").with_poll();
    return td::actor::create_actor<PollFdActor>(options, std::move(fd), std::move(parent));
  }

  explicit PollFdActor(td::PollableFd fd, std::shared_ptr<void> parent)
      : fd_(std::move(fd)), fd_ref_(fd_.ref()), parent_(std::move(parent)) {
  }

  void unsubscribe() {
    CHECK(!closed_);
    td::actor::SchedulerContext::get()->get_poll().unsubscribe(fd_ref_);
    closed_ = true;
    stop();
  }

  void destroy() {
    CHECK(!closed_);
    td::actor::SchedulerContext::get()->get_poll().unsubscribe_before_close(fd_ref_);
    closed_ = true;
    stop();
  }

 private:
  td::PollableFd fd_;
  td::PollableFdRef fd_ref_;
  std::shared_ptr<void> parent_;
  bool closed_{false};

  void start_up() override {
    td::actor::SchedulerContext::get()->get_poll().subscribe(std::move(fd_), td::PollFlags::ReadWrite());
  }

  void tear_down() override {
    CHECK(closed_);
  }
};

}  // namespace

namespace {

template <class FdType>
class FdPipe : public ExtractablePipe<FdType>,
               private td::ObserverBase,
               public std::enable_shared_from_this<FdPipe<FdType>> {
 public:
  explicit FdPipe(FdType&& fd) : fd_(std::move(fd)) {
  }
  FdPipe() = default;

  void subscribe() override {
    listener_ = actor::detail::get_current_actor_id();
    poll_actor_ = PollFdActor::create(fd_.get_poll_info().extract_pollable_fd(this), this->shared_from_this());
  }

  void destroy() override {
    if (!poll_actor_.empty()) {
      send_closure(std::move(poll_actor_), &PollFdActor::destroy);
    }
  }

  Status flush_read() override {
    if (fd_.empty()) {
      return Status::OK();
    }
    sync_with_poll(fd_);
    TRY_STATUS(fd_.get_pending_error());
    TRY_STATUS(fd_.flush_read());
    return Status::OK();
  }

  Status flush_write() override {
    if (fd_.empty()) {
      return Status::OK();
    }
    TRY_STATUS(fd_.flush_write());
    if (can_close_local(fd_)) {
      return Status::Error("closed");
    }
    return Status::OK();
  }

  ChainBufferReader& input_buffer() override {
    return fd_.input_buffer();
  }

  ChainBufferWriter& output_buffer() override {
    return fd_.output_buffer();
  }

  // ExtractablePipe interface
  actor::Task<FdType> extract_fd() override {
    auto fd = std::move(fd_);
    if (!poll_actor_.empty()) {
      co_await ask(std::move(poll_actor_), &PollFdActor::unsubscribe);
    }
    co_return std::move(fd);
  }
  size_t left_unread() const override {
    return fd_.left_unread();
  }
  size_t left_unwritten() const override {
    return fd_.left_unwritten();
  }

 private:
  FdType fd_;
  td::actor::ActorOwn<PollFdActor> poll_actor_;
  td::actor::ActorId<> listener_;

  // ObserverBase interface - called when poll events occur
  void notify() override {
    CHECK(!listener_.empty());
    td::actor::send_signals(listener_, td::actor::ActorSignals::wakeup());
  }
};

}  // namespace

SocketPipe make_socket_pipe(SocketFd&& fd) {
  auto impl = std::make_shared<FdPipe<BufferedFd<SocketFd>>>(BufferedFd<SocketFd>(std::move(fd)));
  return SocketPipe(std::move(impl));
}

SocketPipe make_socket_pipe(BufferedFd<SocketFd>&& fd) {
  auto impl = std::make_shared<FdPipe<BufferedFd<SocketFd>>>(std::move(fd));
  return SocketPipe(std::move(impl));
}

namespace {

struct BufferPipeImpl : public PipeBase {
  using Fd = Unit;
  struct Callback {
    virtual ~Callback() = default;
    virtual void notify() = 0;
    virtual void subscribe() = 0;
    virtual Status get_pending_error() = 0;
  };

  BufferPipeImpl(ChainBufferReader input, ChainBufferWriter output, std::unique_ptr<Callback> callback)
      : input_(std::move(input)), output_(std::move(output)), callback_(std::move(callback)) {
  }

  void subscribe() override {
    callback_->subscribe();
  }

  void destroy() override {
    callback_.reset();
  }

  Status flush_read() override {
    input_.sync_with_writer();
    return callback_->get_pending_error();
  }

  Status flush_write() override {
    if (output_dirty_) {
      callback_->notify();
      output_dirty_ = false;
    }
    return callback_->get_pending_error();
  }

  ChainBufferReader& input_buffer() override {
    return input_;
  }

  ChainBufferWriter& output_buffer() override {
    output_dirty_ = true;
    return output_;
  }

  size_t left_unread() const override {
    return input_.size();
  }
  size_t left_unwritten() const override {
    return output_dirty_ ? 1 : 0;
  }

 private:
  ChainBufferReader input_;
  bool output_dirty_{false};
  ChainBufferWriter output_;
  std::unique_ptr<Callback> callback_;
};

}  // namespace

std::pair<Pipe, Observer> make_pipe(ChainBufferReader input, ChainBufferWriter output) {
  struct SimpleObserver : public ObserverBase {
    void on_destroy() override {
      cancellation_token_source_.cancel();
      notify();
    }
    void notify() override {
      if (has_listener_.test()) {
        actor::send_signals(listener_, td::actor::ActorSignals::wakeup());
      }
    }
    void set_listener(actor::ActorId<> listener) {
      listener_ = std::move(listener);
      CHECK(!has_listener_.test_and_set());
      notify();
    }

    CancellationToken get_cancellation_token() {
      return cancellation_token_source_.get_cancellation_token();
    }

   private:
    CancellationTokenSource cancellation_token_source_;
    std::atomic_flag has_listener_ = ATOMIC_FLAG_INIT;
    actor::ActorId<> listener_;
  };

  auto observer_ptr = std::make_shared<SimpleObserver>();
  Observer observer{observer_ptr};

  struct Callback : public BufferPipeImpl::Callback {
    actor::ActorOwn<> listener_;
    CancellationToken cancellation_token_;
    std::shared_ptr<SimpleObserver> observer_ptr_;

    void notify() override {
      actor::send_signals(listener_, td::actor::ActorSignals::wakeup());
    }

    void subscribe() override {
      observer_ptr_->set_listener(actor::detail::get_current_actor_id());
    }

    Status get_pending_error() override {
      return cancellation_token_.check();
    }
  };

  auto callback = std::make_unique<Callback>();
  callback->listener_ = actor::ActorOwn<>{actor::detail::get_current_actor_id()};
  callback->cancellation_token_ = observer_ptr->get_cancellation_token();
  callback->observer_ptr_ = std::move(observer_ptr);

  auto proxy_fd = std::make_shared<BufferPipeImpl>(std::move(input), std::move(output), std::move(callback));
  auto fd = Pipe(std::move(proxy_fd));

  return {std::move(fd), std::move(observer)};
}

SocketPipe::~SocketPipe() {
  if (impl_) {
    impl_->destroy();
  }
}

SocketPipe::operator bool() const {
  return static_cast<bool>(impl_);
}

void SocketPipe::subscribe() {
  if (impl_) {
    impl_->subscribe();
  }
}

Status SocketPipe::flush_read() {
  if (!impl_) {
    return Status::OK();
  }
  return impl_->flush_read();
}

Status SocketPipe::flush_write() {
  if (!impl_) {
    return Status::OK();
  }
  return impl_->flush_write();
}

ChainBufferReader& SocketPipe::input_buffer() {
  CHECK(impl_);
  return impl_->input_buffer();
}

ChainBufferWriter& SocketPipe::output_buffer() {
  CHECK(impl_);
  return impl_->output_buffer();
}
size_t SocketPipe::left_unread() const {
  CHECK(impl_);
  return impl_->left_unread();
}
size_t SocketPipe::left_unwritten() const {
  CHECK(impl_);
  return impl_->left_unwritten();
}

actor::Task<BufferedFd<SocketFd>> SocketPipe::extract_fd() {
  auto extractable = std::static_pointer_cast<ExtractablePipe<BufferedFd<SocketFd>>>(impl_);
  auto fd = co_await extractable->extract_fd();
  impl_.reset();
  co_return std::move(fd);
}

SocketPipe::SocketPipe(std::shared_ptr<PipeBase> impl) : impl_(std::move(impl)) {
}

Pipe::~Pipe() {
  if (impl_) {
    impl_->destroy();
  }
}

Pipe::operator bool() const {
  return static_cast<bool>(impl_);
}

void Pipe::subscribe() {
  if (impl_) {
    impl_->subscribe();
  }
}

Status Pipe::flush_read() {
  if (!impl_) {
    return Status::OK();
  }
  return impl_->flush_read();
}

Status Pipe::flush_write() {
  if (!impl_) {
    return Status::OK();
  }
  return impl_->flush_write();
}

ChainBufferReader& Pipe::input_buffer() {
  CHECK(impl_);
  return impl_->input_buffer();
}

ChainBufferWriter& Pipe::output_buffer() {
  CHECK(impl_);
  return impl_->output_buffer();
}

size_t Pipe::left_unread() const {
  CHECK(impl_);
  return impl_->left_unread();
}

size_t Pipe::left_unwritten() const {
  CHECK(impl_);
  return impl_->left_unwritten();
}

Pipe::Pipe(std::shared_ptr<PipeBase> impl) : impl_(std::move(impl)) {
}

Pipe::Pipe(SocketPipe&& other) : impl_(std::move(other.impl_)) {
}

Pipe& Pipe::operator=(SocketPipe&& other) {
  if (impl_) {
    impl_->destroy();
  }
  impl_ = std::move(other.impl_);
  return *this;
}

}  // namespace td
