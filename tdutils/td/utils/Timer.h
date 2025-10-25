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
#pragma once

#include "td/utils/StringBuilder.h"
#include "td/utils/Status.h"

#include <functional>

namespace td {

class Timer {
 public:
  Timer() : Timer(false) {
  }
  explicit Timer(bool is_paused);
  Timer(const Timer &other) = default;
  Timer &operator=(const Timer &other) = default;

  double elapsed() const;
  void pause();
  void resume();

 private:
  friend StringBuilder &operator<<(StringBuilder &string_builder, const Timer &timer);

  double elapsed_{0};
  double start_time_;
  bool is_paused_{false};
};

class PerfWarningTimer {
 public:
  explicit PerfWarningTimer(string name, double max_duration = 0.1, std::function<void(double)> &&callback = {});
  PerfWarningTimer(const PerfWarningTimer &) = delete;
  PerfWarningTimer &operator=(const PerfWarningTimer &) = delete;
  PerfWarningTimer(PerfWarningTimer &&other);
  PerfWarningTimer &operator=(PerfWarningTimer &&) = delete;
  ~PerfWarningTimer();
  void reset();
  double elapsed() const;

 private:
  string name_;
  double start_at_{0};
  double max_duration_{0};
  std::function<void(double)> callback_;
};

class ThreadCpuTimer {
 public:
  ThreadCpuTimer() : ThreadCpuTimer(false) {
  }
  explicit ThreadCpuTimer(bool is_paused);
  ThreadCpuTimer(const ThreadCpuTimer &other) = default;
  ThreadCpuTimer &operator=(const ThreadCpuTimer &other) = default;

  double elapsed() const;
  void pause();
  void resume();

 private:
  double elapsed_{0};
  double start_time_;
  bool is_paused_{false};
};

class PerfLog;
struct EmptyDeleter {
  template <class T>
  void operator()(T *) {
  }
};
class PerfLogAction {
 public:
  template <class T>
  double finish(const T &result);
  size_t i_{0};
  std::unique_ptr<PerfLog, EmptyDeleter> perf_log_;
};

class PerfLog {
 public:
  PerfLogAction start_action(std::string name);
  friend td::StringBuilder &operator<<(td::StringBuilder &sb, const PerfLog &log);

 private:
  struct Entry {
    std::string name;
    double begin{};
    double end{};
    td::Status status;
  };
  std::vector<Entry> entries_;
  friend class PerfLogAction;

  double finish_action(size_t i, td::Status status);
};
template <class T>
double PerfLogAction::finish(const T &result) {
  if (result.is_ok()) {
    return perf_log_->finish_action(i_, td::Status::OK());
  } else {
    return perf_log_->finish_action(i_, result.error().clone());
  }
}

}  // namespace td
