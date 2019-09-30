#pragma once

#include "td/utils/int_types.h"
#include "td/actor/actor.h"

namespace ton {

namespace validator {

class StatsMerger : public td::actor::Actor {
 public:
  StatsMerger(td::Promise<std::vector<std::pair<std::string, std::string>>> promise)

      : promise_(std::move(promise)) {
  }
  void start_up() override {
    if (!pending_) {
      finish();
    }
  }

  void finish_subjob(td::Result<std::vector<std::pair<std::string, std::string>>> R, std::string prefix) {
    if (R.is_ok()) {
      auto v = R.move_as_ok();
      for (auto &el : v) {
        cur_.emplace_back(prefix + el.first, std::move(el.second));
      }
    }
    if (--pending_ == 0) {
      finish();
    }
  }
  void inc() {
    ++pending_;
  }
  void dec() {
    if (--pending_ == 0) {
      finish();
    }
  }
  void finish() {
    promise_.set_value(std::move(cur_));
    stop();
  }

  struct InitGuard {
    td::actor::ActorId<StatsMerger> merger;
    ~InitGuard() {
      td::actor::send_closure(merger, &StatsMerger::dec);
    }
    auto make_promise(std::string prefix) {
      merger.get_actor_unsafe().inc();
      return td::PromiseCreator::lambda(
          [merger = merger, prefix](td::Result<std::vector<std::pair<std::string, std::string>>> R) {
            td::actor::send_closure(merger, &StatsMerger::finish_subjob, std::move(R), std::move(prefix));
          });
    }
  };

  static InitGuard create(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) {
    InitGuard ig;
    ig.merger = td::actor::create_actor<StatsMerger>("m", std::move(promise)).release();
    return ig;
  }

 private:
  std::vector<std::pair<std::string, std::string>> cur_;
  std::atomic<td::uint32> pending_{1};
  td::Promise<std::vector<std::pair<std::string, std::string>>> promise_;
};

}  // namespace validator

}  // namespace ton
