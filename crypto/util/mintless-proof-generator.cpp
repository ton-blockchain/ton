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

#include "block-parse.h"
#include "block.h"
#include "td/actor/core/Actor.h"
#include "td/db/utils/BlobView.h"

#include <iostream>
#include "td/utils/OptionParser.h"
#include "td/utils/Time.h"
#include "td/utils/base64.h"
#include "td/utils/filesystem.h"
#include "td/utils/logging.h"
#include "vm/cells/MerkleProof.h"
#include "vm/db/StaticBagOfCellsDb.h"

#include <fstream>
#include <common/delay.h>

const size_t KEY_LEN = 3 + 8 + 256;

void print_help() {
  std::cerr << "mintless-proof-generator - generates proofs for mintless jettons. Usage:\n\n";
  std::cerr << "mintless-proof-generator generate <input-list> <output-file>\n";
  std::cerr << "  Generate a full tree for <input-list>, save boc to <output-file>.\n";
  std::cerr << "  Input format: each line is <address> <amount> <start_from> <expired_at>.\n\n";
  std::cerr << "mintless-proof-generator make_proof <input-boc> <address> <output-file>.\n";
  std::cerr << "  Generate a proof for address <address> from tree <input-boc>, save boc to file <output-file>.\n\n";
  std::cerr << "mintless-proof-generator parse <input-boc> <output-file>\n";
  std::cerr << "  Read a tree from <input-boc> and output it as text to <output-file>.\n";
  std::cerr << "  Output format: same as input for 'generate'.\n\n";
  std::cerr << "mintless-proof-generator make_all_proofs <input-boc> <output-file> [--threads <threads>]\n";
  std::cerr << "  Read a tree from <input-boc> and output proofs for all accounts to <output-file>.\n";
  std::cerr << "  Output format: <address>,<proof-base64>\n";
  std::cerr << "  Default <threads>: 1\n";
  exit(2);
}

void log_mem_stat() {
  auto r_stat = td::mem_stat();
  if (r_stat.is_error()) {
    LOG(WARNING) << "Memory: " << r_stat.move_as_error();
    return;
  }
  auto stat = r_stat.move_as_ok();
  LOG(WARNING) << "Memory: "
               << "res=" << stat.resident_size_ << " (peak=" << stat.resident_size_peak_
               << ") virt=" << stat.virtual_size_ << " (peak=" << stat.virtual_size_peak_ << ")";
}

td::BitArray<KEY_LEN> address_to_key(const block::StdAddress &address) {
  // addr_std$10 anycast:(Maybe Anycast) workchain_id:int8 address:bits256 = MsgAddressInt;
  vm::CellBuilder cb;
  cb.store_long(0b100, 3);
  cb.store_long(address.workchain, 8);
  cb.store_bits(address.addr.as_bitslice());
  return cb.data_bits();
}

block::StdAddress key_to_address(const td::BitArray<KEY_LEN> &key) {
  block::StdAddress addr;
  td::ConstBitPtr ptr = key.bits();
  LOG_CHECK(ptr.get_uint(3) == 0b100) << "Invalid address";
  ptr.advance(3);
  addr.workchain = (ton::WorkchainId)ptr.get_int(8);
  ptr.advance(8);
  addr.addr = ptr;
  return addr;
}

struct Entry {
  block::StdAddress address;
  td::RefInt256 amount;
  td::uint64 start_from = 0, expired_at = 0;

  td::BitArray<KEY_LEN> get_key() const {
    return address_to_key(address);
  }

  td::Ref<vm::CellSlice> get_value() const {
    // _ amount:Coins start_from:uint48 expired_at:uint48 = AirdropItem;
    vm::CellBuilder cb;
    bool ok = block::tlb::t_Grams.store_integer_value(cb, *amount) && cb.store_ulong_rchk_bool(start_from, 48) &&
              cb.store_ulong_rchk_bool(expired_at, 48);
    LOG_CHECK(ok) << "Failed to serialize AirdropItem";
    return cb.as_cellslice_ref();
  }

  static Entry parse(const td::BitArray<KEY_LEN> &key, vm::CellSlice value) {
    Entry e;
    e.address = key_to_address(key);
    bool ok = block::tlb::t_Grams.as_integer_skip_to(value, e.amount) && value.fetch_uint_to(48, e.start_from) &&
              value.fetch_uint_to(48, e.expired_at) && value.empty_ext();
    LOG_CHECK(ok) << "Failed to parse AirdropItem";
    return e;
  }
};

bool read_entry(std::istream &f, Entry &entry) {
  std::string line;
  while (std::getline(f, line)) {
    std::vector<std::string> v = td::full_split(line, ' ');
    if (v.empty()) {
      continue;
    }
    auto S = [&]() -> td::Status {
      if (v.size() != 4) {
        return td::Status::Error("Invalid line in input");
      }
      TRY_RESULT_PREFIX_ASSIGN(entry.address, block::StdAddress::parse(v[0]), "Invalid address in input: ");
      entry.amount = td::string_to_int256(v[1]);
      if (entry.amount.is_null() || !entry.amount->is_valid() || entry.amount->sgn() < 0) {
        return td::Status::Error(PSTRING() << "Invalid amount in input: " << v[1]);
      }
      TRY_RESULT_PREFIX_ASSIGN(entry.start_from, td::to_integer_safe<td::uint64>(v[2]),
                               "Invalid start_from in input: ");
      TRY_RESULT_PREFIX_ASSIGN(entry.expired_at, td::to_integer_safe<td::uint64>(v[3]),
                               "Invalid expired_at in input: ");
      return td::Status::OK();
    }();
    S.ensure();
    return true;
  }
  return false;
}

td::Status run_generate(std::string in_filename, std::string out_filename) {
  LOG(INFO) << "Generating tree from " << in_filename;
  std::ifstream in_file{in_filename};
  LOG_CHECK(in_file.is_open()) << "Cannot open file " << in_filename;

  Entry entry;
  vm::Dictionary dict{KEY_LEN};
  td::uint64 count = 0;
  td::Timestamp log_at = td::Timestamp::in(5.0);
  while (read_entry(in_file, entry)) {
    ++count;
    bool ok = dict.set(entry.get_key(), entry.get_value(), vm::DictionaryBase::SetMode::Add);
    LOG_CHECK(ok) << "Failed to add entry " << entry.address.rserialize() << " (line #" << count << ")";
    if (log_at.is_in_past()) {
      LOG(INFO) << "Added " << count << " entries";
      log_at = td::Timestamp::in(5.0);
    }
  }
  LOG_CHECK(in_file.eof()) << "Failed to read file " << in_filename;
  in_file.close();

  LOG_CHECK(count != 0) << "Input is empty";
  td::Ref<vm::Cell> root = dict.get_root_cell();
  LOG(INFO) << "Total: " << count << " entries, root hash: " << root->get_hash().to_hex();
  vm::BagOfCells boc;
  boc.add_root(root);
  TRY_STATUS(boc.import_cells());
  LOG(INFO) << "Writing to " << out_filename;
  TRY_RESULT(fd, td::FileFd::open(out_filename, td::FileFd::Write | td::FileFd::Truncate | td::FileFd::Create));
  TRY_STATUS(boc.serialize_to_file(fd, 31));
  TRY_STATUS(fd.sync());
  fd.close();
  log_mem_stat();
  return td::Status::OK();
}

td::Status run_make_proof(std::string in_filename, std::string s_address, std::string out_filename) {
  LOG(INFO) << "Generating proof for " << s_address << ", input file is " << in_filename;
  TRY_RESULT(address, block::StdAddress::parse(s_address));

  TRY_RESULT(blob_view, td::FileBlobView::create(in_filename));
  TRY_RESULT(boc, vm::StaticBagOfCellsDbLazy::create(std::move(blob_view)));
  TRY_RESULT(root, boc->get_root_cell(0));

  vm::MerkleProofBuilder mpb{root};
  vm::Dictionary dict{mpb.root(), KEY_LEN};
  auto key = address_to_key(address);
  td::Ref<vm::CellSlice> value = dict.lookup(key);
  LOG_CHECK(value.not_null()) << "No entry for address " << s_address;
  Entry e = Entry::parse(key, *value);
  LOG(INFO) << "Entry: address=" << e.address.workchain << ":" << e.address.addr.to_hex()
            << " amount=" << e.amount->to_dec_string() << " start_from=" << e.start_from
            << " expire_at=" << e.expired_at;

  TRY_RESULT(proof, mpb.extract_proof_boc());
  LOG(INFO) << "Writing proof to " << out_filename << " (" << td::format::as_size(proof.size()) << ")";
  TRY_STATUS(td::write_file(out_filename, proof));
  log_mem_stat();
  return td::Status::OK();
}

td::Status run_parse(std::string in_filename, std::string out_filename) {
  LOG(INFO) << "Parsing " << in_filename;
  std::ofstream out_file{out_filename};
  LOG_CHECK(out_file.is_open()) << "Cannot open file " << out_filename;

  TRY_RESULT(blob_view, td::FileBlobView::create(in_filename));
  TRY_RESULT(boc, vm::StaticBagOfCellsDbLazy::create(std::move(blob_view)));
  TRY_RESULT(root, boc->get_root_cell(0));
  LOG(INFO) << "Root hash = " << root->get_hash().to_hex();
  vm::Dictionary dict{root, KEY_LEN};
  td::Timestamp log_at = td::Timestamp::in(5.0);
  td::uint64 count = 0;
  bool ok = dict.check_for_each([&](td::Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
    CHECK(key_len == KEY_LEN);
    Entry e = Entry::parse(key, *value);
    out_file << e.address.workchain << ":" << e.address.addr.to_hex() << " " << e.amount->to_dec_string() << " "
             << e.start_from << " " << e.expired_at << "\n";
    LOG_CHECK(!out_file.fail()) << "Failed to write to " << out_filename;
    ++count;
    if (log_at.is_in_past()) {
      LOG(INFO) << "Parsed " << count << " entries";
      log_at = td::Timestamp::in(5.0);
    }
    return true;
  });
  LOG_CHECK(ok) << "Failed to parse dictionary";
  out_file.close();
  LOG_CHECK(!out_file.fail()) << "Failed to write to " << out_filename;
  LOG(INFO) << "Written " << count << " entries to " << out_filename;
  log_mem_stat();
  return td::Status::OK();
}

class MakeAllProofsActor : public td::actor::core::Actor {
 public:
  MakeAllProofsActor(std::string in_filename, std::string out_filename, int max_workers)
      : in_filename_(in_filename), out_filename_(out_filename), max_workers_(max_workers) {
  }

  void start_up() override {
    auto S = [&]() -> td::Status {
      out_file_.open(out_filename_);
      LOG_CHECK(out_file_.is_open()) << "Cannot open file " << out_filename_;
      LOG(INFO) << "Reading " << in_filename_;
      TRY_RESULT(blob_view, td::FileBlobView::create(in_filename_));
      TRY_RESULT(boc, vm::StaticBagOfCellsDbLazy::create(std::move(blob_view)));
      TRY_RESULT(root, boc->get_root_cell(0));
      LOG(INFO) << "Root hash = " << root->get_hash().to_hex();
      dict_ = vm::Dictionary{root, KEY_LEN};
      return td::Status::OK();
    }();
    S.ensure();
    run();
    alarm_timestamp() = td::Timestamp::in(5.0);
  }

  void alarm() override {
    alarm_timestamp() = td::Timestamp::in(5.0);
    LOG(INFO) << "Processed " << written_count_ << " entries";
  }

  void run() {
    for (auto it = pending_results_.begin(); it != pending_results_.end() && !it->second.empty();) {
      out_file_ << it->second << "\n";
      LOG_CHECK(!out_file_.fail()) << "Failed to write to " << out_filename_;
      it = pending_results_.erase(it);
      ++written_count_;
    }
    while (active_workers_ < max_workers_ && !eof_) {
      td::Ref<vm::CellSlice> value = dict_.lookup_nearest_key(current_key_, true, current_idx_ == 0);
      if (value.is_null()) {
        eof_ = true;
        break;
      }
      run_worker(current_key_, current_idx_);
      ++current_idx_;
      ++active_workers_;
    }
    if (eof_ && active_workers_ == 0) {
      out_file_.close();
      LOG_CHECK(!out_file_.fail()) << "Failed to write to " << out_filename_;
      LOG(INFO) << "Written " << written_count_ << " entries to " << out_filename_;
      stop();
      td::actor::SchedulerContext::get()->stop();
    }
  }

  void run_worker(td::BitArray<KEY_LEN> key, td::uint64 idx) {
    pending_results_[idx] = "";
    ton::delay_action(
        [SelfId = actor_id(this), key, idx, root = dict_.get_root_cell()]() {
          vm::MerkleProofBuilder mpb{root};
          CHECK(vm::Dictionary(mpb.root(), KEY_LEN).lookup(key).not_null());
          auto r_proof = mpb.extract_proof_boc();
          r_proof.ensure();
          block::StdAddress addr = key_to_address(key);
          std::string result = PSTRING() << addr.workchain << ":" << addr.addr.to_hex() << ","
                                         << td::base64_encode(r_proof.move_as_ok());
          td::actor::send_closure(SelfId, &MakeAllProofsActor::on_result, idx, std::move(result));
        },
        td::Timestamp::now());
  }

  void on_result(td::uint64 idx, std::string result) {
    pending_results_[idx] = std::move(result);
    --active_workers_;
    run();
  }

 private:
  std::string in_filename_, out_filename_;
  int max_workers_;

  std::ofstream out_file_;
  vm::Dictionary dict_{KEY_LEN};
  td::BitArray<KEY_LEN> current_key_ = td::BitArray<KEY_LEN>::zero();
  td::uint64 current_idx_ = 0;
  bool eof_ = false;
  int active_workers_ = 0;

  std::map<td::uint64, std::string> pending_results_;
  td::uint64 written_count_ = 0;
};

td::Status run_make_all_proofs(std::string in_filename, std::string out_filename, int threads) {
  td::actor::Scheduler scheduler({(size_t)threads});
  scheduler.run_in_context(
      [&] { td::actor::create_actor<MakeAllProofsActor>("proofs", in_filename, out_filename, threads).release(); });
  while (scheduler.run(1)) {
  }
  log_mem_stat();
  return td::Status::OK();
}

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_log_fatal_error_callback([](td::CSlice) { exit(2); });
  if (argc <= 1) {
    print_help();
    return 2;
  }

  std::string command = argv[1];
  try {
    if (command == "generate") {
      if (argc != 4) {
        print_help();
      }
      run_generate(argv[2], argv[3]).ensure();
      return 0;
    }
    if (command == "make_proof") {
      if (argc != 5) {
        print_help();
      }
      run_make_proof(argv[2], argv[3], argv[4]).ensure();
      return 0;
    }
    if (command == "parse") {
      if (argc != 4) {
        print_help();
      }
      run_parse(argv[2], argv[3]).ensure();
      return 0;
    }

    if (command == "make_all_proofs") {
      std::vector<std::string> args;
      int threads = 1;
      for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--threads")) {
          ++i;
          auto r = td::to_integer_safe<int>(td::as_slice(argv[i]));
          LOG_CHECK(r.is_ok() && r.ok() >= 1 && r.ok() <= 127) << "<threads> should be in [1..127]";
          threads = r.move_as_ok();
        } else {
          args.push_back(argv[i]);
        }
      }
      if (args.size() != 2) {
        print_help();
      }
      run_make_all_proofs(args[0], args[1], threads).ensure();
      return 0;
    }
  } catch (vm::VmError &e) {
    LOG(FATAL) << "VM error: " << e.get_msg();
  } catch (vm::VmVirtError &e) {
    LOG(FATAL) << "VM error: " << e.get_msg();
  }

  LOG(FATAL) << "Unknown command '" << command << "'";
}
