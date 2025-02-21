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
#pragma once

#include "validator.h"
#include "common/AtomicRef.h"

#include <atomic>

namespace ton {

namespace validator {

class StatsProvider {
 public:
  StatsProvider() = default;
  StatsProvider(td::actor::ActorId<ValidatorManagerInterface> manager, std::string prefix,
                std::function<void(td::Promise<std::vector<std::pair<std::string, std::string>>>)> callback)
      : inited_(true), manager_(std::move(manager)) {
    static std::atomic<td::uint64> cur_idx{0};
    idx_ = cur_idx.fetch_add(1);
    td::actor::send_closure(manager_, &ValidatorManagerInterface::register_stats_provider, idx_, std::move(prefix),
                            std::move(callback));
  }
  StatsProvider(const StatsProvider&) = delete;
  StatsProvider(StatsProvider&& other) noexcept
      : inited_(other.inited_), idx_(other.idx_), manager_(std::move(other.manager_)) {
    other.inited_ = false;
  }
  ~StatsProvider() {
    if (inited_) {
      td::actor::send_closure(manager_, &ValidatorManagerInterface::unregister_stats_provider, idx_);
    }
  }

  StatsProvider& operator=(const StatsProvider&) = delete;
  StatsProvider& operator=(StatsProvider&& other) noexcept {
    if (this != &other) {
      inited_ = other.inited_;
      idx_ = other.idx_;
      manager_ = std::move(other.manager_);
      other.inited_ = false;
    }
    return *this;
  }

  bool inited() const {
    return inited_;
  }

 private:
  bool inited_ = false;
  td::uint64 idx_ = 0;
  td::actor::ActorId<ValidatorManagerInterface> manager_;
};

class ProcessStatus {
 public:
  ProcessStatus() = default;
  ProcessStatus(td::actor::ActorId<ValidatorManagerInterface> manager, std::string name)
      : stats_provider_(std::move(manager), std::move(name), [value = value_](auto promise) {
        auto status = value->load();
        if (status.is_null()) {
          promise.set_error(td::Status::Error("empty"));
          return;
        }
        std::vector<std::pair<std::string, std::string>> vec;
        vec.emplace_back("", *status);
        promise.set_value(std::move(vec));
      }) {
  }
  ProcessStatus(const ProcessStatus&) = delete;
  ProcessStatus(ProcessStatus&& other) noexcept = default;
  ProcessStatus& operator=(const ProcessStatus&) = delete;
  ProcessStatus& operator=(ProcessStatus&& other) noexcept = default;

  void set_status(std::string s) {
    if (!value_) {
      return;
    }
    value_->store(td::Ref<td::Cnt<std::string>>(true, std::move(s)));
  }

 private:
  std::shared_ptr<td::AtomicRef<td::Cnt<std::string>>> value_ = std::make_shared<td::AtomicRef<td::Cnt<std::string>>>();
  StatsProvider stats_provider_;
};

}  // namespace validator

}  // namespace ton
