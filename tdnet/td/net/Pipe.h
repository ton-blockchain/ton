#pragma once

#include "td/actor/ActorId.h"
#include "td/actor/coro_task.h"
#include "td/utils/BufferedFd.h"
#include "td/utils/Observer.h"
#include "td/utils/buffer.h"
#include "td/utils/port/SocketFd.h"

namespace td {

class PipeBase {
 public:
  virtual ~PipeBase() = default;
  virtual void subscribe() = 0;
  virtual void destroy() = 0;
  virtual Status flush_read() = 0;
  virtual Status flush_write() = 0;
  virtual ChainBufferReader& input_buffer() = 0;
  virtual ChainBufferWriter& output_buffer() = 0;
  virtual size_t left_unread() const = 0;
  virtual size_t left_unwritten() const = 0;
};

template <class FdType>
class ExtractablePipe : public PipeBase {
 public:
  virtual actor::Task<FdType> extract_fd() = 0;
};

class SocketPipe {
 public:
  SocketPipe() = default;
  SocketPipe(SocketPipe&&) = default;
  SocketPipe& operator=(SocketPipe&&) = default;
  ~SocketPipe();

  explicit operator bool() const;

  void subscribe();
  Status flush_read();
  Status flush_write();
  ChainBufferReader& input_buffer();
  ChainBufferWriter& output_buffer();
  size_t left_unread() const;
  size_t left_unwritten() const;
  actor::Task<BufferedFd<SocketFd>> extract_fd();

  explicit SocketPipe(std::shared_ptr<PipeBase> impl);

 private:
  friend class Pipe;
  std::shared_ptr<PipeBase> impl_;
};

class Pipe {
 public:
  Pipe() = default;
  Pipe(Pipe&&) = default;
  Pipe& operator=(Pipe&&) = default;

  ~Pipe();

  explicit operator bool() const;

  void subscribe();
  Status flush_read();
  Status flush_write();
  ChainBufferReader& input_buffer();
  ChainBufferWriter& output_buffer();
  size_t left_unread() const;
  size_t left_unwritten() const;
  explicit Pipe(std::shared_ptr<PipeBase> impl);

  // Allow conversion from SocketPipe
  Pipe(SocketPipe&& other);

  Pipe& operator=(SocketPipe&& other);

 private:
  std::shared_ptr<PipeBase> impl_;
};

SocketPipe make_socket_pipe(SocketFd&& fd);

SocketPipe make_socket_pipe(BufferedFd<SocketFd>&& fd);

// This is probably not what you think it is, probably you don't want to use it
// - returns pipe which could be given to somebody else. The other party may subscribe to our writes
// - returns an observer which could be used to notify the subscribed party about new written data
// - subscribes current actor on writes to this pipe from the other side
std::pair<Pipe, td::Observer> make_pipe(ChainBufferReader input, ChainBufferWriter output);

}  // namespace td
