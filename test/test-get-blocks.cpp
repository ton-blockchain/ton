//
// Created by Andrey Tvorozhkov (andrey@head-labs.com) on 4/5/23.
//

#include "td/utils/OptionParser.h"
#include "validator/db/archive-manager.hpp"

#include "td/utils/port/signals.h"
#include "td/utils/Random.h"

#include <memory>
#include <numeric>

namespace ton {

namespace validator {

class TestEngine : public td::actor::Actor {
 public:
  TestEngine(td::actor::ActorId<ArchiveManager> archive_manager_, bool is_masterchain_test_, td::uint32 seqno_first_,
             td::uint32 seqno_last_) {
    archive_manager = std::move(archive_manager_);
    is_masterchain_test = is_masterchain_test_;
    seqno_first = seqno_first_;
    seqno_last = seqno_last_;
  };

  void start_up() override {
    AccountIdPrefixFull pfx;

    if (is_masterchain_test) {
      pfx = AccountIdPrefixFull{masterchainId, 0};
    } else {
      pfx = AccountIdPrefixFull{0, 0};
    }

    const unsigned int to_load = seqno_last - seqno_first;
    timer = td::Timer();
    for (unsigned int i = 0; i < to_load; i++) {
      auto P = td::PromiseCreator::lambda([me = actor_id(this)](td::Result<ConstBlockHandle> R) {
        CHECK(R.is_ok());
        td::actor::send_closure(me, &TestEngine::done_part);
      });
      td::actor::send_closure(archive_manager, &ArchiveManager::get_block_by_seqno, pfx, seqno_first + i, std::move(P));
    }
  }

  void done_part() {
    parts_done_at.push_back(timer.elapsed());

    if (parts_done_at.size() == (seqno_last - seqno_first)) {
      const auto end_at = timer.elapsed();
      LOG(WARNING) << "Test " << get_name() << " done, results: ";

      auto const count = static_cast<double>(parts_done_at.size());
      const auto avg = std::accumulate(parts_done_at.begin(), parts_done_at.end(), 0.0) / count;

      LOG(WARNING) << "AVG ON 1 request: " << avg;
      LOG(WARNING) << "Done at: " << end_at;
      stop();
    }
  }

 private:
  td::actor::ActorId<ArchiveManager> archive_manager;
  bool is_masterchain_test;
  td::uint32 seqno_first;
  td::uint32 seqno_last;
  td::Timer timer;
  td::vector<double> parts_done_at;
};

}  // namespace validator
}  // namespace ton
int main() {
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);

  std::string db_root_;
  td::uint32 t;

  td::OptionParser p;
  p.set_description("test archive db methods");
  p.add_option('d', "db", "set database parse", [&](td::Slice arg) { db_root_ = arg.str(); });
  p.add_option('t', "threads", "set threads", [&](td::Slice arg) { t = td::to_integer<td::uint32>(arg); });

  td::actor::Scheduler scheduler({t});

  scheduler.run_in_context([&scheduler, db_root_] {
    LOG(DEBUG) << "Start testing of get_blocks of archive_db;";
    auto dummy = td::actor::ActorId<ton::validator::RootDb>();
    auto archive_db = td::actor::create_actor<ton::validator::ArchiveManager>("archive", dummy, db_root_);
    archive_db.release();

    LOG(DEBUG) << "Start testing of get_blocks of archive_db;";
    td::actor::create_actor<ton::validator::TestEngine>("TestEngine #1", archive_db.get(), true, 0, 1000000).release();
  });

  scheduler.run();
}
