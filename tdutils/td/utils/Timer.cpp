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
#include "td/utils/Timer.h"

#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/Time.h"

#include <numeric>
#include <algorithm>

namespace td {

Timer::Timer(bool is_paused) : is_paused_(is_paused) {
  if (is_paused_) {
    start_time_ = 0;
  } else {
    start_time_ = Time::now();
  }
}

void Timer::pause() {
  if (is_paused_) {
    return;
  }
  elapsed_ += Time::now() - start_time_;
  is_paused_ = true;
}

void Timer::resume() {
  if (!is_paused_) {
    return;
  }
  start_time_ = Time::now();
  is_paused_ = false;
}

double Timer::elapsed() const {
  double res = elapsed_;
  if (!is_paused_) {
    res += Time::now() - start_time_;
  }
  return res;
}

StringBuilder &operator<<(StringBuilder &string_builder, const Timer &timer) {
  return string_builder << format::as_time(timer.elapsed());
}

PerfWarningTimer::PerfWarningTimer(string name, double max_duration, std::function<void(double)> &&callback)
    : name_(std::move(name)), start_at_(Time::now()), max_duration_(max_duration), callback_(std::move(callback)) {
}

PerfWarningTimer::PerfWarningTimer(PerfWarningTimer &&other)
    : name_(std::move(other.name_))
    , start_at_(other.start_at_)
    , max_duration_(other.max_duration_)
    , callback_(std::move(other.callback_)) {
  other.start_at_ = 0;
}

PerfWarningTimer::~PerfWarningTimer() {
  reset();
}

void PerfWarningTimer::reset() {
  if (start_at_ == 0) {
    return;
  }
  double duration = Time::now() - start_at_;
  if (callback_) {
    callback_(duration);
  } else {
    LOG_IF(WARNING, duration > max_duration_)
        << "SLOW: " << tag("name", name_) << tag("duration", format::as_time(duration));
  }
  start_at_ = 0;
}

double PerfWarningTimer::elapsed() const {
  return Time::now() - start_at_;
}

static double thread_cpu_clock() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
  timespec ts;
  int result = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  CHECK(result == 0);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#else
  return 0.0;  // TODO: MacOS and Windows support (currently cpu timer is used only in validators)
#endif
}

ThreadCpuTimer::ThreadCpuTimer(bool is_paused) : is_paused_(is_paused) {
  if (is_paused_) {
    start_time_ = 0;
  } else {
    start_time_ = thread_cpu_clock();
  }
}

void ThreadCpuTimer::pause() {
  if (is_paused_) {
    return;
  }
  elapsed_ += thread_cpu_clock() - start_time_;
  is_paused_ = true;
}

void ThreadCpuTimer::resume() {
  if (!is_paused_) {
    return;
  }
  start_time_ = thread_cpu_clock();
  is_paused_ = false;
}

double ThreadCpuTimer::elapsed() const {
  double res = elapsed_;
  if (!is_paused_) {
    res += thread_cpu_clock() - start_time_;
  }
  return res;
}

PerfLogAction PerfLog::start_action(std::string name) {
  auto i = entries_.size();
  entries_.push_back({.name = std::move(name), .begin = td::Timestamp::now().at()});
  return PerfLogAction{i, std::unique_ptr<PerfLog, EmptyDeleter>(this)};
}
td::StringBuilder &operator<<(StringBuilder &sb, const PerfLog &log) {
  sb << "{";
  std::vector<size_t> ids(log.entries_.size());
  std::iota(ids.begin(), ids.end(), 0);
  std::sort(ids.begin(), ids.end(), [&](auto a, auto b) {
    return log.entries_[a].end - log.entries_[a].begin > log.entries_[b].end - log.entries_[b].begin;
  });
  sb << "{";
  for (size_t i = 0; i < log.entries_.size(); i++) {
    sb << "\n\t";
    auto &entry = log.entries_[ids[i]];
    sb << "{" << entry.name << ":" << entry.begin << "->" << entry.end << "(" << entry.end - entry.begin << ")"
       << td::format::cond(entry.status.is_error(), entry.status, "") << "}";
  }
  sb << "\n}";
  return sb;
}

double PerfLog::finish_action(size_t i, td::Status status) {
  auto &entry = entries_[i];
  CHECK(entry.end == 0);
  entry.end = td::Timestamp::now().at();
  entry.status = std::move(status);
  return entry.end - entry.begin;
}
}  // namespace td
