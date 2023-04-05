//
// Created by Andrey Tvorozhkov (andrey@head-labs.com) on 4/5/23.
//

#include "td/utils/OptionParser.h"
#include "validator/db/archive-manager.hpp"

#include "td/utils/port/signals.h"
#include <memory>
#include <numeric>
#include "td/utils/logging.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "validator/validator.h"
#include "validator/manager-disk.h"
#include "ton/ton-types.h"
#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include <utility>
#include "auto/tl/lite_api.h"
#include "adnl/utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "tuple"

namespace ton {

namespace validator {
class TestEngine : public td::actor::Actor {
 public:
  TestEngine(bool is_masterchain_test_, td::uint32 seqno_first_, td::uint32 seqno_last_, bool with_index_ = false,
             bool async_ = false) {
    is_masterchain_test = is_masterchain_test_;
    seqno_first = seqno_first_;
    seqno_last = seqno_last_;
    with_index = with_index_;
    async = async_;
  };

  void run(td::Promise<td::uint32> promise_, const td::actor::ActorId<ArchiveManager> &archive_manager_) {
    ts = td::Time::now();
    LOG(WARNING) << "Start test: " << get_name() << " Start from: " << seqno_first << " End at: " << seqno_last
                 << " Is masterchain: " << is_masterchain_test;

    archive_manager = archive_manager_;

    promise = std::move(promise_);
    AccountIdPrefixFull pfx;

    if (is_masterchain_test) {
      pfx = AccountIdPrefixFull{masterchainId, 0};
    } else {
      pfx = AccountIdPrefixFull{0, 0};
    }

    const unsigned int to_load = seqno_last - seqno_first;

    for (unsigned int i = 0; i < to_load; i++) {
      auto P = td::PromiseCreator::lambda([me = actor_id(this)](td::Result<ConstBlockHandle> R) {
        if (R.is_error()) {
          LOG(ERROR) << R.move_as_error();
          return;
        }

        auto ok = R.move_as_ok();
        auto seqno = ok.get()->id().seqno();
        td::actor::send_closure(me, &TestEngine::done_part, seqno);
      });

      by_block_ts[seqno_first + i] = td::Time::now();

      td::actor::send_closure(archive_manager, &ArchiveManager::get_block_by_seqno_custom, pfx, seqno_first + i,
                              std::move(P), with_index, async);
    }
  }

  void done_part(BlockSeqno s) {
    auto it = by_block_ts.find(s);
    if (it == by_block_ts.end()) {
      throw std::invalid_argument("?");
    }
    auto t = it->second;
    parts_done_at.push_back(td::Time::now() - t);

    if (parts_done_at.size() == (seqno_last - seqno_first)) {
      const auto end_at = td::Time::now() - ts;
      LOG(WARNING) << "Test " << get_name() << " done, results: ";

      const auto avg =
          std::accumulate(parts_done_at.begin(), parts_done_at.end(), 0.0) / static_cast<double>(parts_done_at.size());

      LOG(WARNING) << "AVG ON 1 request: " << avg;
      LOG(WARNING) << "Done at: " << end_at;
      promise.set_value(0);
      stop();
    }
  }

 private:
  td::actor::ActorId<ArchiveManager> archive_manager;
  bool is_masterchain_test;
  td::uint32 seqno_first;
  td::uint32 seqno_last;
  double ts;
  std::map<BlockSeqno, double> by_block_ts;
  td::vector<double> parts_done_at;
  td::Promise<td::uint32> promise;
  bool with_index = false;
  bool async = false;
};

class TestEngineVisor : public td::actor::Actor {
 public:
  TestEngineVisor(td::unique_ptr<td::vector<td::actor::ActorId<TestEngine>>> tests_) {
    tests = std::move(tests_);
  };

  void run(const std::string &db_root, const std::string &global_config) {
    auto dummy = td::actor::ActorId<ton::validator::RootDb>();
    archive_manager = td::actor::create_actor<ton::validator::ArchiveManager>("archive", dummy, db_root);

    auto P = td::PromiseCreator::lambda([me = actor_id(this)](td::Result<td::uint32> R) {
      if (R.is_error()) {
        LOG(ERROR) << R.move_as_error();
        return;
      }

      td::actor::send_closure(me, &TestEngineVisor::done_part, 0);
    });

    LOG(DEBUG) << "Start tests";
    td::actor::send_closure(tests->at(current_test), &TestEngine::run, std::move(P), archive_manager.get());
  }

  void read_complete(td::uint32 a) {
    auto P = td::PromiseCreator::lambda([me = actor_id(this)](td::Result<td::uint32> R) {
      if (R.is_error()) {
        LOG(ERROR) << R.move_as_error();
        return;
      }

      td::actor::send_closure(me, &TestEngineVisor::done_part, 0);
    });

    LOG(DEBUG) << "Start tests";
    td::actor::send_closure(tests->at(current_test), &TestEngine::run, std::move(P), archive_manager.get());
  }

  void done_part(td::uint32 a) {
    if (current_test < tests->size()) {
      current_test++;

      if (current_test == tests->size()) {
        std::exit(0);
      }

      auto P = td::PromiseCreator::lambda([me = actor_id(this)](td::Result<td::uint32> R) {
        CHECK(R.is_ok());
        td::actor::send_closure(me, &TestEngineVisor::done_part, 0);
      });
      td::actor::send_closure(tests->at(current_test), &TestEngine::run, std::move(P), archive_manager.get());
    } else {
      std::exit(0);
    }
  }

 private:
  td::unique_ptr<td::vector<td::actor::ActorId<TestEngine>>> tests;
  td::uint32 current_test = 0;
  td::actor::ActorOwn<ton::validator::ArchiveManager> archive_manager;
};

}  // namespace validator
}  // namespace ton
int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);

  std::string db_root_;
  std::string global_config_;
  td::uint32 t;

  td::OptionParser p;
  p.set_description("test archive db methods");
  p.add_option('d', "db", "set database parse", [&db_root_](td::Slice arg) { db_root_ = arg.str(); });
  p.add_option('c', "config", "set global config", [&global_config_](td::Slice arg) { global_config_ = arg.str(); });
  p.add_option('t', "threads", "set threads", [&t](td::Slice arg) { t = td::to_integer<td::uint32>(arg); });
  p.run(argc, argv).ensure();

  td::actor::Scheduler scheduler({t});
  td::actor::ActorOwn<ton::validator::TestEngineVisor> test_visor;
  td::vector<td::actor::ActorId<ton::validator::TestEngine>> tests_actors;
  //  archive_db_ = td::actor::create_actor<ArchiveManager>("archive", actor_id(this), root_path_);

  scheduler.run_in_context([&] {
    LOG(DEBUG) << "Start testing of get_blocks of archive_db; DB_ROOT: " << db_root_ << " Threads: " << t;
    auto dummy = td::actor::ActorId<ton::validator::RootDb>();

    tests_actors.push_back(td::actor::create_actor<ton::validator::TestEngine>("Dummy test1", true, 2, 10).release());
    tests_actors.push_back(
        td::actor::create_actor<ton::validator::TestEngine>("Dummy test2", false, 1000, 10000).release());

    tests_actors.push_back(
        td::actor::create_actor<ton::validator::TestEngine>("Pure TestMC #1", true, 3600000, 3610000).release());  // mc
    tests_actors.push_back(
        td::actor::create_actor<ton::validator::TestEngine>("Pure TestWC #2", false, 3600000, 3610000)
            .release());  // wc

    // With index
    tests_actors.push_back(
        td::actor::create_actor<ton::validator::TestEngine>("Index TestMC #1", true, 3600000, 3610000, true)
            .release());  // mc
    tests_actors.push_back(
        td::actor::create_actor<ton::validator::TestEngine>("Index TestWC #2", false, 3600000, 3610000, true)
            .release());  // wc

    // With async
    tests_actors.push_back(
        td::actor::create_actor<ton::validator::TestEngine>("Async TestMC #1", true, 3600000, 3610000, true, true)
            .release());  // mc
    tests_actors.push_back(
        td::actor::create_actor<ton::validator::TestEngine>("Async TestWC #2", false, 3600000, 3610000, true, true)
            .release());  // wc

    auto tests_actors_ptr = td::make_unique<td::vector<td::actor::ActorId<ton::validator::TestEngine>>>(tests_actors);

    test_visor = td::actor::create_actor<ton::validator::TestEngineVisor>("tests_visor", std::move(tests_actors_ptr));
  });

  scheduler.run_in_context([&] {
    td::actor::send_closure(test_visor.get(), &ton::validator::TestEngineVisor::run, std::move(db_root_),
                            std::move(global_config_));
  });

  scheduler.run();
}
