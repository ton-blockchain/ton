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

#include <list>
#include <queue>
#include <map>

#include "catchain-types.h"
#include "catchain-receiver.h"
#include "catchain-receiver-source.h"
#include "catchain-received-block.h"

#include "td/db/KeyValueAsync.h"

namespace ton {

namespace catchain {

class CatChainReceiverImpl final : public CatChainReceiver {
 public:
  PrintId print_id() const override {
    return PrintId{incarnation_, local_id_};
  }

  void add_prepared_event(td::BufferSlice data) override {
    add_block(std::move(data), std::vector<CatChainBlockHash>());
  }
  CatChainSessionId get_incarnation() const override {
    return incarnation_;
  }
  void run_block(CatChainReceivedBlock *block) override;

  td::uint32 get_forks_cnt() const override {
    return total_forks_;
  }
  td::uint32 get_sources_cnt() const override {
    return static_cast<td::uint32>(sources_.size());
  }

  CatChainReceiverSource *get_source(td::uint32 source_id) const override {
    if (source_id >= get_sources_cnt()) {
      return nullptr;
    }

    return sources_[source_id].get();
  }
  PublicKeyHash get_source_hash(td::uint32 source_id) const override;
  CatChainReceiverSource *get_source_by_hash(const PublicKeyHash &source_hash) const;
  CatChainReceiverSource *get_source_by_adnl_id(adnl::AdnlNodeIdShort source_hash) const;

  td::uint32 add_fork() override;

  void deliver_block(CatChainReceivedBlock *block) override;

  void receive_message_from_overlay(adnl::AdnlNodeIdShort src, td::BufferSlice data);
  void receive_query_from_overlay(adnl::AdnlNodeIdShort src, td::BufferSlice data,
                                  td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::catchain_getBlock query, td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::catchain_getBlocks query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::catchain_getBlockHistory query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::catchain_getDifference query,
                     td::Promise<td::BufferSlice> promise);
  template <class T>
  void process_query(adnl::AdnlNodeIdShort src, const T &query, td::Promise<td::BufferSlice> promise) {
    //LOG(WARNING) << this << ": unknown query from " << src;
    callback_->on_custom_query(get_source_by_adnl_id(src)->get_hash(), serialize_tl_object(&query, true),
                               std::move(promise));
  }
  void receive_broadcast_from_overlay(const PublicKeyHash &src, td::BufferSlice data);

  void receive_block(adnl::AdnlNodeIdShort src, tl_object_ptr<ton_api::catchain_block> block, td::BufferSlice payload);
  void receive_block_answer(adnl::AdnlNodeIdShort src, td::BufferSlice);
  //void send_block(const PublicKeyHash &src, tl_object_ptr<ton_api::catchain_block> block, td::BufferSlice payload);

  CatChainReceivedBlock *create_block(tl_object_ptr<ton_api::catchain_block> block, td::SharedSlice payload) override;
  CatChainReceivedBlock *create_block(tl_object_ptr<ton_api::catchain_block_dep> block) override;

  td::Status validate_block_sync(const tl_object_ptr<ton_api::catchain_block_dep> &dep) const override;
  td::Status validate_block_sync(const tl_object_ptr<ton_api::catchain_block> &block,
                                 const td::Slice &payload) const override;

  void send_fec_broadcast(td::BufferSlice data) override;
  void send_custom_query_data(const PublicKeyHash &dst, std::string name, td::Promise<td::BufferSlice> promise,
                              td::Timestamp timeout, td::BufferSlice query) override;
  void send_custom_query_data_via(const PublicKeyHash &dst, std::string name, td::Promise<td::BufferSlice> promise,
                                  td::Timestamp timeout, td::BufferSlice query, td::uint64 max_answer_size,
                                  td::actor::ActorId<adnl::AdnlSenderInterface> via) override;
  void send_custom_message_data(const PublicKeyHash &dst, td::BufferSlice query) override;

  void run_scheduler();
  void add_block(td::BufferSlice data, std::vector<CatChainBlockHash> deps) override;
  void add_block_cont(tl_object_ptr<ton_api::catchain_block> block, td::BufferSlice payload);
  void add_block_cont_2(tl_object_ptr<ton_api::catchain_block> block, td::BufferSlice payload);
  void add_block_cont_3(tl_object_ptr<ton_api::catchain_block> block, td::BufferSlice payload);
  void debug_add_fork(td::BufferSlice payload, CatChainBlockHeight height,
                      std::vector<CatChainBlockHash> deps) override;
  void debug_add_fork_cont(tl_object_ptr<ton_api::catchain_block> block, td::BufferSlice payload);
  void on_blame(td::uint32 src) override {
    callback_->blame(src);
  }
  const CatChainOptions &opts() const override {
    return opts_;
  }

  void got_fork_proof(td::BufferSlice data);
  void synchronize_with(CatChainReceiverSource *source);
  void alarm() override;
  void start_up() override;
  void tear_down() override;
  void read_db();
  void read_db_from(CatChainBlockHash id);
  void read_block_from_db(CatChainBlockHash id, td::BufferSlice data);

  void block_written_to_db(CatChainBlockHash hash);

  bool unsafe_start_up_check_completed();
  void written_unsafe_root_block(CatChainReceivedBlock *block);

  void destroy() override;

  CatChainReceivedBlock *get_block(CatChainBlockHash hash) const;

  CatChainReceiverImpl(std::unique_ptr<Callback> callback, const CatChainOptions &opts,
                       td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                       td::actor::ActorId<overlay::Overlays> overlays, const std::vector<CatChainNode> &ids,
                       const PublicKeyHash &local_id, const CatChainBlockHash &unique_hash, std::string db_root,
                       std::string db_suffix, bool allow_unsafe_self_blocks_resync);

 private:
  std::unique_ptr<overlay::Overlays::Callback> make_callback() {
    class Callback : public overlay::Overlays::Callback {
     public:
      void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id,
                           td::BufferSlice data) override {
        td::actor::send_closure(id_, &CatChainReceiverImpl::receive_message_from_overlay, src, std::move(data));
      }
      void receive_query(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        td::actor::send_closure(id_, &CatChainReceiverImpl::receive_query_from_overlay, src, std::move(data),
                                std::move(promise));
      }

      void receive_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
        td::actor::send_closure(id_, &CatChainReceiverImpl::receive_broadcast_from_overlay, src, std::move(data));
      }
      explicit Callback(td::actor::ActorId<CatChainReceiverImpl> id) : id_(std::move(id)) {
      }

     private:
      td::actor::ActorId<CatChainReceiverImpl> id_;
    };

    return std::make_unique<Callback>(actor_id(this));
  }

  struct PendingBlock {
    td::BufferSlice payload_;
    std::vector<CatChainBlockHash> deps_;

    PendingBlock(td::BufferSlice &&payload, std::vector<CatChainBlockHash> &&deps)
        : payload_(std::move(payload)), deps_(std::move(deps)) {
    }
  };

  std::list<std::unique_ptr<PendingBlock>> pending_blocks_;
  bool active_send_ = false;
  bool read_db_ = false;
  td::uint32 pending_in_db_ = 0;
  CatChainBlockHash db_root_block_ = CatChainBlockHash::zero();

  void choose_neighbours();

  std::vector<std::unique_ptr<CatChainReceiverSource>> sources_;
  std::map<PublicKeyHash, td::uint32> sources_hashes_;
  std::map<adnl::AdnlNodeIdShort, td::uint32> sources_adnl_addrs_;
  td::uint32 total_forks_ = 0;
  std::map<CatChainBlockHash, std::unique_ptr<CatChainReceivedBlock>> blocks_;
  CatChainReceivedBlock *root_block_;
  CatChainReceivedBlock *last_sent_block_;

  CatChainSessionId incarnation_{};

  std::unique_ptr<Callback> callback_;
  CatChainOptions opts_;

  std::vector<td::uint32> neighbours_;

  //std::queue<tl_object_ptr<ton_api::catchain_block_inner_Data>> events_;
  //std::queue<td::BufferSlice> raw_events_;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<overlay::Overlays> overlay_manager_;
  overlay::OverlayIdShort overlay_id_;
  overlay::OverlayIdFull overlay_full_id_;
  PublicKeyHash local_id_;
  td::uint32 local_idx_;

  td::Timestamp next_sync_;
  td::Timestamp next_rotate_;

  std::string db_root_;
  std::string db_suffix_;

  using DbType = td::KeyValueAsync<CatChainBlockHash, td::BufferSlice>;
  DbType db_;

  bool intentional_fork_ = false;
  td::Timestamp initial_sync_complete_at_{td::Timestamp::never()};
  bool allow_unsafe_self_blocks_resync_{false};
  bool unsafe_root_block_writing_{false};
  bool started_{false};

  std::list<CatChainReceivedBlock *> to_run_;
};

}  // namespace catchain

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChainReceiverImpl *catchain) {
  sb << catchain->print_id();
  return sb;
}

}  // namespace td
