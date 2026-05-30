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
*/
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "td/utils/AsyncFileLog.h"
#include "td/utils/FileLog.h"
#include "td/utils/SpscRing.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/Time.h"
#include "td/utils/TimestampFormat.h"
#include "td/utils/port/thread.h"

namespace td {

namespace {
constexpr size_t kMaxBatchSize = 1u << 18;
constexpr int kMaxSpinIdle = 64;
constexpr int kMaxSleepIdle = 1024;
constexpr double kFatalFlushTimeout = 0.2;
constexpr auto kFatalFlushPollDelay = std::chrono::microseconds(100);
constexpr auto kMaxIdleSleepDelay = std::chrono::microseconds(1000);

// Distinguishes one AsyncFileLog instance from another, so a thread can recreate its cached producer
// after a log_interface swap.
std::atomic<uint64> g_logger_generation{1};
}  // namespace

struct AsyncFileLog::Impl {
  struct Producer {
    SpscRing ring;
    std::atomic<bool> closed{false};
    uint64 reported_drops{0};
    uint32 id;

    Producer(uint32 capacity, uint32 id) : ring(capacity), id(id) {
    }
  };

  struct ProducerHandle {
    uint64 generation{0};
    std::shared_ptr<Producer> producer;

    ~ProducerHandle() {
      close();
    }

    void close() {
      if (producer != nullptr) {
        producer->closed.store(true, std::memory_order_release);
        producer.reset();
      }
      generation = 0;
    }

    void set(std::shared_ptr<Producer> new_producer, uint64 new_generation) {
      close();
      producer = std::move(new_producer);
      generation = new_generation;
    }
  };
  static thread_local ProducerHandle handle_;

  FileLog file_log_;
  uint32 ring_capacity_{DEFAULT_RING_CAPACITY};
  const uint64 generation_{g_logger_generation.fetch_add(1, std::memory_order_relaxed)};

  std::mutex producers_mutex_;
  std::vector<std::shared_ptr<Producer>> producers_;
  uint32 next_producer_id_{0};

  std::mutex io_mutex_;
  td::thread writer_;
  std::atomic<bool> running_{false};
  std::atomic<bool> fatal_flush_request_{false};
  std::atomic<uint64> dropped_{0};

  std::string batch_;

  ~Impl() {
    stop();
  }

  Status init(string path, int64 rotate_threshold, bool redirect_stderr, uint32 ring_capacity) {
    if (!SpscRing::is_valid_capacity(ring_capacity)) {
      return Status::Error(PSLICE() << "Invalid async log ring capacity " << ring_capacity);
    }
    if (running_.load(std::memory_order_acquire)) {
      return Status::Error("AsyncFileLog is already running");
    }
    ring_capacity_ = ring_capacity;
    TRY_STATUS(file_log_.init(std::move(path), rotate_threshold, redirect_stderr));
    batch_.clear();
    batch_.reserve(kMaxBatchSize);
    fatal_flush_request_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    writer_ = td::thread([this] { writer_loop(); });
    writer_.set_name("log-writer");
    return Status::OK();
  }

  void stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    writer_.join();
  }

  std::shared_ptr<Producer> register_producer() {
    std::lock_guard<std::mutex> guard(producers_mutex_);
    auto producer = std::make_shared<Producer>(ring_capacity_, next_producer_id_++);
    producers_.push_back(producer);
    return producer;
  }

  Producer *get_producer() {
    if (handle_.generation != generation_) {
      auto producer = register_producer();
      handle_.set(std::move(producer), generation_);
    }
    return handle_.producer.get();
  }

  void append(CSlice slice, int log_level) {
    if (log_level == VERBOSITY_NAME(FATAL)) {
      write_fatal(slice);
    }
    get_producer()->ring.push(slice);
  }

  uint64 get_dropped_count() const {
    return dropped_.load(std::memory_order_relaxed);
  }

  bool drain_producer(Producer &producer) {
    bool any = false;
    producer.ring.pop_each([&](Slice rec) {
      batch_.append(rec.data(), rec.size());
      any = true;
    });
    uint64 dropped = producer.ring.dropped();
    if (dropped != producer.reported_drops) {
      auto delta = dropped - producer.reported_drops;
      append_drop_notice(producer.id, delta);
      dropped_.fetch_add(delta, std::memory_order_relaxed);
      producer.reported_drops = dropped;
      any = true;
    }
    return any;
  }

  void append_drop_notice(uint32 id, uint64 n) {
    char ts[TIMESTAMP_BUF_SIZE];
    auto ts_slice = format_system_clock(MutableSlice(ts, sizeof(ts)), std::chrono::system_clock::now());
    char line[160];
    StringBuilder sb(MutableSlice(line, sizeof(line)));
    sb << "[ 2][t" << id << "][" << ts_slice << "][async_log] dropped " << n << " log lines\n";
    CHECK(!sb.is_error());
    auto s = sb.as_cslice();
    batch_.append(s.data(), s.size());
  }

  bool drain_all() {
    std::vector<std::shared_ptr<Producer>> producers;
    {
      std::lock_guard<std::mutex> guard(producers_mutex_);
      producers = producers_;
    }

    bool any = false;
    bool reap = false;
    for (auto &producer : producers) {
      any |= drain_producer(*producer);
      reap |= producer->closed.load(std::memory_order_acquire) && producer->ring.empty();
      if (batch_.size() >= kMaxBatchSize) {
        flush();
      }
    }
    if (reap) {
      reap_dead();
    }
    return any;
  }

  void reap_dead() {
    std::lock_guard<std::mutex> guard(producers_mutex_);
    auto is_dead = [](const std::shared_ptr<Producer> &producer) {
      return producer->closed.load(std::memory_order_acquire) && producer->ring.empty();
    };
    producers_.erase(std::remove_if(producers_.begin(), producers_.end(), is_dead), producers_.end());
  }

  void flush() {
    if (batch_.empty()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(io_mutex_);
      file_log_.append(batch_, -1);
    }
    batch_.clear();
  }

  void writer_loop() {
    int idle = 0;
    while (running_.load(std::memory_order_acquire)) {
      bool any = drain_all();
      flush();
      any |= flush_fatal_request();
      if (any) {
        idle = 0;
      } else {
        backoff(idle++);
      }
    }
    drain_all();
    flush();
  }

  bool flush_fatal_request() {
    if (!fatal_flush_request_.load(std::memory_order_acquire)) {
      return false;
    }
    drain_all();
    flush();
    fatal_flush_request_.store(false, std::memory_order_release);
    return true;
  }

  static void backoff(int idle) {
    if (idle < kMaxSpinIdle) {
      std::this_thread::yield();
    } else {
      auto delay = idle < kMaxSleepIdle ? std::chrono::microseconds(idle - kMaxSpinIdle) : kMaxIdleSleepDelay;
      std::this_thread::sleep_for(delay);
    }
  }

  [[noreturn]] void write_fatal(CSlice slice) {
    fatal_flush_request_.store(true, std::memory_order_release);
    auto deadline = Timestamp::in(kFatalFlushTimeout);
    while (fatal_flush_request_.load(std::memory_order_acquire) && !deadline.is_in_past() &&
           running_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(kFatalFlushPollDelay);
    }
    {
      std::lock_guard<std::mutex> lock(io_mutex_);
      file_log_.append(slice, VERBOSITY_NAME(FATAL));
    }
    process_fatal_error(slice);
  }

  void rotate() {
    file_log_.lazy_rotate();
  }

  vector<string> get_file_paths() {
    return file_log_.get_file_paths();
  }
};

thread_local AsyncFileLog::Impl::ProducerHandle AsyncFileLog::Impl::handle_;

AsyncFileLog::AsyncFileLog() : impl_(std::make_unique<Impl>()) {
}
AsyncFileLog::~AsyncFileLog() = default;

Status AsyncFileLog::init(string path, int64 rotate_threshold, bool redirect_stderr, uint32 ring_capacity) {
  return impl_->init(std::move(path), rotate_threshold, redirect_stderr, ring_capacity);
}

Result<td::unique_ptr<LogInterface>> AsyncFileLog::create(string path, int64 rotate_threshold, bool redirect_stderr,
                                                          uint32 ring_capacity) {
  auto res = td::make_unique<AsyncFileLog>();
  TRY_STATUS(res->init(std::move(path), rotate_threshold, redirect_stderr, ring_capacity));
  return std::move(res);
}

void AsyncFileLog::append(CSlice slice, int log_level) {
  impl_->append(slice, log_level);
}
void AsyncFileLog::rotate() {
  impl_->rotate();
}
vector<string> AsyncFileLog::get_file_paths() {
  return impl_->get_file_paths();
}
uint64 AsyncFileLog::get_dropped_count() const {
  return impl_->get_dropped_count();
}

}  // namespace td
