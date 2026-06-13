// Google Benchmark: producer-side cost of the async logging fast path vs today's synchronous write(2).
//
// Compares, per log line, on the PRODUCER thread:
//   - BM_SyncWriteLine        : one ::write(2) of a preformatted line (what TsFileLog does today)
//   - BM_AsyncEnqueueLine     : frame+memcpy the preformatted line into the per-thread SPSC ring (eager-compat)
//   - BM_AsyncCaptureBinary   : capture rdtsc + a few binary args into the ring (the LOGF deferred fast path)
//   - BM_DisabledGate         : the verbosity gate when the level is disabled (sanity, ~1-2 ns)
// Each is also run multi-threaded (one ring / one fd per producer thread, mirroring TsFileLog's fan-out)
// to show the async path stays wait-free and scales while the synchronous path pays a syscall per line.
//
// A single background writer thread drains all rings to /dev/null so the producer cost is measured in
// isolation with a live consumer (we assert zero drops at the end).

#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "td/utils/Slice.h"
#include "td/utils/SpscRing.h"
#include "td/utils/date.h"
#include "td/utils/port/Clocks.h"
#include "td/utils/port/FileFd.h"
#include "td/utils/port/thread.h"

namespace {

constexpr size_t kMaxThreads = 16;
constexpr uint32_t kRingCapacity = 4u << 20;  // 4 MB/thread: LLC-resident (realistic), writer keeps up at sane rates

// A representative formatted log line (matches the byte shape TON emits: [lvl][tNN][ts][file:line][#tag] body).
const std::string kLine =
    "[ 3][t 5][2026-05-30 12:00:00.000000][quic-server.cpp:752][#quic] connection closed after ingress error";

// One background writer draining N per-thread rings. To isolate the PRODUCER cost we drain in bulk and do
// not write to a file here: the whole point is that the file write happens off the producer thread, and we
// want the ring kept empty so reserve() never drops (which would understate the real enqueue cost). The
// file-write throughput of the writer is a separate concern, off the producer's critical path.
class AsyncSink {
 public:
  explicit AsyncSink(size_t n_rings) {
    rings_.reserve(n_rings);
    for (size_t i = 0; i < n_rings; i++) {
      rings_.push_back(std::make_unique<td::SpscRing>(kRingCapacity));
    }
    writer_ = std::make_unique<td::thread>([this] { writer_loop(); });
    writer_->set_name("log-writer");
  }
  ~AsyncSink() {
    stop();
  }

  td::SpscRing &ring(size_t i) {
    return *rings_[i];
  }

  uint64_t total_dropped() const {
    uint64_t d = 0;
    for (auto &r : rings_) {
      d += r->dropped();
    }
    return d;
  }

  void stop() {
    if (stopped_.exchange(true)) {
      return;
    }
    stop_flag_.store(true, std::memory_order_release);
    writer_->join();
  }

 private:
  void writer_loop() {
    // Spin while data is flowing (yielding here lets the ~5 ns producers overflow the ring before we are
    // rescheduled); only yield after a long idle streak so we don't burn a core between benchmarks.
    uint32_t idle = 0;
    for (;;) {
      bool any = false;
      for (auto &r : rings_) {
        if (r->clear() != 0) {  // producer-cost bench: keep the ring drained as cheaply as possible
          any = true;
        }
      }
      if (any) {
        idle = 0;
        continue;
      }
      if (stop_flag_.load(std::memory_order_acquire)) {
        break;
      }
      if (++idle > (1u << 16)) {
        std::this_thread::yield();
      }
    }
  }

  std::vector<std::unique_ptr<td::SpscRing>> rings_;
  std::unique_ptr<td::thread> writer_;
  std::atomic<bool> stop_flag_{false};
  std::atomic<bool> stopped_{false};
};

AsyncSink *g_sink = nullptr;                     // async destination
std::vector<std::unique_ptr<td::FileFd>> g_fds;  // per-thread fds for the synchronous baseline
std::atomic<int> g_level{1};                     // mimics log_options.get_level()

td::FileFd open_null() {
  return td::FileFd::open("/dev/null", td::FileFd::Write | td::FileFd::Append).move_as_ok();
}

void BM_SyncWriteLine(benchmark::State &state) {
  auto &fd = *g_fds[state.thread_index()];
  td::Slice line(kLine);
  for (auto _ : state) {
    fd.write(line).ignore();
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_AsyncEnqueueLine(benchmark::State &state) {
  auto &ring = g_sink->ring(state.thread_index());
  td::Slice line(kLine);
  for (auto _ : state) {
    ring.push(line);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_AsyncCaptureBinary(benchmark::State &state) {
  auto &ring = g_sink->ring(state.thread_index());
  struct Rec {
    uint32_t callsite_id;
    uint8_t level;
    uint64_t tsc;
    int64_t a0;
    int64_t a1;
  };
  uint64_t i = 0;
  for (auto _ : state) {
    Rec r{42u, 3u, td::Clocks::rdtsc(), static_cast<int64_t>(i), static_cast<int64_t>(i) * 2};
    ring.push(td::Slice(reinterpret_cast<const char *>(&r), sizeof(r)));
    i++;
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_DisabledGate(benchmark::State &state) {
  int msg_level = 4;  // a VLOG(DEBUG)-style line
  for (auto _ : state) {
    int lvl = g_level.load(std::memory_order_relaxed);
    if (msg_level <= lvl) {
      benchmark::DoNotOptimize(lvl);  // would log
    }
    benchmark::DoNotOptimize(msg_level);
  }
}

// ---- Timestamp formatting: the fixed per-line cost that is independent of arg complexity ----
// All of these run on the PRODUCER thread (formatting stays parallel; the writer is pure I/O).

using SysClock = std::chrono::system_clock;

inline void put2(char *p, int v) {
  p[0] = static_cast<char>('0' + v / 10);
  p[1] = static_cast<char>('0' + v % 10);
}
inline void put6(char *p, int v) {
  for (int i = 5; i >= 0; i--) {
    p[i] = static_cast<char>('0' + v % 10);
    v /= 10;
  }
}

constexpr size_t kTsLen = 26;  // "YYYY-MM-DD HH:MM:SS.ffffff"

// Writes the to-the-second part "YYYY-MM-DD HH:MM:SS." (20 bytes) without ostringstream / locale / heap.
inline void format_ts_to_second(char *out, SysClock::time_point sec_tp) {
  using namespace date;
  auto dp = floor<days>(sec_tp);
  year_month_day ymd{dp};
  auto tod = make_time(sec_tp - dp);
  int y = static_cast<int>(ymd.year());
  out[0] = static_cast<char>('0' + (y / 1000) % 10);
  out[1] = static_cast<char>('0' + (y / 100) % 10);
  out[2] = static_cast<char>('0' + (y / 10) % 10);
  out[3] = static_cast<char>('0' + y % 10);
  out[4] = '-';
  put2(out + 5, static_cast<int>(static_cast<unsigned>(ymd.month())));
  out[7] = '-';
  put2(out + 8, static_cast<int>(static_cast<unsigned>(ymd.day())));
  out[10] = ' ';
  put2(out + 11, static_cast<int>(tod.hours().count()));
  out[13] = ':';
  put2(out + 14, static_cast<int>(tod.minutes().count()));
  out[16] = ':';
  put2(out + 17, static_cast<int>(tod.seconds().count()));
  out[19] = '.';
}

// Full "YYYY-MM-DD HH:MM:SS.ffffff" (26 bytes), byte-identical to date::format("%F %T") on a us-resolution clock.
inline void format_ts_fast(char *out, SysClock::time_point tp) {
  auto sec = std::chrono::floor<std::chrono::seconds>(tp);
  format_ts_to_second(out, sec);
  put6(out + 20, static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(tp - sec).count()));
}

// Today's path: date::format builds a std::ostringstream + locale facets + a heap-allocated std::string.
void BM_TsDateFormat(benchmark::State &state) {
  for (auto _ : state) {
    std::string s = date::format("%F %T", SysClock::now());
    benchmark::DoNotOptimize(s.data());
    benchmark::ClobberMemory();
  }
}

// Just the clock read, to separate it from the formatting cost.
void BM_TsClockNow(benchmark::State &state) {
  for (auto _ : state) {
    auto t = SysClock::now();
    benchmark::DoNotOptimize(t);
  }
}

// Hand-rolled, no allocation (full 26-char timestamp incl. microseconds).
void BM_TsFast(benchmark::State &state) {
  char buf[kTsLen];
  for (auto _ : state) {
    format_ts_fast(buf, SysClock::now());
    benchmark::DoNotOptimize(buf);
  }
}

// Thread-local cache of the to-the-second part; per line we only render the 6-digit microsecond fraction.
// Stays on the producer, thread-local -> no sharing, scales linearly, byte-identical output.
void BM_TsCachedPerSecond(benchmark::State &state) {
  static thread_local int64_t cached_sec = -1;
  static thread_local char cached[kTsLen];
  char buf[kTsLen];
  for (auto _ : state) {
    auto now = SysClock::now();
    auto sec = std::chrono::floor<std::chrono::seconds>(now);
    int64_t sec_count = sec.time_since_epoch().count();
    if (sec_count != cached_sec) {
      format_ts_to_second(cached, sec);
      cached_sec = sec_count;
    }
    std::memcpy(buf, cached, 20);
    put6(buf + 20, static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(now - sec).count()));
    benchmark::DoNotOptimize(buf);
  }
}

}  // namespace

BENCHMARK(BM_SyncWriteLine)->Threads(1)->Threads(4)->Threads(8)->UseRealTime();
BENCHMARK(BM_AsyncEnqueueLine)->Threads(1)->Threads(4)->Threads(8)->UseRealTime();
BENCHMARK(BM_AsyncCaptureBinary)->Threads(1)->Threads(4)->Threads(8)->UseRealTime();
BENCHMARK(BM_DisabledGate)->Threads(1);
// Timestamp variants, single- and multi-threaded (date::format's per-line malloc contends across threads;
// the no-alloc variants scale linearly -> formatting can stay parallel on the producers).
BENCHMARK(BM_TsClockNow)->Threads(1);
BENCHMARK(BM_TsDateFormat)->Threads(1)->Threads(8)->UseRealTime();
BENCHMARK(BM_TsFast)->Threads(1)->Threads(8)->UseRealTime();
BENCHMARK(BM_TsCachedPerSecond)->Threads(1)->Threads(8)->UseRealTime();

int main(int argc, char **argv) {
  {  // sanity: the fast formatter must match date::format byte-for-byte
    auto now = SysClock::now();
    char buf[kTsLen] = {};
    format_ts_fast(buf, now);
    std::string ref = date::format("%F %T", now);
    if (ref != std::string(buf, kTsLen)) {
      fprintf(stderr, "FATAL: fast timestamp '%.26s' != date::format '%s'\n", buf, ref.c_str());
      return 1;
    }
  }
  for (size_t i = 0; i < kMaxThreads; i++) {
    g_fds.push_back(std::make_unique<td::FileFd>(open_null()));
  }
  AsyncSink sink(kMaxThreads);
  g_sink = &sink;

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();

  sink.stop();
  fprintf(stderr, "async dropped records: %llu (expect 0)\n", (unsigned long long)sink.total_dropped());
  return 0;
}
