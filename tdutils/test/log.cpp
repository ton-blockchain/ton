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

#include <chrono>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <thread>
#include <vector>

#include "td/utils/AsyncFileLog.h"
#include "td/utils/FileLog.h"
#include "td/utils/Slice.h"
#include "td/utils/TimestampFormat.h"
#include "td/utils/TsFileLog.h"
#include "td/utils/benchmark.h"
#include "td/utils/date.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/port/path.h"
#include "td/utils/port/thread.h"
#include "td/utils/port/thread_local.h"
#include "td/utils/tests.h"

char disable_linker_warning_about_empty_file_tdutils_test_log_cpp TD_UNUSED;

#if !TD_THREAD_UNSUPPORTED
template <class Log>
class LogBenchmark : public td::Benchmark {
 public:
  LogBenchmark(std::string name, int threads_n, std::function<td::unique_ptr<Log>()> creator)
      : name_(std::move(name)), threads_n_(threads_n), creator_(std::move(creator)) {
  }
  std::string get_description() const override {
    return PSTRING() << name_ << " " << td::tag("threads_n", threads_n_);
  }
  void start_up() override {
    log_ = creator_();
    threads_.resize(threads_n_);
  }
  void tear_down() override {
    auto paths = log_->get_file_paths();
    log_.reset();
    for (auto path : paths) {
      td::unlink(path).ignore();
    }
  }
  void run(int n) override {
    for (auto &thread : threads_) {
      thread = td::thread([this, n] { this->run_thread(n); });
    }
    for (auto &thread : threads_) {
      thread.join();
    }
  }

  void run_thread(int n) {
    auto str = PSTRING() << "#" << n << " : fsjklfdjsklfjdsklfjdksl\n";
    for (int i = 0; i < n; i++) {
      if (i % 10000 == 0) {
        log_->rotate();
      }
      log_->append(str);
    }
  }

 private:
  std::string name_;
  td::unique_ptr<Log> log_;
  int threads_n_{0};
  std::function<td::unique_ptr<Log>()> creator_;
  std::vector<td::thread> threads_;
};

template <class F>
static void bench_log(std::string name, int threads_n, F &&f) {
  bench(LogBenchmark<typename decltype(f())::element_type>(std::move(name), threads_n, std::move(f)));
};

TEST(Log, TsLogger) {
  bench_log("NewTsFileLog", 4,
            [] { return td::TsFileLog::create("tmplog", std::numeric_limits<td::int64>::max(), false).move_as_ok(); });
  bench_log("TsFileLog", 8, [] {
    class FileLog : public td::LogInterface {
     public:
      FileLog() {
        file_log_.init("tmplog", std::numeric_limits<td::int64>::max(), false).ensure();
        ts_log_.init(&file_log_);
      }
      ~FileLog() {
      }
      void append(td::CSlice slice) override {
        ts_log_.append(slice, -1);
      }
      std::vector<std::string> get_file_paths() override {
        return file_log_.get_file_paths();
      }

     private:
      td::FileLog file_log_;
      td::TsLog ts_log_{nullptr};
    };
    return td::make_unique<FileLog>();
  });

  bench_log("noop", 4, [] {
    class NoopLog : public td::LogInterface {
     public:
      void append(td::CSlice slice) override {
      }
    };
    return td::make_unique<NoopLog>();
  });

  bench_log("FileLog", 4, [] {
    class FileLog : public td::LogInterface {
     public:
      FileLog() {
        file_log_.init("tmplog", std::numeric_limits<td::int64>::max(), false).ensure();
      }
      ~FileLog() {
      }
      void append(td::CSlice slice) override {
        file_log_.append(slice, -1);
      }
      std::vector<std::string> get_file_paths() override {
        return file_log_.get_file_paths();
      }

     private:
      td::FileLog file_log_;
    };
    return td::make_unique<FileLog>();
  });

  bench_log("AsyncFileLog", 4, [] {
    return td::AsyncFileLog::create("tmplog_async", std::numeric_limits<td::int64>::max(), false).move_as_ok();
  });
}
#endif

#if !TD_THREAD_UNSUPPORTED
namespace {
void verify_async_log_file(const std::string &path, int threads_n, int per_thread) {
  auto content = td::read_file_str(path).move_as_ok();
  ASSERT_TRUE(content.find("dropped") == std::string::npos);  // writer keeps up -> no drops

  // Every line must be present exactly once and each thread's lines must appear in FIFO order.
  std::map<int, int> next_seq;  // thread -> expected next s
  int total = 0;
  size_t pos = 0;
  while (pos < content.size()) {
    size_t eol = content.find('\n', pos);
    ASSERT_TRUE(eol != std::string::npos);
    int t = -1, s = -1;
    ASSERT_TRUE(std::sscanf(content.c_str() + pos, "t=%d s=%d", &t, &s) == 2);
    ASSERT_EQ(next_seq[t], s);  // per-thread FIFO preserved
    next_seq[t] = s + 1;
    total++;
    pos = eol + 1;
  }
  ASSERT_EQ(threads_n * per_thread, total);
  for (int t = 0; t < threads_n; t++) {
    ASSERT_EQ(per_thread, next_seq[t]);
  }
}

template <class Thread>
void run_async_log_test(const std::string &path, int threads_n, int per_thread) {
  td::unlink(path).ignore();
  {
    auto log =
        td::AsyncFileLog::create(path, std::numeric_limits<td::int64>::max(), /*redirect_stderr=*/false).move_as_ok();
    std::vector<Thread> threads;
    for (int t = 0; t < threads_n; t++) {
      threads.emplace_back([log = log.get(), t, per_thread] {
        for (int s = 0; s < per_thread; s++) {
          char buf[64];
          int n = std::snprintf(buf, sizeof(buf), "t=%d s=%d\n", t, s);
          log->append(td::CSlice(buf, buf + n), 3);
        }
      });
    }
    for (auto &th : threads) {
      th.join();
    }
    // log destroyed here -> writer performs its final drain and joins
  }
  verify_async_log_file(path, threads_n, per_thread);
  td::unlink(path).ignore();
}
}  // namespace

TEST(Log, AsyncFileLog) {
  run_async_log_test<td::thread>("tmp_async_file_log", 8, 20000);
}

TEST(Log, AsyncFileLogInvalidRingCapacity) {
  auto r_log = td::AsyncFileLog::create("tmp_async_file_log_bad_capacity", std::numeric_limits<td::int64>::max(),
                                        /*redirect_stderr=*/false, 1000);
  ASSERT_TRUE(r_log.is_error());
  td::unlink("tmp_async_file_log_bad_capacity").ignore();
}

TEST(Log, AsyncFileLogDrops) {
  std::string path = "tmp_async_file_log_drops";
  td::unlink(path).ignore();
  td::uint64 dropped_count = 0;
  {
    td::AsyncFileLog log;
    log.init(path, std::numeric_limits<td::int64>::max(), /*redirect_stderr=*/false, 16).ensure();
    log.append("this line is too large for a 16 byte ring\n", 3);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (log.get_dropped_count() != 1 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    dropped_count = log.get_dropped_count();
  }
  auto content = td::read_file_str(path).move_as_ok();
  ASSERT_EQ(1u, dropped_count);
  ASSERT_TRUE(content.find("[async_log] dropped 1 log lines") != std::string::npos);
  td::unlink(path).ignore();
}

TEST(Log, AsyncFileLogThreadRecreation) {
  std::string path = "tmp_async_file_log_thread_recreation";
  td::unlink(path).ignore();
  constexpr int threads_n = static_cast<int>(td::MAX_THREADS) + 32;
  {
    auto log =
        td::AsyncFileLog::create(path, std::numeric_limits<td::int64>::max(), /*redirect_stderr=*/false).move_as_ok();
    for (int i = 0; i < threads_n; i++) {
      td::thread thread([log = log.get(), i] {
        auto line = PSTRING() << "i=" << i << "\n";
        log->append(line, 3);
      });
      thread.join();
    }
  }
  auto content = td::read_file_str(path).move_as_ok();
  ASSERT_TRUE(content.find("dropped") == std::string::npos);
  int lines = 0;
  for (char c : content) {
    lines += c == '\n';
  }
  ASSERT_EQ(threads_n, lines);
  td::unlink(path).ignore();
}

TEST(Log, AsyncFileLogReuseAfterDestroy) {
  // A thread may log to one AsyncFileLog and then, after it is destroyed, to another -- each line to its file.
  std::string pa = "tmp_async_reuse_a";
  std::string pb = "tmp_async_reuse_b";
  td::unlink(pa).ignore();
  td::unlink(pb).ignore();
  {
    auto a =
        td::AsyncFileLog::create(pa, std::numeric_limits<td::int64>::max(), /*redirect_stderr=*/false).move_as_ok();
    a->append("aaa\n", 3);
  }
  {
    auto b =
        td::AsyncFileLog::create(pb, std::numeric_limits<td::int64>::max(), /*redirect_stderr=*/false).move_as_ok();
    b->append("bbb\n", 3);
  }
  ASSERT_TRUE(td::read_file_str(pa).move_as_ok().find("aaa") != std::string::npos);
  ASSERT_TRUE(td::read_file_str(pb).move_as_ok().find("bbb") != std::string::npos);
  td::unlink(pa).ignore();
  td::unlink(pb).ignore();
}

TEST(Log, AsyncFileLogSwitchLiveLoggers) {
  std::string pa = "tmp_async_switch_a";
  std::string pb = "tmp_async_switch_b";
  td::unlink(pa).ignore();
  td::unlink(pb).ignore();
  {
    auto a =
        td::AsyncFileLog::create(pa, std::numeric_limits<td::int64>::max(), /*redirect_stderr=*/false).move_as_ok();
    auto b =
        td::AsyncFileLog::create(pb, std::numeric_limits<td::int64>::max(), /*redirect_stderr=*/false).move_as_ok();
    a->append("a1\n", 3);
    b->append("b1\n", 3);
    a->append("a2\n", 3);
    b->append("b2\n", 3);
  }
  auto ca = td::read_file_str(pa).move_as_ok();
  auto cb = td::read_file_str(pb).move_as_ok();
  ASSERT_TRUE(ca.find("a1") != std::string::npos);
  ASSERT_TRUE(ca.find("a2") != std::string::npos);
  ASSERT_TRUE(ca.find("b1") == std::string::npos);
  ASSERT_TRUE(ca.find("b2") == std::string::npos);
  ASSERT_TRUE(cb.find("b1") != std::string::npos);
  ASSERT_TRUE(cb.find("b2") != std::string::npos);
  ASSERT_TRUE(cb.find("a1") == std::string::npos);
  ASSERT_TRUE(cb.find("a2") == std::string::npos);
  td::unlink(pa).ignore();
  td::unlink(pb).ignore();
}

#if !TD_USE_ASAN
TEST(Log, AsyncFileLogStdThread) {
  // Skipped under ASAN: raw std::thread teardown trips an ASAN+jemalloc TSD-cleanup issue at thread exit unrelated
  // to logging (td::thread is TON's integrated thread type).
  run_async_log_test<std::thread>("tmp_async_file_log_std", 6, 10000);
}
#endif
#endif

TEST(Log, format_system_clock) {
  using namespace std::chrono;
  auto check = [](system_clock::time_point tp) {
    char buf[td::TIMESTAMP_BUF_SIZE];
    auto s = td::format_system_clock(td::MutableSlice(buf, sizeof(buf)), tp);
    ASSERT_EQ(date::format("%F %T", tp), s.str());
  };

  auto base = system_clock::now();
  check(base);
  // sweep sub-second fractions, time-of-day, and day rollovers (~2.3 days of ~1s steps)
  for (long long i = 0; i < 200000; i++) {
    check(base + duration_cast<system_clock::duration>(microseconds(i * 999983LL)));
  }
  // span ~30 years to exercise the date arithmetic
  for (int k = 0; k < 5000; k++) {
    check(base + duration_cast<system_clock::duration>(hours(k * 53) + microseconds(k * 31 % 1000000)));
  }
}
