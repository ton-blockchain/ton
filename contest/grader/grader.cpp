#include "td/utils/OptionParser.h"
#include <fstream>
#include "overlay/overlays.h"

#include "adnl/adnl-ext-client.h"

#include <algorithm>
#include <list>
#include "td/utils/JsonBuilder.h"
#include "mc-config.h"
#include "td/utils/filesystem.h"
#include "td/utils/port/path.h"
#include "terminal/terminal.h"
#include "vm/cells/MerkleProof.h"
#include "ton/ton-tl.hpp"
#include "block-auto.h"
#include "contest/solution/solution.hpp"
#include "td/utils/PathView.h"
#include "td/utils/port/signals.h"
#include "vm/vm.h"
#include "vm/cells/MerkleUpdate.h"

#include <sys/resource.h>

using namespace ton;

static constexpr td::uint64 CPU_USAGE_PER_SEC = 1000000;

static td::uint64 get_cpu_usage() {
  rusage usage;
  CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
  return (td::uint64)usage.ru_utime.tv_sec * 1000000 + (td::uint64)usage.ru_utime.tv_usec;
}

class ContestGrader : public td::actor::Actor {
 public:
  explicit ContestGrader(std::string tests_dir) : tests_dir_(tests_dir) {
  }

  void start_up() override {
    vm::init_vm().ensure();
    scan_tests_dir();
    run_next_test();
  }

  void scan_tests_dir() {
    auto walk_status = td::WalkPath::run(tests_dir_, [&](td::CSlice name, td::WalkPath::Type type) {
      if (type == td::WalkPath::Type::NotDir && td::ends_with(name, ".bin")) {
        test_files_.push_back(td::PathView::relative(name.str(), tests_dir_).str());
      }
      return td::WalkPath::Action::Continue;
    });
    walk_status.ensure();
    LOG_CHECK(!test_files_.empty()) << "No tests found";
    std::sort(test_files_.begin(), test_files_.end());

    test_name_column_width_ = 4;
    for (const std::string& s : test_files_) {
      test_name_column_width_ = std::max(test_name_column_width_, s.size());
    }
    test_idx_column_width_ = 1;
    size_t threshold = 10;
    while (test_files_.size() >= threshold) {
      ++test_idx_column_width_;
      threshold *= 10;
    }
    separator_length_ = test_idx_column_width_ + test_name_column_width_ + 60;

    printf("Executing %lu tests\n", test_files_.size());
    printf("%s\n", std::string(separator_length_, '=').c_str());
    printf("%*s  %-*s     Time      CPU  Status Comment\n", (int)test_idx_column_width_, "#",
           (int)test_name_column_width_, "Name");
    printf("%s\n", std::string(separator_length_, '=').c_str());
  }

  void run_next_test() {
    if (test_idx_ == test_files_.size()) {
      finish();
      return;
    }

    auto r_test = read_test_file();
    if (r_test.is_error()) {
      printf("%*lu  %-*s %8.5f %8.5f  FATAL  %s\n", (int)test_idx_column_width_, test_idx_ + 1,
             (int)test_name_column_width_, test_files_[test_idx_].c_str(), 0.0, 0.0,
             r_test.error().to_string().c_str());
      fflush(stdout);
      ++cnt_fatal_;
      ++test_idx_;
      run_next_test();
      return;
    }

    auto test = r_test.move_as_ok();
    BlockIdExt block_id = create_block_id(test->block_id_);
    td::BufferSlice block_data = std::move(test->block_data_);
    td::BufferSlice collated_data = std::move(test->collated_data_);
    bool valid = test->valid_;

    td::Ref<vm::Cell> original_merkle_update;
    auto S = [&]() -> td::Status {
      TRY_RESULT(root, vm::std_boc_deserialize(block_data));
      block::gen::Block::Record rec;
      if (!block::gen::t_Block.cell_unpack(root, rec)) {
        return td::Status::Error("Failed to parse block root");
      }
      vm::CellSlice cs{rec.state_update->load_cell().move_as_ok()};
      if (cs.special_type() != vm::CellTraits::SpecialType::MerkleUpdate) {
        return td::Status::Error("Invalid Merkle Update in block");
      }
      original_merkle_update = rec.state_update;
      rec.state_update = vm::CellBuilder{}.finalize_novm();
      if (!block::gen::pack_cell(root, rec)) {
        return td::Status::Error("Failed to pack new block root");
      }
      TRY_RESULT_ASSIGN(block_data, vm::std_boc_serialize(root, 31));
      return td::Status::OK();
    }();
    if (S.is_error()) {
      printf("%*lu  %-*s %8.5f %8.5f  FATAL  %s\n", (int)test_idx_column_width_, test_idx_ + 1,
             (int)test_name_column_width_, test_files_[test_idx_].c_str(), 0.0, 0.0, S.to_string().c_str());
      fflush(stdout);
      ++cnt_fatal_;
      ++test_idx_;
      run_next_test();
      return;
    }

    run_contest_solution(
        block_id, std::move(block_data), std::move(collated_data),
        [=, SelfId = actor_id(this), timer = td::Timer{}, start_cpu = get_cpu_usage()](td::Result<td::BufferSlice> R) {
          td::actor::send_closure(SelfId, &ContestGrader::got_solution_result, std::move(R), valid,
                                  original_merkle_update, timer.elapsed(),
                                  (double)(get_cpu_usage() - start_cpu) / CPU_USAGE_PER_SEC);
        });
  }

  td::Result<tl_object_ptr<ton_api::contest_test>> read_test_file() {
    TRY_RESULT(data, td::read_file(tests_dir_ + "/" + test_files_[test_idx_]));
    return ton::fetch_tl_object<ton_api::contest_test>(data, true);
  }

  void got_solution_result(td::Result<td::BufferSlice> res, bool valid, td::Ref<vm::Cell> original_merkle_update,
                           double elapsed, double cpu_time) {
    bool got_valid = res.is_ok();
    if (got_valid != valid) {
      printf("%*lu  %-*s %8.5f %8.5f  ERROR  expected %s, found %s\n", (int)test_idx_column_width_, test_idx_ + 1,
             (int)test_name_column_width_, test_files_[test_idx_].c_str(), elapsed, cpu_time,
             (valid ? "VALID" : "INVALID"), (got_valid ? "VALID" : "INVALID"));
      fflush(stdout);
      ++cnt_fail_;
      ++test_idx_;
      run_next_test();
      return;
    }
    if (!valid) {
      printf("%*lu  %-*s %8.5f %8.5f  OK     block is INVALID\n", (int)test_idx_column_width_, test_idx_ + 1,
             (int)test_name_column_width_, test_files_[test_idx_].c_str(), elapsed, cpu_time);
      fflush(stdout);
      ++cnt_ok_;
      ++test_idx_;
      run_next_test();
      return;
    }
    auto S = check_merkle_update(res.move_as_ok(), original_merkle_update);
    if (S.is_error()) {
      printf("%*lu  %-*s %8.5f %8.5f  ERROR  invalid Merkle update %s\n", (int)test_idx_column_width_, test_idx_ + 1,
             (int)test_name_column_width_, test_files_[test_idx_].c_str(), elapsed, cpu_time, S.to_string().c_str());
      fflush(stdout);
      ++cnt_fail_;
      ++test_idx_;
      run_next_test();
      return;
    }

    printf("%*lu  %-*s %8.5f %8.5f  OK     block is VALID\n", (int)test_idx_column_width_, test_idx_ + 1,
           (int)test_name_column_width_, test_files_[test_idx_].c_str(), elapsed, cpu_time);
    fflush(stdout);
    total_time_ += elapsed;
    total_cpu_time_ += cpu_time;
    ++cnt_ok_;
    ++test_idx_;
    run_next_test();
  }

  td::Status check_merkle_update(td::Slice data, td::Ref<vm::Cell> original_merkle_update) {
    TRY_RESULT(new_merkle_update, vm::std_boc_deserialize(data));
    TRY_STATUS(vm::MerkleUpdate::validate(new_merkle_update));

    vm::CellSlice new_cs{new_merkle_update->load_cell().move_as_ok()};
    vm::CellSlice old_cs{original_merkle_update->load_cell().move_as_ok()};
    if (new_cs.lex_cmp(old_cs)) {  // compare hashes in Merkle update roots
      return td::Status::Error("Merkle Update does not match the original Merkle Update");
    }
    return td::Status::OK();
  }

  void finish() {
    printf("%s\n", std::string(separator_length_, '=').c_str());
    printf("Passed %lu/%lu tests\n", cnt_ok_, test_files_.size());
    printf("Total time (only passed valid tests): %.5f\n", total_time_);
    printf("Total CPU time (only passed valid tests): %.5f\n", total_cpu_time_);
    if (cnt_fail_ > 0) {
      printf("Failed %lu/%lu tests\n", cnt_fail_, test_files_.size());
    }
    if (cnt_fatal_ > 0) {
      printf("FATAL ERROR %lu/%lu tests\n", cnt_fatal_, test_files_.size());
    }
    exit(0);
  }

 private:
  std::string tests_dir_;
  std::vector<std::string> test_files_;
  size_t test_idx_ = 0;
  size_t cnt_ok_ = 0, cnt_fail_ = 0, cnt_fatal_ = 0;

  size_t test_idx_column_width_ = 0;
  size_t test_name_column_width_ = 0;
  size_t separator_length_ = 0;

  double total_time_ = 0.0;
  double total_cpu_time_ = 0.0;
};

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_ERROR);

  td::actor::ActorOwn<ContestGrader> x;
  td::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  td::OptionParser p;
  p.set_description("Block validation contest");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_option('h', "help", "prints a help message", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  std::string tests_dir = "tests/";
  p.add_option('d', "tests", "directory with tests (default: tests/)",
               [&](td::Slice arg) { tests_dir = arg.str() + "/"; });
  td::uint32 threads = 8;
  p.add_checked_option('t', "threads", "number of threads (default: 8)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(threads, td::to_integer_safe<td::uint32>(arg));
    return td::Status::OK();
  });

  p.run(argc, argv).ensure();
  td::actor::Scheduler scheduler({threads});

  scheduler.run_in_context([&] { x = td::actor::create_actor<ContestGrader>("grader", tests_dir); });
  while (scheduler.run(1)) {
  }

  return 0;
}