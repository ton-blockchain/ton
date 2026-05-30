#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "td/utils/AsyncFileLog.h"
#include "td/utils/OptionParser.h"
#include "td/utils/SpscRing.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/path.h"

namespace {

using Clock = std::chrono::steady_clock;

const std::string kLine =
    "[ 3][t 5][2026-05-30 12:00:00.000000][quic-server.cpp:752][#quic] connection closed after ingress error\n";

struct Options {
  int threads = 4;
  double seconds = 10.0;
  double wait_seconds = 1.0;
  double start_rate = 1000000;
  double max_rate = 256000000;
  double rate = 0;
  int search_steps = 12;
  int samples = 1;
  uint32_t ring_capacity = td::AsyncFileLog::DEFAULT_RING_CAPACITY;
  std::string dir = td::get_temporary_dir().str();
  bool keep_files = false;
};

struct TrialResult {
  double target_rate = 0;
  double actual_rate = 0;
  td::uint64 attempted = 0;
  td::uint64 dropped = 0;
  std::string path;
};

struct ProbeResult {
  double target_rate = 0;
  td::uint64 attempted = 0;
  td::uint64 dropped = 0;
  double min_actual_rate = std::numeric_limits<double>::max();
  double max_actual_rate = 0;
};

td::Status parse_positive_double(td::Slice arg, td::Slice name, double &value) {
  value = td::to_double(arg);
  if (value <= 0) {
    return td::Status::Error(PSLICE() << name << " should be positive");
  }
  return td::Status::OK();
}

td::Status parse_non_negative_double(td::Slice arg, td::Slice name, double &value) {
  value = td::to_double(arg);
  if (value < 0) {
    return td::Status::Error(PSLICE() << name << " should be non-negative");
  }
  return td::Status::OK();
}

td::Status parse_positive_int(td::Slice arg, td::Slice name, int &value) {
  TRY_RESULT(parsed, td::to_integer_safe<int>(arg));
  if (parsed <= 0) {
    return td::Status::Error(PSLICE() << name << " should be positive");
  }
  value = parsed;
  return td::Status::OK();
}

td::Status parse_ring_capacity(td::Slice arg, uint32_t &value) {
  TRY_RESULT(parsed, td::to_integer_safe<uint32_t>(arg));
  if (!td::SpscRing::is_valid_capacity(parsed)) {
    return td::Status::Error(PSLICE() << "ring capacity should be a power of two in [8, 1 << 30], got " << parsed);
  }
  value = parsed;
  return td::Status::OK();
}

void print_help(const td::OptionParser &parser) {
  char buf[8192];
  td::StringBuilder sb(td::MutableSlice(buf, sizeof(buf)));
  sb << parser;
  std::printf("%s", sb.as_cslice().c_str());
}

Options parse_options(int argc, char **argv) {
  Options options;
  td::OptionParser parser;
  parser.set_description("find AsyncFileLog no-drop rate with fresh logger instances");

  parser.add_option('h', "help", "prints help", [&] {
    print_help(parser);
    std::exit(0);
  });
  parser.add_checked_option('t', "threads", PSTRING() << "producer threads (default=" << options.threads << ")",
                            [&](td::Slice arg) { return parse_positive_int(arg, "--threads", options.threads); });
  parser.add_checked_option('s', "seconds",
                            PSTRING() << "seconds per fresh-log trial (default=" << options.seconds << ")",
                            [&](td::Slice arg) { return parse_positive_double(arg, "--seconds", options.seconds); });
  parser.add_checked_option(
      '\0', "wait",
      PSTRING() << "seconds to wait for drop reports after producers finish (default=" << options.wait_seconds << ")",
      [&](td::Slice arg) { return parse_non_negative_double(arg, "--wait", options.wait_seconds); });
  parser.add_checked_option('\0', "rate", "run one fixed rate instead of binary search",
                            [&](td::Slice arg) { return parse_positive_double(arg, "--rate", options.rate); });
  parser.add_checked_option(
      '\0', "start-rate",
      PSTRING() << "first binary-search bracket rate in lines/sec (default="
                << static_cast<td::uint64>(options.start_rate) << ")",
      [&](td::Slice arg) { return parse_positive_double(arg, "--start-rate", options.start_rate); });
  parser.add_checked_option(
      '\0', "max-rate",
      PSTRING() << "highest bracket rate in lines/sec (default=" << static_cast<td::uint64>(options.max_rate) << ")",
      [&](td::Slice arg) { return parse_positive_double(arg, "--max-rate", options.max_rate); });
  parser.add_checked_option(
      '\0', "steps", PSTRING() << "binary-search steps after bracketing (default=" << options.search_steps << ")",
      [&](td::Slice arg) { return parse_positive_int(arg, "--steps", options.search_steps); });
  parser.add_checked_option(
      '\0', "samples",
      PSTRING() << "fresh-log trials per tested rate; any drop makes rate unsafe (default=" << options.samples << ")",
      [&](td::Slice arg) { return parse_positive_int(arg, "--samples", options.samples); });
  parser.add_checked_option('\0', "ring-capacity",
                            PSTRING() << "bytes per producer ring (default=" << options.ring_capacity << ")",
                            [&](td::Slice arg) { return parse_ring_capacity(arg, options.ring_capacity); });
  parser.add_option('\0', "dir", PSTRING() << "directory for benchmark logs (default=" << options.dir << ")",
                    [&](td::Slice arg) { options.dir = arg.str(); });
  parser.add_option('\0', "keep-files", "keep generated log files", [&] { options.keep_files = true; });
  parser.add_check([&] {
    if (options.start_rate > options.max_rate) {
      return td::Status::Error("--start-rate should be <= --max-rate");
    }
    return td::Status::OK();
  });

  auto status = parser.run(argc, argv, 0);
  if (status.is_error()) {
    auto error = status.move_as_error();
    std::fprintf(stderr, "failed to parse options: %s\n\n", error.message().str().c_str());
    print_help(parser);
    std::exit(2);
  }
  return options;
}

std::string make_path(const Options &options, int trial_id) {
  auto dir = options.dir;
  if (!dir.empty() && dir.back() == TD_DIR_SLASH) {
    dir.pop_back();
  }
  return dir + TD_DIR_SLASH + "async-log-rate-" + std::to_string(static_cast<long long>(getpid())) + "-" +
         std::to_string(trial_id) + ".log";
}

void wait_after_trial(const Options &options, td::AsyncFileLog &log) {
  auto deadline =
      Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(options.wait_seconds));
  auto dropped = log.get_dropped_count();
  while (Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto next_dropped = log.get_dropped_count();
    if (next_dropped != 0 && next_dropped == dropped) {
      break;
    }
    dropped = next_dropped;
  }
}

TrialResult run_trial(const Options &options, double target_rate, int trial_id) {
  auto path = make_path(options, trial_id);
  td::unlink(path).ignore();

  std::atomic<td::uint64> attempted{0};
  auto start = Clock::now() + std::chrono::milliseconds(100);
  auto stop = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(options.seconds));
  auto producer_done = stop;
  td::uint64 dropped = 0;

  {
    td::AsyncFileLog log;
    log.init(path, std::numeric_limits<td::int64>::max(), /*redirect_stderr=*/false, options.ring_capacity).ensure();

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(options.threads));
    for (int i = 0; i < options.threads; i++) {
      threads.emplace_back([&, per_thread_rate = target_rate / options.threads] {
        td::uint64 local_attempted = 0;
        while (Clock::now() < start) {
          std::this_thread::yield();
        }

        while (true) {
          auto target_time = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
                                         static_cast<double>(local_attempted) / per_thread_rate));
          auto now = Clock::now();
          if (now >= stop) {
            break;
          }
          if (now < target_time) {
            auto remaining = target_time - now;
            if (remaining > std::chrono::microseconds(100)) {
              std::this_thread::sleep_for(remaining / 2);
            }
            while (Clock::now() < target_time) {
            }
          }
          log.append(kLine, VERBOSITY_NAME(INFO));
          local_attempted++;
        }
        attempted.fetch_add(local_attempted, std::memory_order_relaxed);
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }
    producer_done = Clock::now();
    wait_after_trial(options, log);
    dropped = log.get_dropped_count();
  }

  if (!options.keep_files) {
    td::unlink(path).ignore();
    path.clear();
  }

  auto producer_seconds = std::chrono::duration<double>(producer_done - start).count();
  auto total_attempted = attempted.load(std::memory_order_relaxed);
  return {target_rate, total_attempted / producer_seconds, total_attempted, dropped, std::move(path)};
}

void print_trial(const char *label, int sample, int samples, const TrialResult &result) {
  auto mib_per_second = result.actual_rate * kLine.size() / 1024 / 1024;
  std::printf("%-8s %2d/%-2d target=%8.3fM/s actual=%8.3fM/s bytes=%8.1fMiB/s attempted=%10llu dropped=%llu\n", label,
              sample, samples, result.target_rate / 1e6, result.actual_rate / 1e6, mib_per_second,
              static_cast<unsigned long long>(result.attempted), static_cast<unsigned long long>(result.dropped));
  if (result.path.empty()) {
    std::fflush(stdout);
    return;
  }
  std::printf("            path=%s\n", result.path.c_str());
  std::fflush(stdout);
}

ProbeResult probe_rate(const Options &options, const char *label, double target_rate, int &trial_id) {
  ProbeResult probe;
  probe.target_rate = target_rate;
  for (int i = 0; i < options.samples; i++) {
    auto trial = run_trial(options, target_rate, trial_id++);
    print_trial(label, i + 1, options.samples, trial);
    probe.attempted += trial.attempted;
    probe.dropped += trial.dropped;
    probe.min_actual_rate = std::min(probe.min_actual_rate, trial.actual_rate);
    probe.max_actual_rate = std::max(probe.max_actual_rate, trial.actual_rate);
  }
  return probe;
}

bool is_safe(const ProbeResult &probe) {
  return probe.dropped == 0;
}

void print_summary(const Options &options, double low, double high) {
  auto low_mib = low * kLine.size() / 1024 / 1024;
  auto high_mib = high * kLine.size() / 1024 / 1024;
  std::printf("safe target ~= %.3fM/s (%.1fMiB/s), first unsafe target ~= %.3fM/s (%.1fMiB/s)\n", low / 1e6, low_mib,
              high / 1e6, high_mib);
  std::printf("classification: safe iff all %d fresh-log sample(s) had zero reported drops\n", options.samples);
}

}  // namespace

int main(int argc, char **argv) {
  auto options = parse_options(argc, argv);
  std::printf("line_size=%zu threads=%d seconds=%.3f wait=%.3f samples=%d ring_capacity=%u dir=%s keep_files=%s\n",
              kLine.size(), options.threads, options.seconds, options.wait_seconds, options.samples,
              options.ring_capacity, options.dir.c_str(), options.keep_files ? "true" : "false");

  int trial_id = 0;
  if (options.rate > 0) {
    auto probe = probe_rate(options, "rate", options.rate, trial_id);
    return is_safe(probe) ? 0 : 1;
  }

  double low = 0;
  double high = options.start_rate;
  while (true) {
    auto rate = std::min(high, options.max_rate);
    auto probe = probe_rate(options, "bracket", rate, trial_id);
    if (!is_safe(probe)) {
      high = rate;
      break;
    }
    low = rate;
    if (rate == options.max_rate) {
      std::printf("No drops found up to %.3fM/s\n", options.max_rate / 1e6);
      return 0;
    }
    high = std::min(high * 2, options.max_rate);
  }

  for (int i = 0; i < options.search_steps; i++) {
    auto mid = (low + high) / 2;
    auto probe = probe_rate(options, "search", mid, trial_id);
    if (is_safe(probe)) {
      low = mid;
    } else {
      high = mid;
    }
  }

  print_summary(options, low, high);
  return 0;
}
