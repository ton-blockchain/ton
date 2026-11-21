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

    Copyright 2017-2020 Telegram Systems LLP
*/
#include <array>
#include <set>

#include "td/utils/Slice.h"
#include "td/utils/Time.h"
#include "td/utils/common.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/FileFd.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/IoSlice.h"
#include "td/utils/port/PollFlags.h"
#include "td/utils/port/UdpSocketFd.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/sleep.h"
#include "td/utils/port/thread.h"
#include "td/utils/port/thread_local.h"
#include "td/utils/tests.h"

using namespace td;

#if TD_PORT_POSIX
namespace {

struct UdpTestPair {
  UdpSocketFd sender;
  UdpSocketFd receiver;
  IPAddress receiver_address;
};

UdpTestPair make_udp_test_pair() {
  IPAddress bind_address;
  bind_address.init_host_port("127.0.0.1", 0).ensure();
  auto sender = UdpSocketFd::open(bind_address).move_as_ok();
  auto receiver = UdpSocketFd::open(bind_address).move_as_ok();
  auto receiver_bound_address = receiver.get_local_address().move_as_ok();
  IPAddress receiver_address;
  receiver_address.init_host_port("127.0.0.1", receiver_bound_address.get_port()).ensure();
  return {std::move(sender), std::move(receiver), std::move(receiver_address)};
}

struct UdpInboundBatch {
  std::array<std::array<char, 64>, 4> buffers;
  std::array<IPAddress, 4> addresses;
  std::array<Status, 4> errors;
  std::array<UdpSocketFd::InboundMessage, 4> messages;

  UdpInboundBatch() {
    for (size_t i = 0; i < messages.size(); i++) {
      messages[i].from = &addresses[i];
      messages[i].data = MutableSlice(buffers[i].data(), buffers[i].size());
      messages[i].error = &errors[i];
    }
  }
};

std::array<UdpSocketFd::OutboundMessage, 3> make_udp_outbound_batch(const IPAddress &to) {
  return {{{.to = &to, .data = "one"}, {.to = &to, .data = "two"}, {.to = &to, .data = "three"}}};
}

struct UdpReceiveResult {
  size_t datagrams = 0;
  size_t calls = 0;
};

// Loopback delivery is asynchronous (on macOS the first call routinely sees nothing at all), so the
// datagrams may take several calls to show up: keep asking until they do. Both the datagram total
// and the number of calls are exact, which is what lets the tests below pin the syscall counter.
UdpReceiveResult receive_datagrams(UdpSocketFd &fd, UdpInboundBatch &inbound, size_t expected) {
  UdpReceiveResult result;
  auto deadline = Timestamp::in(10.0);
  while (true) {
    fd.get_poll_info().add_flags(PollFlags::Read());  // a call that ended in EAGAIN cleared it
    size_t received = 0;
    fd.receive_messages(inbound.messages, received).ensure();
    result.calls++;
    result.datagrams += received;
    if (result.datagrams >= expected || deadline.is_in_past()) {
      return result;
    }
    usleep_for(1000);
  }
}

}  // namespace

TEST(Port, UdpSocketSyscallStatsFallback) {
  auto pair = make_udp_test_pair();
  pair.sender.disable_mmsg();
  pair.receiver.disable_mmsg();

  auto outbound = make_udp_outbound_batch(pair.receiver_address);
  UdpSocketFd::SendResult send_result;
  pair.sender.send_messages(outbound, send_result).ensure();
  ASSERT_EQ(3u, send_result.sent);
  ASSERT_EQ(3u, pair.sender.get_syscall_stats().send);

  UdpInboundBatch inbound;
  auto received = receive_datagrams(pair.receiver, inbound, 3);
  ASSERT_EQ(3u, received.datagrams);
  // One recvmsg per datagram, plus the EAGAIN that ends each call: the queue never holds the four
  // datagrams it would take to fill the batch instead.
  ASSERT_EQ(received.datagrams + received.calls, pair.receiver.get_syscall_stats().receive);

  auto drained = receive_datagrams(pair.receiver, inbound, 0);
  ASSERT_EQ(0u, drained.datagrams);
  ASSERT_EQ(1u, drained.calls);
  ASSERT_EQ(received.datagrams + received.calls + 1, pair.receiver.get_syscall_stats().receive);
}

TEST(Port, UdpSocketSyscallStatsMmsg) {
  auto pair = make_udp_test_pair();
  pair.sender.enable_mmsg();
  pair.receiver.enable_mmsg();
  if (!pair.sender.is_mmsg_enabled() || !pair.receiver.is_mmsg_enabled()) {
    return;
  }

  auto outbound = make_udp_outbound_batch(pair.receiver_address);
  UdpSocketFd::SendResult send_result;
  pair.sender.send_messages(outbound, send_result).ensure();
  ASSERT_EQ(3u, send_result.sent);
  ASSERT_EQ(1u, pair.sender.get_syscall_stats().send);

  UdpInboundBatch inbound;
  auto received = receive_datagrams(pair.receiver, inbound, 3);
  ASSERT_EQ(3u, received.datagrams);
  ASSERT_EQ(received.calls, pair.receiver.get_syscall_stats().receive);  // one recvmmsg per call

  auto drained = receive_datagrams(pair.receiver, inbound, 0);
  ASSERT_EQ(0u, drained.datagrams);
  ASSERT_EQ(1u, drained.calls);
  ASSERT_EQ(received.calls + 1, pair.receiver.get_syscall_stats().receive);
}
#endif

TEST(Port, files) {
  CSlice main_dir = "test_dir";
  rmrf(main_dir).ignore();
  ASSERT_TRUE(FileFd::open(main_dir, FileFd::Write).is_error());
  ASSERT_TRUE(walk_path(main_dir, [](CSlice name, WalkPath::Type type) { UNREACHABLE(); }).is_error());
  mkdir(main_dir).ensure();
  mkdir(PSLICE() << main_dir << TD_DIR_SLASH << "A").ensure();
  mkdir(PSLICE() << main_dir << TD_DIR_SLASH << "B").ensure();
  mkdir(PSLICE() << main_dir << TD_DIR_SLASH << "B" << TD_DIR_SLASH << "D").ensure();
  mkdir(PSLICE() << main_dir << TD_DIR_SLASH << "C").ensure();
  ASSERT_TRUE(FileFd::open(main_dir, FileFd::Write).is_error());
  std::string fd_path = PSTRING() << main_dir << TD_DIR_SLASH << "t.txt";
  std::string fd2_path = PSTRING() << main_dir << TD_DIR_SLASH << "C" << TD_DIR_SLASH << "t2.txt";

  auto fd = FileFd::open(fd_path, FileFd::Write | FileFd::CreateNew).move_as_ok();
  auto fd2 = FileFd::open(fd2_path, FileFd::Write | FileFd::CreateNew).move_as_ok();
  fd2.close();

  int cnt = 0;
  const int ITER_COUNT = 1000;
  for (int i = 0; i < ITER_COUNT; i++) {
    walk_path(main_dir, [&](CSlice name, WalkPath::Type type) {
      if (type == WalkPath::Type::RegularFile) {
        ASSERT_TRUE(name == fd_path || name == fd2_path);
      }
      cnt++;
    }).ensure();
  }
  ASSERT_EQ((5 * 2 + 2) * ITER_COUNT, cnt);
  bool was_abort = false;
  walk_path(main_dir, [&](CSlice name, WalkPath::Type type) {
    CHECK(!was_abort);
    if (type == WalkPath::Type::EnterDir && ends_with(name, PSLICE() << TD_DIR_SLASH << "B")) {
      was_abort = true;
      return WalkPath::Action::Abort;
    }
    return WalkPath::Action::Continue;
  }).ensure();
  CHECK(was_abort);

  cnt = 0;
  bool is_first_dir = true;
  walk_path(main_dir, [&](CSlice name, WalkPath::Type type) {
    cnt++;
    if (type == WalkPath::Type::EnterDir) {
      if (is_first_dir) {
        is_first_dir = false;
      } else {
        return WalkPath::Action::SkipDir;
      }
    }
    return WalkPath::Action::Continue;
  }).ensure();
  ASSERT_EQ(6, cnt);

  ASSERT_EQ(0u, fd.get_size().move_as_ok());
  ASSERT_EQ(12u, fd.write("Hello world!").move_as_ok());
  ASSERT_EQ(4u, fd.pwrite("abcd", 1).move_as_ok());
  char buf[100];
  MutableSlice buf_slice(buf, sizeof(buf));
  ASSERT_TRUE(fd.pread(buf_slice.substr(0, 4), 2).is_error());
  fd.seek(11).ensure();
  ASSERT_EQ(2u, fd.write("?!").move_as_ok());

  ASSERT_TRUE(FileFd::open(main_dir, FileFd::Read | FileFd::CreateNew).is_error());
  fd = FileFd::open(fd_path, FileFd::Read | FileFd::Create).move_as_ok();
  ASSERT_EQ(13u, fd.get_size().move_as_ok());
  ASSERT_EQ(4u, fd.pread(buf_slice.substr(0, 4), 1).move_as_ok());
  ASSERT_STREQ("abcd", buf_slice.substr(0, 4));

  fd.seek(0).ensure();
  ASSERT_EQ(13u, fd.read(buf_slice.substr(0, 13)).move_as_ok());
  ASSERT_STREQ("Habcd world?!", buf_slice.substr(0, 13));
}

TEST(Port, SparseFiles) {
  CSlice path = "sparse.txt";
  unlink(path).ignore();
  auto fd = FileFd::open(path, FileFd::Write | FileFd::CreateNew).move_as_ok();
  ASSERT_EQ(0, fd.get_size().move_as_ok());
  // ASSERT_EQ(0, fd.get_real_size().move_as_ok());
  int64 offset = 100000000;
  fd.pwrite("a", offset).ensure();
  ASSERT_EQ(offset + 1, fd.get_size().move_as_ok());
  auto real_size = fd.get_real_size().move_as_ok();
  if (real_size >= offset + 1) {
    LOG(ERROR) << "File system doesn't support sparse files, rewind during streaming can be slow";
  }
  unlink(path).ensure();
}

TEST(Port, Writev) {
  std::vector<IoSlice> vec;
  CSlice test_file_path = "test.txt";
  unlink(test_file_path).ignore();
  auto fd = FileFd::open(test_file_path, FileFd::Write | FileFd::CreateNew).move_as_ok();
  vec.push_back(as_io_slice("a"));
  vec.push_back(as_io_slice("b"));
  vec.push_back(as_io_slice("cd"));
  ASSERT_EQ(4u, fd.writev(vec).move_as_ok());
  vec.clear();
  vec.push_back(as_io_slice("efg"));
  vec.push_back(as_io_slice(""));
  vec.push_back(as_io_slice("hi"));
  ASSERT_EQ(5u, fd.writev(vec).move_as_ok());
  fd.close();
  fd = FileFd::open(test_file_path, FileFd::Read).move_as_ok();
  Slice expected_content = "abcdefghi";
  ASSERT_EQ(static_cast<int64>(expected_content.size()), fd.get_size().ok());
  std::string content(expected_content.size(), '\0');
  ASSERT_EQ(content.size(), fd.read(content).move_as_ok());
  ASSERT_EQ(expected_content, content);
}

#if TD_PORT_POSIX && !TD_THREAD_UNSUPPORTED
#include <algorithm>
#include <mutex>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

static std::mutex m;
static std::vector<std::string> ptrs;
static std::vector<int *> addrs;
static TD_THREAD_LOCAL int thread_id;

static void on_user_signal(int sig) {
  int addr;
  addrs[thread_id] = &addr;
  char ptr[10];
  snprintf(ptr, 6, "%d", thread_id);
  std::unique_lock<std::mutex> guard(m);
  ptrs.push_back(std::string(ptr));
}

TEST(Post, SignalsAndThread) {
  setup_signals_alt_stack().ensure();
  set_signal_handler(SignalType::User, on_user_signal).ensure();
  std::vector<std::string> ans = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
  {
    std::vector<td::thread> threads;
    int thread_n = 10;
    std::vector<Stage> stages(thread_n);
    ptrs.clear();
    addrs.resize(thread_n);
    for (int i = 0; i < 10; i++) {
      threads.emplace_back([&, i] {
        setup_signals_alt_stack().ensure();
        if (i != 0) {
          stages[i].wait(2);
        }
        thread_id = i;
        pthread_kill(pthread_self(), SIGUSR1);
        if (i + 1 < thread_n) {
          stages[i + 1].wait(2);
        }
      });
    }
    for (auto &t : threads) {
      t.join();
    }
    CHECK(ptrs == ans);

    LOG(ERROR) << ptrs;
    //LOG(ERROR) << std::set<int *>(addrs.begin(), addrs.end()).size();
    //LOG(ERROR) << addrs;
  }

  {
    Stage stage;
    std::vector<td::thread> threads;
    int thread_n = 10;
    ptrs.clear();
    addrs.resize(thread_n);
    for (int i = 0; i < 10; i++) {
      threads.emplace_back([&, i] {
        stage.wait(thread_n);
        thread_id = i;
        pthread_kill(pthread_self(), SIGUSR1);
        //kill(pid_t(syscall(SYS_gettid)), SIGUSR1);
      });
    }
    for (auto &t : threads) {
      t.join();
    }
    std::sort(ptrs.begin(), ptrs.end());
    CHECK(ptrs == ans);
    std::sort(addrs.begin(), addrs.end());
    ASSERT_TRUE(std::unique(addrs.begin(), addrs.end()) == addrs.end());
    //LOG(ERROR) << addrs;
  }
}
#endif
