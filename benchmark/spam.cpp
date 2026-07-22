/*
    bench-spam — load generator + measurement client for the single-node jetton
    TPS benchmark (benchmark/DESIGN.md).

    Talks to a node's liteserver over the ADNL ext-client protocol:
      * pre-signs wallet-v5 externals (jetton transfers) in worker threads,
      * sends them via liteServer.sendMessage at a configured rate (token bucket),
      * watches workchain-0 blocks (lookupBlock seqno++ / getBlock), parses
        InMsgDescr to match sent message hashes -> inclusion latency, and
        ShardAccountBlocks to count transactions -> TPS,
      * optionally traces a sample of transfers through the full 3-tx chain
        (wallet -> sender jetton wallet -> recipient jetton wallet),
      * writes results.json + blocks.csv (+ optional timeline csv).

    If liteServer.getBlock fails (the ADNL ext protocol caps packets at 1<<24
    bytes, so 16MB+ blocks cannot be fetched), the tool falls back to
    liteServer.listBlockTransactions pagination and matches externals by
    account address (each wallet sends exactly one external per run).

    Subcommands:
      (none)     run the spammer (CLI contract in DESIGN.md)
      addr       print wallet #i's v5 address and jetton-wallet address
      selfcheck  unit-style checks of the external message builder
*/
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "auto/tl/lite_api.h"
#include "auto/tl/lite_api.hpp"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/block.h"
#include "block/mc-config.h"
#include "keys/keys.hpp"
#include "lite-client/ext-client.h"
#include "td/actor/actor.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Time.h"
#include "td/utils/base64.h"
#include "td/utils/filesystem.h"
#include "td/utils/misc.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/signals.h"
#include "tl-utils/lite-utils.hpp"
#include "ton/lite-tl.hpp"
#include "ton/ton-types.h"
#include "vm/boc.h"
#include "vm/cells/MerkleProof.h"
#include "vm/dict.h"
#include "vm/vm.h"

#include "common.h"

namespace bench {
namespace {

constexpr td::uint64 kRecipientSalt = 0x7265636970696e74ULL;  // "recipint"
constexpr td::uint64 kSampleSalt = 0x73616d706c652121ULL;     // "sample!!"

std::atomic<int> g_interrupts{0};
std::atomic<int> g_exit_code{0};

struct SpamOptions {
  std::string manifest_path;
  std::string contracts_dir = "benchmark/contracts";
  std::string liteserver_addr;
  std::string liteserver_pubkey_b64;
  double rate = 100.0;
  double duration = 60.0;
  double warmup = 5.0;
  double drain = 10.0;
  td::uint64 wallet_offset = 0;
  double track_sample = 0.0;
  std::string out_path = "results.json";
  std::string blocks_csv_path = "blocks.csv";
  std::string timeline_csv_path;
  int connections = 1;   // parallel send connections (one extra is used for watching)
  td::uint64 presign = 0;  // presign buffer size; 0 = auto
  int signer_threads = 0;  // 0 = auto
  td::uint64 index = 0;    // for the addr subcommand
  bool force_fallback = false;  // start in listBlockTransactions mode (testing)
};

struct Bits256Hash {
  std::size_t operator()(const td::Bits256 &x) const {
    std::size_t r;
    std::memcpy(&r, x.data(), sizeof(r));
    return r;
  }
};

td::BufferSlice envelope(td::BufferSlice query) {
  return ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_query>(std::move(query)), true);
}

// Unwraps a liteserver answer: transport errors pass through, liteServer.error
// payloads become td::Status errors.
td::Result<td::BufferSlice> unwrap_answer(td::Result<td::BufferSlice> R) {
  TRY_RESULT(data, std::move(R));
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_error>(data.clone(), true);
  if (F.is_ok()) {
    auto f = F.move_as_ok();
    return td::Status::Error(f->code_, f->message_);
  }
  return std::move(data);
}

double percentile(const std::vector<double> &sorted, double p) {
  if (sorted.empty()) {
    return 0.0;
  }
  double idx = p * static_cast<double>(sorted.size() - 1);
  size_t lo = static_cast<size_t>(idx);
  size_t hi = std::min(lo + 1, sorted.size() - 1);
  double frac = idx - static_cast<double>(lo);
  return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

std::string json_escape(td::Slice s) {
  std::string r;
  r.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') {
      r += '\\';
      r += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      r += buf;
    } else {
      r += c;
    }
  }
  return r;
}

// ---------------------------------------------------------------------------
// SignerPool: worker threads keep a buffer of pre-signed externals so that
// ed25519 signing (~0.1-0.3ms per message) never blocks the send hot path.
// ---------------------------------------------------------------------------

struct PresignedMsg {
  td::uint64 wallet_index{0};
  td::Bits256 msg_hash{};  // hash of the Message cell == InMsgDescr key
  td::Bits256 w5_addr{};
  td::Bits256 jw_addr{};
  td::BufferSlice query;  // pre-enveloped liteServer.query{liteServer.sendMessage{boc}}
  bool sampled{false};
};

class SignerPool {
 public:
  SignerPool(const Manifest &manifest, const ContractSet &contracts, td::uint64 first, td::uint64 count,
             td::uint64 target_buffer, double track_sample)
      : manifest_(manifest)
      , contracts_(contracts)
      , next_(first)
      , end_(first + count)
      , target_buffer_(target_buffer)
      , track_sample_(track_sample) {
  }
  ~SignerPool() {
    stop();
  }

  void start(int threads) {
    for (int i = 0; i < threads; i++) {
      threads_.emplace_back([this] { worker(); });
    }
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto &t : threads_) {
      t.join();
    }
    threads_.clear();
  }

  std::vector<PresignedMsg> take(size_t max_n) {
    std::vector<PresignedMsg> out;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      while (out.size() < max_n && !buffer_.empty()) {
        out.push_back(std::move(buffer_.front()));
        buffer_.pop_front();
      }
    }
    cv_.notify_all();
    return out;
  }

  bool exhausted() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_ >= end_ && buffer_.empty();
  }

  td::Status error() {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_.clone();
  }

 private:
  void worker() {
    while (true) {
      td::uint64 index;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return stop_ || (buffer_.size() < target_buffer_ && next_ < end_); });
        if (stop_ || next_ >= end_) {
          return;
        }
        index = next_++;
      }
      auto r = build(index);
      std::lock_guard<std::mutex> lock(mutex_);
      if (r.is_error()) {
        error_ = r.move_as_error();
        stop_ = true;
        cv_.notify_all();
        return;
      }
      buffer_.push_back(r.move_as_ok());
    }
  }

  td::Result<PresignedMsg> build(td::uint64 index) {
    PresignedMsg msg;
    msg.wallet_index = index;
    td::uint64 x = index ^ kRecipientSalt;
    td::uint64 recipient = splitmix64_next(x) % manifest_.num_v5;
    if (recipient == index) {
      recipient = (recipient + 1) % manifest_.num_v5;
    }
    if (track_sample_ > 0) {
      td::uint64 y = index ^ kSampleSalt;
      msg.sampled = static_cast<double>(splitmix64_next(y) >> 11) * 0x1p-53 < track_sample_;
    }
    TRY_RESULT(wallet, derive_wallet(manifest_.seed, index, manifest_.wallet_id, manifest_.minter_addr, contracts_));
    msg.w5_addr = wallet.w5_addr;
    msg.jw_addr = wallet.jw_addr;
    TRY_RESULT(ext, build_signed_external(manifest_.seed, index, recipient, manifest_, contracts_));
    msg.msg_hash = ext->get_hash().bits();
    TRY_RESULT(boc, vm::std_boc_serialize(ext, 31));
    msg.query = envelope(
        ton::create_serialize_tl_object<ton::lite_api::liteServer_sendMessage>(std::move(boc)));
    return std::move(msg);
  }

  const Manifest &manifest_;
  const ContractSet &contracts_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<PresignedMsg> buffer_;
  td::uint64 next_;
  const td::uint64 end_;
  const td::uint64 target_buffer_;
  const double track_sample_;
  bool stop_{false};
  td::Status error_;
  std::vector<std::thread> threads_;
};

// ---------------------------------------------------------------------------
// BlockParser: parses fetched blocks off the runner's critical path (block
// BoCs can reach tens of MB; deserialization takes 100+ ms). Also owns the
// chain-completion tracking state for sampled transfers:
//   stage1: ext msg hash       -> waiting for the wallet tx
//   stage2: transfer msg hash  -> waiting for the sender-jw tx
//   stage3: int-transfer hash  -> waiting for the recipient-jw tx (completion)
// ---------------------------------------------------------------------------

class BlockParser : public td::actor::Actor {
 public:
  struct Sample {
    td::Bits256 msg_hash;
    td::Bits256 w5_addr;
    td::Bits256 jw_addr;  // sender's jetton wallet
    double t_send{0};
  };
  struct Summary {
    td::uint32 utime{0};
    td::uint64 n_txs{0};
    std::vector<td::Bits256> in_msg_keys;
  };
  struct CompletionStats {
    std::vector<double> latencies_ms;
    td::uint64 samples_registered{0};
    td::uint64 tracking_failures{0};
  };

  void add_samples(std::vector<Sample> samples) {
    for (auto &s : samples) {
      samples_registered_++;
      stage1_.emplace(s.msg_hash, std::move(s));
    }
  }

  void parse_block(ton::BlockIdExt blkid, td::BufferSlice data, double observed_at,
                   td::Promise<Summary> promise) {
    promise.set_result([&]() -> td::Result<Summary> {
      try {
        return do_parse(blkid, std::move(data), observed_at);
      } catch (vm::VmError &e) {
        return e.as_status("block parse failed: ");
      } catch (vm::VmVirtError &e) {
        return e.as_status("block parse failed: ");
      }
    }());
  }

  void get_completion_stats(td::Promise<CompletionStats> promise) {
    CompletionStats stats;
    stats.latencies_ms = std::move(completion_ms_);
    stats.samples_registered = samples_registered_;
    stats.tracking_failures = tracking_failures_;
    promise.set_value(std::move(stats));
  }

 private:
  td::Result<Summary> do_parse(const ton::BlockIdExt &blkid, td::BufferSlice data, double observed_at) {
    TRY_RESULT(root, vm::std_boc_deserialize(std::move(data)));
    if (blkid.root_hash != td::Bits256{root->get_hash().bits()}) {
      return td::Status::Error(PSLICE() << "block " << blkid.to_str() << " root hash mismatch");
    }
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    block::gen::BlockExtra::Record extra;
    if (!(tlb::unpack_cell(root, blk) && tlb::unpack_cell(blk.info, info) && tlb::unpack_cell(blk.extra, extra))) {
      return td::Status::Error(PSLICE() << "cannot unpack block " << blkid.to_str());
    }
    Summary s;
    s.utime = info.gen_utime;

    vm::AugmentedDictionary in_dict{vm::load_cell_slice_ref(extra.in_msg_descr), 256,
                                    block::tlb::aug_InMsgDescrDefault};
    in_dict.check_for_each([&](td::Ref<vm::CellSlice>, td::ConstBitPtr key, int) {
      td::Bits256 hash;
      hash.bits().copy_from(key, 256);
      s.in_msg_keys.push_back(hash);
      return true;
    });

    vm::AugmentedDictionary acc_dict{vm::load_cell_slice_ref(extra.account_blocks), 256,
                                     block::tlb::aug_ShardAccountBlocks};
    acc_dict.check_for_each_extra([&](td::Ref<vm::CellSlice> value, td::Ref<vm::CellSlice>, td::ConstBitPtr, int) {
      block::gen::AccountBlock::Record acc_blk;
      if (!tlb::csr_unpack(std::move(value), acc_blk)) {
        return false;
      }
      vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                         block::tlb::aug_AccountTransactions};
      trans_dict.check_for_each([&](td::Ref<vm::CellSlice>, td::ConstBitPtr, int) {
        s.n_txs++;
        return true;
      });
      return true;
    });

    if (!stage1_.empty() || !stage2_.empty() || !stage3_.empty()) {
      track_chains(s, acc_dict, observed_at);
    }
    return std::move(s);
  }

  // Promote sampled transfers through the 3-tx chain. A later stage of the
  // same chain may complete in the same block (single-shard collators process
  // freshly generated internal messages immediately), so the stages are
  // scanned in order within one block.
  void track_chains(const Summary &s, vm::AugmentedDictionary &acc_dict, double observed_at) {
    auto promote = [&](std::unordered_map<td::Bits256, Sample, Bits256Hash> &from,
                       std::unordered_map<td::Bits256, Sample, Bits256Hash> *to, bool account_is_jw) {
      for (const auto &key : s.in_msg_keys) {
        auto it = from.find(key);
        if (it == from.end()) {
          continue;
        }
        Sample sample = std::move(it->second);
        from.erase(it);
        if (to == nullptr) {
          completion_ms_.push_back((observed_at - sample.t_send) * 1e3);
          continue;
        }
        auto r_out = find_out_msg_hash(acc_dict, account_is_jw ? sample.jw_addr : sample.w5_addr, key);
        if (r_out.is_error()) {
          tracking_failures_++;
          LOG(DEBUG) << "chain tracking lost a sample: " << r_out.error();
          continue;
        }
        to->emplace(r_out.move_as_ok(), std::move(sample));
      }
    };
    promote(stage1_, &stage2_, false);
    promote(stage2_, &stage3_, true);
    promote(stage3_, nullptr, false);
  }

  // Find the transaction of `account` whose inbound message has hash
  // `in_msg_hash` and return the hash of its (single expected) outbound message.
  static td::Result<td::Bits256> find_out_msg_hash(vm::AugmentedDictionary &acc_dict, const td::Bits256 &account,
                                                   const td::Bits256 &in_msg_hash) {
    auto value = acc_dict.lookup(account.bits(), 256);
    if (value.is_null()) {
      return td::Status::Error("no AccountBlock for the tracked account");
    }
    block::gen::AccountBlock::Record acc_blk;
    if (!tlb::csr_unpack(std::move(value), acc_blk)) {
      return td::Status::Error("cannot unpack AccountBlock");
    }
    vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                       block::tlb::aug_AccountTransactions};
    td::Result<td::Bits256> result = td::Status::Error("no transaction with the expected in-msg");
    trans_dict.check_for_each([&](td::Ref<vm::CellSlice> tvalue, td::ConstBitPtr, int) {
      auto tx_cell = tvalue->prefetch_ref();
      if (tx_cell.is_null()) {
        return true;
      }
      block::gen::Transaction::Record trans;
      if (!tlb::unpack_cell(tx_cell, trans)) {
        return true;
      }
      vm::CellSlice in_cs{*trans.r1.in_msg};
      if (in_cs.fetch_ulong(1) != 1) {
        return true;
      }
      auto in_ref = in_cs.fetch_ref();
      if (in_ref.is_null() || in_msg_hash != td::Bits256{in_ref->get_hash().bits()}) {
        return true;
      }
      vm::Dictionary out_dict{trans.r1.out_msgs, 15};
      td::Ref<vm::Cell> out;
      out_dict.check_for_each([&](td::Ref<vm::CellSlice> ov, td::ConstBitPtr, int) {
        if (out.is_null()) {
          out = ov->prefetch_ref();
        }
        return true;
      });
      if (out.is_null()) {
        result = td::Status::Error("tracked transaction has no out msgs");
      } else {
        result = td::Bits256{out->get_hash().bits()};
      }
      return false;  // stop iteration
    });
    return result;
  }

  std::unordered_map<td::Bits256, Sample, Bits256Hash> stage1_, stage2_, stage3_;
  std::vector<double> completion_ms_;
  td::uint64 samples_registered_{0};
  td::uint64 tracking_failures_{0};
};

// ---------------------------------------------------------------------------
// SpamRunner: the main actor. Token-bucket sender + block watcher + stats.
// ---------------------------------------------------------------------------

class SpamRunner : public td::actor::Actor {
 public:
  SpamRunner(SpamOptions opts, Manifest manifest, ContractSet contracts, ton::adnl::AdnlNodeIdFull server_id,
             td::IPAddress server_addr)
      : opts_(std::move(opts))
      , manifest_(std::move(manifest))
      , contracts_(std::move(contracts))
      , server_id_(std::move(server_id))
      , server_addr_(server_addr) {
  }

  void start_up() override {
    fallback_mode_ = opts_.force_fallback;
    start_time_ = td::Time::now();
    unix_offset_ = td::Clocks::system() - start_time_;
    last_tick_ = start_time_;
    next_timeline_ = start_time_;
    next_progress_ = start_time_ + 5.0;

    // clients_[0] is dedicated to the block watcher so that lookup polling and
    // (potentially huge) getBlock transfers never queue behind sendMessage
    // traffic; senders round-robin over the rest.
    for (int i = 0; i < opts_.connections + 1; i++) {
      clients_.push_back(liteclient::ExtClient::create(server_id_, server_addr_, nullptr));
    }
    parser_ = td::actor::create_actor<BlockParser>("blockparser");

    target_total_ = static_cast<td::uint64>(opts_.rate * opts_.duration + 0.5);
    if (opts_.wallet_offset + target_total_ > manifest_.num_v5) {
      target_total_ = manifest_.num_v5 - std::min(manifest_.num_v5, opts_.wallet_offset);
      LOG(WARNING) << "wallet range clamped to " << target_total_ << " messages (num_v5=" << manifest_.num_v5
                   << ", offset=" << opts_.wallet_offset << ")";
    }
    td::uint64 buffer = opts_.presign ? opts_.presign
                                      : std::min<td::uint64>(std::max<td::uint64>(td::uint64(opts_.rate * 2), 1000),
                                                             target_total_);
    int threads = opts_.signer_threads ? opts_.signer_threads
                                       : td::clamp(static_cast<int>(opts_.rate / 2500) + 1, 1, 8);
    signer_ = std::make_unique<SignerPool>(manifest_, contracts_, opts_.wallet_offset, target_total_, buffer,
                                           opts_.track_sample);
    signer_->start(threads);
    LOG(INFO) << "bench-spam: target " << target_total_ << " messages at " << opts_.rate << "/s, " << threads
              << " signer threads, presign buffer " << buffer << ", " << opts_.connections << " send connection(s)";

    alarm_timestamp() = td::Timestamp::in(0.01);
  }

  void alarm() override {
    double now = td::Time::now();
    if (g_interrupts.load(std::memory_order_relaxed) > 0 && !draining_) {
      LOG(WARNING) << "interrupted; stopping sends, draining for " << opts_.drain << "s";
      begin_drain(now);
    }
    if (g_interrupts.load(std::memory_order_relaxed) > 1 && !finishing_) {
      LOG(WARNING) << "second interrupt; finishing immediately";
      finish();
      return;
    }
    tick_send(now);
    tick_watch();
    tick_timeline(now);
    tick_progress(now);
    check_done(now);
    if (!finishing_) {
      alarm_timestamp() = td::Timestamp::in(0.01);
    }
  }

 private:
  struct SentRecord {
    td::uint64 wallet_index{0};
    double t_send{0};
    td::Bits256 msg_hash{};
    td::Bits256 w5_addr{};
  };
  struct BlockRec {
    td::uint32 seqno{0};
    td::uint32 utime{0};
    double observed_at{0};  // td::Time::now() clock
    td::uint64 n_txs{0};
    td::uint64 n_matched{0};
  };
  struct TimelineRow {
    double t{0};
    td::uint64 sent{0}, send_ok{0}, send_err{0}, included{0}, blocks{0};
  };

  // ---- sending ----

  void tick_send(double now) {
    if (sending_done_) {
      return;
    }
    double dt = now - last_tick_;
    last_tick_ = now;
    double burst_cap = std::max(opts_.rate * 0.25, 32.0);
    tokens_ = std::min(tokens_ + opts_.rate * dt, burst_cap);
    auto n = static_cast<size_t>(tokens_);
    if (n > 0) {
      auto batch = signer_->take(n);
      tokens_ -= static_cast<double>(batch.size());
      for (auto &msg : batch) {
        send_message(std::move(msg), now);
      }
      flush_samples();
    }
    if (sent_ >= target_total_) {
      LOG(INFO) << "all " << sent_ << " messages sent; draining for " << opts_.drain << "s";
      begin_drain(now);
    } else if (first_send_time_ > 0 && now - first_send_time_ >= opts_.duration) {
      LOG(WARNING) << "duration elapsed after " << sent_ << "/" << target_total_
                   << " messages (signer or sender could not sustain the rate); draining";
      begin_drain(now);
    } else {
      auto err = signer_->error();
      if (err.is_error()) {
        LOG(ERROR) << "signer failed: " << err;
        g_exit_code.store(2);
        begin_drain(now);
      }
    }
  }

  void send_message(PresignedMsg &&msg, double now) {
    if (first_send_time_ == 0) {
      first_send_time_ = now;
    }
    last_send_time_ = now;
    auto idx = static_cast<td::uint32>(sent_records_.size());
    sent_records_.push_back(SentRecord{msg.wallet_index, now, msg.msg_hash, msg.w5_addr});
    pending_by_hash_.emplace(msg.msg_hash, idx);
    pending_by_account_.emplace(msg.w5_addr, idx);
    if (msg.sampled) {
      sample_batch_.push_back(BlockParser::Sample{msg.msg_hash, msg.w5_addr, msg.jw_addr, now});
    }
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      td::actor::send_closure(SelfId, &SpamRunner::on_send_result, std::move(R));
    });
    auto &client = clients_[1 + (send_conn_rr_++ % opts_.connections)];
    td::actor::send_closure(client, &liteclient::ExtClient::send_query, "query", std::move(msg.query),
                            td::Timestamp::in(10.0), std::move(P));
    sent_++;
  }

  void flush_samples() {
    if (!sample_batch_.empty() && !fallback_mode_) {
      td::actor::send_closure(parser_, &BlockParser::add_samples, std::move(sample_batch_));
    }
    sample_batch_.clear();
  }

  void on_send_result(td::Result<td::BufferSlice> R) {
    auto r_data = unwrap_answer(std::move(R));
    if (r_data.is_error()) {
      send_err_++;
      auto msg = r_data.error().message().str();
      send_error_categories_[msg.substr(0, 80)]++;
      return;
    }
    auto F = ton::fetch_tl_object<ton::lite_api::liteServer_sendMsgStatus>(r_data.move_as_ok(), true);
    if (F.is_error()) {
      send_err_++;
      send_error_categories_["unparsable sendMsgStatus answer"]++;
      return;
    }
    if (F.ok()->status_ == 1) {
      send_ok_++;
    } else {
      send_err_++;
      send_error_categories_[PSTRING() << "sendMsgStatus status=" << F.ok()->status_]++;
    }
  }

  // ---- block watcher ----

  void tick_watch() {
    if (watcher_busy_ || !watcher_retry_.is_in_past()) {
      return;
    }
    watcher_busy_ = true;
    if (next_seqno_ == 0) {
      find_tip();
    } else {
      lookup_next();
    }
  }

  void watcher_retry_in(double delay) {
    watcher_busy_ = false;
    watcher_retry_ = td::Timestamp::in(delay);
  }

  void watcher_advance() {
    next_seqno_++;
    fetch_attempts_ = 0;
    transient_attempts_ = 0;
    watcher_retry_in(0.0);
  }

  void lite_query(size_t conn, td::BufferSlice query, double timeout,
                  void (SpamRunner::*handler)(td::Result<td::BufferSlice>)) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handler](td::Result<td::BufferSlice> R) {
      td::actor::send_closure(SelfId, handler, unwrap_answer(std::move(R)));
    });
    td::actor::send_closure(clients_[conn], &liteclient::ExtClient::send_query, "query", envelope(std::move(query)),
                            td::Timestamp::in(timeout), std::move(P));
  }

  void find_tip() {
    lite_query(0, ton::create_serialize_tl_object<ton::lite_api::liteServer_getMasterchainInfo>(), 5.0,
               &SpamRunner::got_mc_info);
  }

  void got_mc_info(td::Result<td::BufferSlice> R) {
    auto status = [&]() -> td::Status {
      TRY_RESULT(data, std::move(R));
      TRY_RESULT(f, ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfo>(std::move(data), true));
      auto last = ton::create_block_id(f->last_);
      lite_query(0,
                 ton::create_serialize_tl_object<ton::lite_api::liteServer_getAllShardsInfo>(
                     ton::create_tl_lite_block_id(last)),
                 5.0, &SpamRunner::got_shards);
      return td::Status::OK();
    }();
    if (status.is_error()) {
      LOG(INFO) << "getMasterchainInfo failed (will retry): " << status;
      watcher_retry_in(0.2);
    }
  }

  void got_shards(td::Result<td::BufferSlice> R) {
    auto status = [&]() -> td::Status {
      TRY_RESULT(data, std::move(R));
      TRY_RESULT(f, ton::fetch_tl_object<ton::lite_api::liteServer_allShardsInfo>(std::move(data), true));
      TRY_RESULT(root, vm::std_boc_deserialize(std::move(f->data_)));
      block::ShardConfig conf;
      if (!conf.unpack(vm::load_cell_slice_ref(root))) {
        return td::Status::Error("cannot unpack ShardHashes");
      }
      auto shard = conf.get_shard_hash(ton::ShardIdFull{0, ton::shardIdAll}, false);
      if (shard.is_null()) {
        return td::Status::Error("no workchain-0 shard in the shard config yet");
      }
      next_seqno_ = shard->top_block_id().id.seqno + 1;
      LOG(INFO) << "watching wc0 blocks from seqno " << next_seqno_;
      watcher_retry_in(0.0);
      return td::Status::OK();
    }();
    if (status.is_error()) {
      LOG(INFO) << "shard tip discovery failed (will retry): " << status;
      watcher_retry_in(0.2);
    }
  }

  void lookup_next() {
    auto q = ton::create_serialize_tl_object<ton::lite_api::liteServer_lookupBlock>(
        1, ton::create_tl_lite_block_id_simple(ton::BlockId{0, ton::shardIdAll, next_seqno_}), 0, 0);
    lite_query(0, std::move(q), 5.0, &SpamRunner::got_lookup);
  }

  void got_lookup(td::Result<td::BufferSlice> R) {
    auto status = [&]() -> td::Status {
      TRY_RESULT(data, std::move(R));
      TRY_RESULT(f, ton::fetch_tl_object<ton::lite_api::liteServer_blockHeader>(std::move(data), true));
      auto blkid = ton::create_block_id(f->id_);
      if (blkid.id.seqno != next_seqno_) {
        return td::Status::Error(PSLICE() << "lookupBlock returned wrong seqno " << blkid.id.seqno);
      }
      cur_observed_at_ = td::Time::now();
      cur_utime_ = 0;
      if (fallback_mode_) {
        // gen_utime is only available from the header proof in this mode.
        auto r_utime = parse_header_utime(std::move(f->header_proof_));
        if (r_utime.is_ok()) {
          cur_utime_ = r_utime.move_as_ok();
        }
        start_list_txs(blkid);
      } else {
        request_block(blkid);
      }
      return td::Status::OK();
    }();
    if (status.is_error()) {
      // Almost always "block not found": the next block was not produced yet.
      watcher_retry_in(0.06);
    }
  }

  static td::Result<td::uint32> parse_header_utime(td::BufferSlice proof) {
    TRY_RESULT(root, vm::std_boc_deserialize(std::move(proof)));
    try {
      TRY_RESULT(virt, vm::MerkleProof::virtualize(root));
      block::gen::Block::Record blk;
      block::gen::BlockInfo::Record info;
      if (!(tlb::unpack_cell(virt, blk) && tlb::unpack_cell(blk.info, info))) {
        return td::Status::Error("cannot unpack header proof");
      }
      return info.gen_utime;
    } catch (vm::VmError &e) {
      return e.as_status();
    } catch (vm::VmVirtError &e) {
      return e.as_status();
    }
  }

  void request_block(ton::BlockIdExt blkid) {
    auto q = ton::create_serialize_tl_object<ton::lite_api::liteServer_getBlock>(ton::create_tl_lite_block_id(blkid));
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), blkid](td::Result<td::BufferSlice> R) {
      td::actor::send_closure(SelfId, &SpamRunner::got_block, blkid, unwrap_answer(std::move(R)));
    });
    td::actor::send_closure(clients_[0], &liteclient::ExtClient::send_query, "query", envelope(std::move(q)),
                            td::Timestamp::in(15.0), std::move(P));
  }

  // The liteserver indexes blocks (ltdb/archive) slightly behind the shard
  // client; with large fast blocks lookups/fetches transiently fail with
  // "block not found ... possibly out of sync". Such errors must be retried
  // on the same seqno, NOT counted toward the >16MB fallback heuristic.
  static bool is_transient_block_error(const td::Status &s) {
    auto msg = s.message();
    return msg.str().find("not found") != std::string::npos ||
           msg.str().find("out of sync") != std::string::npos ||
           msg.str().find("notready") != std::string::npos;
  }

  void got_block(ton::BlockIdExt blkid, td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      if (is_transient_block_error(R.error()) && ++transient_attempts_ < 200) {
        watcher_retry_in(0.1);  // re-lookup the same seqno until the liteserver catches up
        return;
      }
      fetch_attempts_++;
      LOG(WARNING) << "getBlock(" << blkid.id.seqno << ") failed (attempt " << fetch_attempts_
                   << "): " << R.error();
      if (fetch_attempts_ >= 2) {
        // Most likely the block exceeds the ADNL ext-protocol packet limit
        // (1<<24 bytes, adnl/adnl-ext-connection.cpp); switch permanently to
        // listBlockTransactions pagination with per-account matching.
        LOG(ERROR) << "switching to listBlockTransactions fallback mode (chain-completion tracking disabled)";
        fallback_mode_ = true;
        // Re-do the lookup to obtain the header proof (gen_utime).
        watcher_retry_in(0.0);
        return;
      }
      watcher_retry_in(0.05);  // re-lookup the same seqno, then refetch
      return;
    }
    auto data = R.move_as_ok();
    auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockData>(std::move(data), true);
    if (F.is_error()) {
      LOG(ERROR) << "cannot parse getBlock answer for seqno " << blkid.id.seqno << "; skipping block";
      blocks_skipped_++;
      watcher_advance();
      return;
    }
    bytes_fetched_ += F.ok()->data_.size();
    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), blkid, observed = cur_observed_at_](td::Result<BlockParser::Summary> R2) {
          td::actor::send_closure(SelfId, &SpamRunner::on_block_summary, blkid, observed, std::move(R2));
        });
    td::actor::send_closure(parser_, &BlockParser::parse_block, blkid, std::move(F.move_as_ok()->data_),
                            cur_observed_at_, std::move(P));
  }

  void on_block_summary(ton::BlockIdExt blkid, double observed_at, td::Result<BlockParser::Summary> R) {
    if (R.is_error()) {
      LOG(ERROR) << "failed to parse block " << blkid.id.seqno << ": " << R.error() << "; skipping";
      parse_failures_++;
      blocks_skipped_++;
      watcher_advance();
      return;
    }
    auto summary = R.move_as_ok();
    BlockRec rec{blkid.id.seqno, summary.utime, observed_at, summary.n_txs, 0};
    for (const auto &key : summary.in_msg_keys) {
      auto it = pending_by_hash_.find(key);
      if (it == pending_by_hash_.end()) {
        continue;
      }
      const auto &sent = sent_records_[it->second];
      inclusion_ms_.push_back((observed_at - sent.t_send) * 1e3);
      rec.n_matched++;
      pending_by_account_.erase(sent.w5_addr);
      pending_by_hash_.erase(it);
    }
    blocks_.push_back(rec);
    watcher_advance();
  }

  // ---- fallback: listBlockTransactions pagination, match by account ----

  void start_list_txs(ton::BlockIdExt blkid) {
    fb_n_txs_ = 0;
    fb_matched_ = 0;
    list_more(blkid, nullptr);
  }

  void list_more(ton::BlockIdExt blkid, ton::tl_object_ptr<ton::lite_api::liteServer_transactionId3> after) {
    int mode = 1 + 2 + 4;  // account, lt, hash
    if (after) {
      mode |= 128;
    }
    auto q = ton::create_serialize_tl_object<ton::lite_api::liteServer_listBlockTransactions>(
        ton::create_tl_lite_block_id(blkid), mode, 256, std::move(after), false, false);
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), blkid](td::Result<td::BufferSlice> R) {
      td::actor::send_closure(SelfId, &SpamRunner::got_list, blkid, unwrap_answer(std::move(R)));
    });
    td::actor::send_closure(clients_[0], &liteclient::ExtClient::send_query, "query", envelope(std::move(q)),
                            td::Timestamp::in(10.0), std::move(P));
  }

  void got_list(ton::BlockIdExt blkid, td::Result<td::BufferSlice> R) {
    auto status = [&]() -> td::Status {
      TRY_RESULT(data, std::move(R));
      TRY_RESULT(f, ton::fetch_tl_object<ton::lite_api::liteServer_blockTransactions>(std::move(data), true));
      td::Bits256 last_account{};
      td::uint64 last_lt = 0;
      for (auto &id : f->ids_) {
        fb_n_txs_++;
        last_account = id->account_;
        last_lt = static_cast<td::uint64>(id->lt_);
        auto it = pending_by_account_.find(id->account_);
        if (it == pending_by_account_.end()) {
          continue;
        }
        const auto &sent = sent_records_[it->second];
        inclusion_ms_.push_back((cur_observed_at_ - sent.t_send) * 1e3);
        fb_matched_++;
        pending_by_hash_.erase(sent.msg_hash);
        pending_by_account_.erase(it);
      }
      if (f->incomplete_ && !f->ids_.empty()) {
        list_more(blkid,
                  ton::create_tl_object<ton::lite_api::liteServer_transactionId3>(last_account,
                                                                                  static_cast<td::int64>(last_lt)));
        return td::Status::OK();
      }
      blocks_.push_back(BlockRec{blkid.id.seqno, cur_utime_, cur_observed_at_, fb_n_txs_, fb_matched_});
      watcher_advance();
      return td::Status::OK();
    }();
    if (status.is_error()) {
      if (is_transient_block_error(status) && ++transient_attempts_ < 200) {
        watcher_retry_in(0.1);
        return;
      }
      fetch_attempts_++;
      LOG(WARNING) << "listBlockTransactions(" << blkid.id.seqno << ") failed: " << status;
      if (fetch_attempts_ >= 5) {
        blocks_skipped_++;
        watcher_advance();
      } else {
        watcher_retry_in(0.1);  // restart from lookup; the whole block is re-listed
      }
    }
  }

  // ---- lifecycle ----

  void begin_drain(double now) {
    if (draining_) {
      return;
    }
    draining_ = true;
    sending_done_ = true;
    flush_samples();
    drain_until_ = now + opts_.drain;
  }

  void tick_timeline(double now) {
    if (opts_.timeline_csv_path.empty() || now < next_timeline_) {
      return;
    }
    next_timeline_ = now + 1.0;
    timeline_.push_back(
        TimelineRow{now, sent_, send_ok_, send_err_, static_cast<td::uint64>(inclusion_ms_.size()), blocks_.size()});
  }

  void tick_progress(double now) {
    if (now < next_progress_) {
      return;
    }
    next_progress_ = now + 5.0;
    // Instantaneous throughput over a trailing window of observed blocks. The
    // span runs from the first out-of-window block (the true interval start) to
    // now, so the rate isn't deflated at the window edge.
    constexpr double kInstWindow = 5.0;
    double cutoff = now - kInstWindow;
    td::uint64 w_txs = 0, w_matched = 0;
    double lower = now;
    for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {
      lower = it->observed_at;
      if (it->observed_at < cutoff) {
        break;  // first block before the window: its timestamp bounds the span
      }
      w_txs += it->n_txs;
      w_matched += it->n_matched;
    }
    double span = now - lower;
    double inst_tps = span > 0.1 ? static_cast<double>(w_txs) / span : 0.0;
    double inst_jtps = span > 0.1 ? static_cast<double>(w_matched) / span : 0.0;
    LOG(INFO) << "progress: sent " << sent_ << "/" << target_total_ << " (ok " << send_ok_ << ", err " << send_err_
              << "), included " << inclusion_ms_.size() << ", blocks " << blocks_.size() << " (tip seqno "
              << (blocks_.empty() ? 0u : blocks_.back().seqno) << ")"
              << " | inst " << td::StringBuilder::FixedDouble(inst_jtps, 1) << " jTPS ("
              << td::StringBuilder::FixedDouble(inst_tps, 1) << " tx/s, last " << td::StringBuilder::FixedDouble(span, 1)
              << "s)" << (fallback_mode_ ? " [fallback mode]" : "");
  }

  void check_done(double now) {
    if (draining_ && !finishing_ && now >= drain_until_) {
      finish();
    }
  }

  void finish() {
    finishing_ = true;
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockParser::CompletionStats> R) {
      td::actor::send_closure(SelfId, &SpamRunner::on_completions, std::move(R));
    });
    td::actor::send_closure(parser_, &BlockParser::get_completion_stats, std::move(P));
  }

  void on_completions(td::Result<BlockParser::CompletionStats> R) {
    BlockParser::CompletionStats chain;
    if (R.is_ok()) {
      chain = R.move_as_ok();
    }
    write_outputs(chain);
    signer_->stop();
    if (inclusion_ms_.empty() && g_exit_code.load() == 0) {
      LOG(ERROR) << "no sent externals were observed in any block";
      g_exit_code.store(1);
    }
    td::actor::SchedulerContext::get().stop();
    stop();
  }

  // ---- output ----

  struct LatencyStats {
    double p50{0}, p90{0}, p99{0}, p999{0}, mean{0};
    size_t samples{0};
  };

  static LatencyStats latency_stats(std::vector<double> v) {
    LatencyStats s;
    s.samples = v.size();
    if (v.empty()) {
      return s;
    }
    std::sort(v.begin(), v.end());
    s.p50 = percentile(v, 0.50);
    s.p90 = percentile(v, 0.90);
    s.p99 = percentile(v, 0.99);
    s.p999 = percentile(v, 0.999);
    double sum = 0;
    for (double x : v) {
      sum += x;
    }
    s.mean = sum / static_cast<double>(v.size());
    return s;
  }

  static void append_latency_json(std::ostringstream &os, const char *name, const LatencyStats &s) {
    os << '"' << name << "\":{\"p50\":" << s.p50 << ",\"p90\":" << s.p90 << ",\"p99\":" << s.p99
       << ",\"p99.9\":" << s.p999 << ",\"mean\":" << s.mean << ",\"samples\":" << s.samples << "}";
  }

  double to_unix_ms(double t) const {
    return (t + unix_offset_) * 1e3;
  }

  void write_outputs(const BlockParser::CompletionStats &chain) {
    // Steady-state window: [first_send + warmup, last_send]; tail drain excluded.
    double ws = first_send_time_ + opts_.warmup;
    double we = last_send_time_;
    if (first_send_time_ == 0 || we <= ws) {
      ws = first_send_time_;
      we = last_send_time_;
    }
    double window = we > ws ? we - ws : 0;
    td::uint64 win_txs = 0, win_matched = 0;
    for (const auto &b : blocks_) {
      if (b.observed_at > ws && b.observed_at <= we) {
        win_txs += b.n_txs;
        win_matched += b.n_matched;
      }
    }
    double tps_included = window > 0 ? static_cast<double>(win_txs) / window : 0;
    double jetton_tps = window > 0 ? static_cast<double>(win_matched) / window : 0;
    auto inc = latency_stats(inclusion_ms_);
    auto chn = latency_stats(chain.latencies_ms);
    double send_span = last_send_time_ > first_send_time_ ? last_send_time_ - first_send_time_ : 0;

    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(3);
    os << "{\"mode\":\"" << (fallback_mode_ ? "listBlockTransactions" : "getBlock") << "\"";
    os << ",\"sent\":" << sent_ << ",\"send_ok\":" << send_ok_ << ",\"send_errors\":" << send_err_;
    os << ",\"included\":" << inclusion_ms_.size()
       << ",\"unmatched\":" << pending_by_hash_.size();
    os << ",\"rate_target\":" << opts_.rate << ",\"duration_s\":" << send_span
       << ",\"warmup_s\":" << opts_.warmup << ",\"window_s\":" << window;
    os << ",\"wallet_offset\":" << opts_.wallet_offset;
    os << ",\"tps_included\":" << tps_included << ",\"jetton_tps\":" << jetton_tps << ",";
    append_latency_json(os, "inclusion_latency_ms", inc);
    os << ",";
    append_latency_json(os, "chain_latency_ms", chn);
    os << ",\"chain_samples_registered\":" << chain.samples_registered
       << ",\"chain_tracking_failures\":" << chain.tracking_failures;
    os << ",\"blocks_observed\":" << blocks_.size() << ",\"blocks_skipped\":" << blocks_skipped_
       << ",\"parse_failures\":" << parse_failures_ << ",\"bytes_fetched\":" << bytes_fetched_;
    os << ",\"send_error_categories\":{";
    bool first = true;
    for (const auto &[cat, cnt] : send_error_categories_) {
      os << (first ? "" : ",") << '"' << json_escape(cat) << "\":" << cnt;
      first = false;
    }
    os << "},\"blocks\":[";
    first = true;
    for (const auto &b : blocks_) {
      os << (first ? "" : ",") << "{\"seqno\":" << b.seqno << ",\"utime\":" << b.utime
         << ",\"observed_at_unix_ms\":" << static_cast<td::int64>(to_unix_ms(b.observed_at))
         << ",\"n_txs\":" << b.n_txs << ",\"n_ext_matched\":" << b.n_matched << "}";
      first = false;
    }
    os << "]}\n";
    auto st = td::write_file(opts_.out_path, os.str());
    if (st.is_error()) {
      LOG(ERROR) << "cannot write " << opts_.out_path << ": " << st;
      g_exit_code.store(2);
    }

    std::ostringstream csv;
    csv << "seqno,utime,observed_at_unix_ms,n_txs,n_ext_matched\n";
    for (const auto &b : blocks_) {
      csv << b.seqno << ',' << b.utime << ',' << static_cast<td::int64>(to_unix_ms(b.observed_at)) << ',' << b.n_txs
          << ',' << b.n_matched << '\n';
    }
    st = td::write_file(opts_.blocks_csv_path, csv.str());
    if (st.is_error()) {
      LOG(ERROR) << "cannot write " << opts_.blocks_csv_path << ": " << st;
      g_exit_code.store(2);
    }

    if (!opts_.timeline_csv_path.empty()) {
      std::ostringstream tcsv;
      tcsv << "unix_ms,sent,send_ok,send_err,included,blocks\n";
      for (const auto &row : timeline_) {
        tcsv << static_cast<td::int64>(to_unix_ms(row.t)) << ',' << row.sent << ',' << row.send_ok << ','
             << row.send_err << ',' << row.included << ',' << row.blocks << '\n';
      }
      st = td::write_file(opts_.timeline_csv_path, tcsv.str());
      if (st.is_error()) {
        LOG(ERROR) << "cannot write " << opts_.timeline_csv_path << ": " << st;
      }
    }

    printf("=== bench-spam summary ===\n");
    printf("  mode:               %s\n", fallback_mode_ ? "listBlockTransactions (fallback)" : "getBlock");
    printf("  sent:               %llu (ok %llu, errors %llu)\n", (unsigned long long)sent_,
           (unsigned long long)send_ok_, (unsigned long long)send_err_);
    printf("  included:           %zu (unmatched %zu)\n", inclusion_ms_.size(), pending_by_hash_.size());
    printf("  blocks observed:    %zu (skipped %llu)\n", blocks_.size(), (unsigned long long)blocks_skipped_);
    printf("  window:             %.1fs (warmup %.1fs excluded)\n", window, opts_.warmup);
    printf("  tps_included:       %.1f\n", tps_included);
    printf("  jetton_tps:         %.1f\n", jetton_tps);
    printf("  inclusion ms:       p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  mean %.1f (n=%zu)\n", inc.p50, inc.p90,
           inc.p99, inc.p999, inc.mean, inc.samples);
    printf("  chain ms:           p50 %.1f  p90 %.1f  p99 %.1f  mean %.1f (n=%zu, registered %llu, lost %llu)\n",
           chn.p50, chn.p90, chn.p99, chn.mean, chn.samples, (unsigned long long)chain.samples_registered,
           (unsigned long long)chain.tracking_failures);
    printf("  results:            %s\n", opts_.out_path.c_str());
    printf("  blocks csv:         %s\n", opts_.blocks_csv_path.c_str());
    fflush(stdout);
  }

  // ---- members ----

  SpamOptions opts_;
  Manifest manifest_;
  ContractSet contracts_;
  ton::adnl::AdnlNodeIdFull server_id_;
  td::IPAddress server_addr_;

  std::vector<td::actor::ActorOwn<liteclient::ExtClient>> clients_;
  td::actor::ActorOwn<BlockParser> parser_;
  std::unique_ptr<SignerPool> signer_;

  double start_time_{0};
  double unix_offset_{0};

  // sending
  td::uint64 target_total_{0};
  double tokens_{0};
  double last_tick_{0};
  td::uint64 sent_{0}, send_ok_{0}, send_err_{0};
  std::map<std::string, td::uint64> send_error_categories_;
  double first_send_time_{0}, last_send_time_{0};
  bool sending_done_{false};
  size_t send_conn_rr_{0};
  std::vector<BlockParser::Sample> sample_batch_;

  std::vector<SentRecord> sent_records_;
  std::unordered_map<td::Bits256, td::uint32, Bits256Hash> pending_by_hash_;
  std::unordered_map<td::Bits256, td::uint32, Bits256Hash> pending_by_account_;
  std::vector<double> inclusion_ms_;

  // watcher
  td::uint32 next_seqno_{0};
  bool watcher_busy_{false};
  td::Timestamp watcher_retry_{td::Timestamp::now()};
  bool fallback_mode_{false};
  int fetch_attempts_{0};
  int transient_attempts_{0};
  double cur_observed_at_{0};
  td::uint32 cur_utime_{0};
  td::uint64 fb_n_txs_{0}, fb_matched_{0};
  td::uint64 blocks_skipped_{0}, parse_failures_{0}, bytes_fetched_{0};
  std::vector<BlockRec> blocks_;

  // lifecycle
  bool draining_{false}, finishing_{false};
  double drain_until_{0};
  double next_timeline_{0}, next_progress_{0};
  std::vector<TimelineRow> timeline_;
};

// ---------------------------------------------------------------------------
// Subcommands
// ---------------------------------------------------------------------------

td::Result<std::pair<Manifest, ContractSet>> load_inputs(const SpamOptions &opts) {
  if (opts.manifest_path.empty()) {
    return td::Status::Error("--manifest is required");
  }
  TRY_RESULT(manifest_data, td::read_file(opts.manifest_path));
  TRY_RESULT(manifest, Manifest::from_json(manifest_data.as_slice()));
  TRY_RESULT(contracts, load_contracts(opts.contracts_dir));
  if (td::Bits256{contracts.w5_code->get_hash().bits()} != manifest.w5_code_hash &&
      !manifest.w5_code_hash.is_zero()) {
    return td::Status::Error("w5 code hash in manifest does not match --contracts-dir");
  }
  return std::make_pair(std::move(manifest), std::move(contracts));
}

int run_addr(const SpamOptions &opts) {
  auto r_inputs = load_inputs(opts);
  if (r_inputs.is_error()) {
    LOG(ERROR) << r_inputs.error();
    return 2;
  }
  auto [manifest, contracts] = r_inputs.move_as_ok();
  auto r_wallet = derive_wallet(manifest.seed, opts.index, manifest.wallet_id, manifest.minter_addr, contracts);
  if (r_wallet.is_error()) {
    LOG(ERROR) << r_wallet.error();
    return 2;
  }
  auto wallet = r_wallet.move_as_ok();
  printf("w5_addr=%s\njw_addr=%s\n", wallet.w5_addr.to_hex().c_str(), wallet.jw_addr.to_hex().c_str());
  return 0;
}

td::Status do_selfcheck(const Manifest &manifest, const ContractSet &contracts) {
  if (manifest.num_v5 < 2) {
    return td::Status::Error("selfcheck needs num_v5 >= 2 in the manifest");
  }
  TRY_RESULT(ext, build_signed_external(manifest.seed, 0, 1, manifest, contracts));

  // (1) the message passes TLB validation
  if (!block::gen::t_Message_Any.validate_ref(1000000, ext)) {
    return td::Status::Error("external message failed block::gen::Message validation");
  }

  // (2) the InMsgDescr matching key (Message cell hash) survives the BoC
  // round-trip we use on the wire
  TRY_RESULT(boc, vm::std_boc_serialize(ext, 31));
  TRY_RESULT(rt, vm::std_boc_deserialize(boc.clone()));
  if (rt->get_hash() != ext->get_hash()) {
    return td::Status::Error("BoC round-trip changed the message cell hash");
  }

  // (3) dest address == derived wallet address; signature verifies against the
  // derived pubkey over the slice-hash of the signed body
  TRY_RESULT(wallet0, derive_wallet(manifest.seed, 0, manifest.wallet_id, manifest.minter_addr, contracts));
  auto cs = vm::load_cell_slice(ext);
  if (cs.fetch_ulong(2) != 0b10 || cs.fetch_ulong(2) != 0) {
    return td::Status::Error("unexpected ext_in_msg_info header");
  }
  if (cs.fetch_ulong(3) != 0b100 || cs.fetch_ulong(8) != 0) {
    return td::Status::Error("unexpected dest addr_std header");
  }
  td::Bits256 dest;
  if (!cs.fetch_bits_to(dest.bits(), 256) || dest != wallet0.w5_addr) {
    return td::Status::Error("dest address does not match derived w5 address");
  }
  if (cs.fetch_ulong(4) != 0 || cs.fetch_ulong(1) != 0 || cs.fetch_ulong(1) != 0) {
    return td::Status::Error("unexpected import_fee/init/body markers");
  }
  if (cs.size() < 512 || cs.size_refs() != 1) {
    return td::Status::Error("unexpected body shape");
  }
  vm::CellBuilder unsigned_cb;
  unsigned_cb.append_bitslice(cs.prefetch_bits(static_cast<int>(cs.size()) - 512));
  unsigned_cb.store_ref(cs.prefetch_ref());
  auto unsigned_cell = unsigned_cb.finalize_novm();
  cs.advance(static_cast<int>(cs.size()) - 512);
  unsigned char sig[64];
  if (!cs.fetch_bytes(sig, 64)) {
    return td::Status::Error("cannot fetch signature");
  }
  TRY_RESULT(pubkey, td::Ed25519::PublicKey::from_slice(wallet0.pubkey.as_slice()));
  TRY_STATUS_PREFIX(pubkey.verify_signature(unsigned_cell->get_hash().as_slice(), td::Slice(sig, 64)),
                    "signature verification failed: ");

  // (4) address derivation is consistent between derive_wallet calls
  TRY_RESULT(again, derive_wallet(manifest.seed, 0, manifest.wallet_id, manifest.minter_addr, contracts));
  if (again.w5_addr != wallet0.w5_addr || again.jw_addr != wallet0.jw_addr) {
    return td::Status::Error("derive_wallet is not deterministic");
  }

  printf("selfcheck: ALL OK (msg_hash=%s)\n", ext->get_hash().to_hex().c_str());
  return td::Status::OK();
}

int run_selfcheck(const SpamOptions &opts) {
  auto r_inputs = load_inputs(opts);
  if (r_inputs.is_error()) {
    LOG(ERROR) << r_inputs.error();
    return 2;
  }
  auto [manifest, contracts] = r_inputs.move_as_ok();
  auto st = do_selfcheck(manifest, contracts);
  if (st.is_error()) {
    LOG(ERROR) << "selfcheck failed: " << st;
    return 1;
  }
  return 0;
}

int run_spam(const SpamOptions &opts) {
  auto r_inputs = load_inputs(opts);
  if (r_inputs.is_error()) {
    LOG(ERROR) << r_inputs.error();
    return 2;
  }
  auto [manifest, contracts] = r_inputs.move_as_ok();
  if (manifest.num_v5 < 2) {
    LOG(ERROR) << "manifest num_v5 must be >= 2";
    return 2;
  }
  if (opts.liteserver_addr.empty() || opts.liteserver_pubkey_b64.empty()) {
    LOG(ERROR) << "--liteserver and --liteserver-pubkey-b64 are required";
    return 2;
  }
  td::IPAddress addr;
  auto st = addr.init_host_port(opts.liteserver_addr);
  if (st.is_error()) {
    LOG(ERROR) << "bad --liteserver address: " << st;
    return 2;
  }
  auto r_key = td::base64_decode(opts.liteserver_pubkey_b64);
  if (r_key.is_error() || r_key.ok().size() != 32) {
    LOG(ERROR) << "--liteserver-pubkey-b64 must decode to 32 bytes";
    return 2;
  }
  td::Bits256 key_bits;
  key_bits.as_slice().copy_from(r_key.ok());
  ton::adnl::AdnlNodeIdFull server_id{ton::PublicKey{ton::pubkeys::Ed25519{key_bits}}};

  td::set_signal_handler(td::SignalType::Quit, [](int) {
    if (g_interrupts.fetch_add(1, std::memory_order_relaxed) >= 2) {
      std::_Exit(130);
    }
  }).ensure();

  td::actor::Scheduler scheduler({4});
  scheduler.run_in_context([&] {
    td::actor::create_actor<SpamRunner>("spamrunner", opts, std::move(manifest), std::move(contracts),
                                        std::move(server_id), addr)
        .release();
  });
  scheduler.run();
  return g_exit_code.load();
}

}  // namespace
}  // namespace bench

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  bench::SpamOptions opts;
  td::OptionParser p;
  p.set_description("bench-spam [addr|selfcheck] [options] (see benchmark/DESIGN.md)");
  p.add_option('\0', "manifest", "manifest.json written by bench-state-gen",
               [&](td::Slice arg) { opts.manifest_path = arg.str(); });
  p.add_option('\0', "contracts-dir", "directory with contract .boc files (default: benchmark/contracts)",
               [&](td::Slice arg) { opts.contracts_dir = arg.str(); });
  p.add_option('\0', "liteserver", "liteserver <ip>:<port>", [&](td::Slice arg) { opts.liteserver_addr = arg.str(); });
  p.add_option('\0', "liteserver-pubkey-b64", "liteserver ed25519 public key, base64",
               [&](td::Slice arg) { opts.liteserver_pubkey_b64 = arg.str(); });
  p.add_checked_option('\0', "rate", "external messages per second", [&](td::Slice arg) {
    opts.rate = td::to_double(arg);
    return opts.rate > 0 ? td::Status::OK() : td::Status::Error("--rate must be positive");
  });
  p.add_checked_option('\0', "duration", "send phase duration, seconds", [&](td::Slice arg) {
    opts.duration = td::to_double(arg);
    return opts.duration > 0 ? td::Status::OK() : td::Status::Error("--duration must be positive");
  });
  p.add_checked_option('\0', "warmup", "seconds excluded from the start of the measurement window",
                       [&](td::Slice arg) {
                         opts.warmup = td::to_double(arg);
                         return opts.warmup >= 0 ? td::Status::OK() : td::Status::Error("--warmup must be >= 0");
                       });
  p.add_checked_option('\0', "drain", "seconds to keep watching blocks after the send phase (default 10)",
                       [&](td::Slice arg) {
                         opts.drain = td::to_double(arg);
                         return opts.drain >= 0 ? td::Status::OK() : td::Status::Error("--drain must be >= 0");
                       });
  p.add_checked_option('\0', "wallet-offset", "first wallet index to use", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(opts.wallet_offset, td::to_integer_safe<td::uint64>(arg));
    return td::Status::OK();
  });
  p.add_checked_option('\0', "track-sample", "fraction of transfers to trace through the full tx chain",
                       [&](td::Slice arg) {
                         opts.track_sample = td::to_double(arg);
                         return opts.track_sample >= 0 && opts.track_sample <= 1
                                    ? td::Status::OK()
                                    : td::Status::Error("--track-sample must be in [0,1]");
                       });
  p.add_option('\0', "out", "results json path (default results.json)",
               [&](td::Slice arg) { opts.out_path = arg.str(); });
  p.add_option('\0', "blocks-csv", "per-block csv path (default blocks.csv)",
               [&](td::Slice arg) { opts.blocks_csv_path = arg.str(); });
  p.add_option('\0', "timeline-csv", "optional per-second timeline csv path",
               [&](td::Slice arg) { opts.timeline_csv_path = arg.str(); });
  p.add_checked_option('\0', "connections", "parallel ADNL connections for sending (default 1)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(opts.connections, td::to_integer_safe<int>(arg));
    return opts.connections > 0 ? td::Status::OK() : td::Status::Error("--connections must be positive");
  });
  p.add_checked_option('\0', "presign", "pre-sign buffer size (default: 2s worth of messages)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(opts.presign, td::to_integer_safe<td::uint64>(arg));
    return td::Status::OK();
  });
  p.add_checked_option('\0', "signer-threads", "signer thread count (default: auto from rate)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(opts.signer_threads, td::to_integer_safe<int>(arg));
    return opts.signer_threads >= 0 ? td::Status::OK() : td::Status::Error("--signer-threads must be >= 0");
  });
  p.add_option('\0', "force-fallback", "start in listBlockTransactions mode (for testing the fallback path)",
               [&] { opts.force_fallback = true; });
  p.add_checked_option('\0', "index", "wallet index (addr subcommand)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(opts.index, td::to_integer_safe<td::uint64>(arg));
    return td::Status::OK();
  });
  p.add_option('v', "verbosity", "verbosity level",
               [&](td::Slice arg) { SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + td::to_integer<int>(arg)); });

  auto r_rest = p.run(argc, argv);
  if (r_rest.is_error()) {
    LOG(ERROR) << r_rest.error();
    LOG(ERROR) << p;
    return 2;
  }
  auto rest = r_rest.move_as_ok();
  if (rest.size() > 1) {
    LOG(ERROR) << p;
    return 2;
  }
  std::string command = rest.empty() ? "spam" : rest[0];

  vm::init_vm().ensure();

  if (command == "addr") {
    return bench::run_addr(opts);
  }
  if (command == "selfcheck") {
    return bench::run_selfcheck(opts);
  }
  if (command == "spam") {
    return bench::run_spam(opts);
  }
  LOG(ERROR) << "unknown command " << command;
  LOG(ERROR) << p;
  return 2;
}
