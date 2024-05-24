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

#include <map>

#include "catchain.h"
#include "catchain-types.h"
#include "catchain-receiver-interface.h"
#include "td/utils/DecTree.h"

namespace ton {

namespace catchain {

class CatChainImpl : public CatChain {
 private:
  std::unique_ptr<CatChain::Callback> callback_;
  CatChainOptions opts_;
  td::DecTree<CatChainBlockHash, CatChainBlock *> top_blocks_;
  std::map<CatChainBlockHash, std::unique_ptr<CatChainBlock>> blocks_;
  std::vector<CatChainBlock *> top_source_blocks_;

  std::vector<PublicKeyHash> sources_;
  std::vector<bool> blamed_sources_;

  std::vector<CatChainBlockHash> process_deps_;

  CatChainSessionId unique_hash_;
  td::uint32 local_idx_;
  bool active_process_ = false;
  bool force_process_ = false;
  td::actor::ActorOwn<CatChainReceiverInterface> receiver_;

  bool receiver_started_ = false;

  std::string db_root_;
  std::string db_suffix_;
  bool allow_unsafe_self_blocks_resync_;

  void send_process();
  void send_preprocess(CatChainBlock *block);
  void set_processed(CatChainBlock *block);

  struct Args {
    td::actor::ActorId<keyring::Keyring> keyring;
    td::actor::ActorId<adnl::Adnl> adnl;
    td::actor::ActorId<overlay::Overlays> overlay_manager;
    std::vector<CatChainNode> ids;
    PublicKeyHash local_id;
    CatChainSessionId unique_hash;

    Args(td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
         td::actor::ActorId<overlay::Overlays> overlay_manager, std::vector<CatChainNode> ids,
         const PublicKeyHash &local_id, const CatChainSessionId &unique_hash)
        : keyring(std::move(keyring))
        , adnl(std::move(adnl))
        , overlay_manager(std::move(overlay_manager))
        , ids(std::move(ids))
        , local_id(local_id)
        , unique_hash(unique_hash) {
    }
  };
  std::unique_ptr<Args> args_;

 public:
  PrintId print_id() const override {
    return PrintId{unique_hash_, sources_[local_idx_]};
  }
  CatChainBlock *get_block(CatChainBlockHash hash) const;
  void on_new_block(td::uint32 src_id, td::uint32 fork, CatChainBlockHash hash, CatChainBlockHeight height,
                    CatChainBlockHash prev, std::vector<CatChainBlockHash> deps, std::vector<CatChainBlockHeight> vt,
                    td::SharedSlice data);
  void on_blame(td::uint32 src_id);
  void on_custom_query(const PublicKeyHash &src, td::BufferSlice data, td::Promise<td::BufferSlice> promise);
  void on_broadcast(const PublicKeyHash &src, td::BufferSlice data);
  void on_receiver_started();
  void processed_block(td::BufferSlice payload) override;
  void need_new_block(td::Timestamp t) override;
  void debug_add_fork(td::BufferSlice payload, CatChainBlockHeight height) override {
    td::actor::send_closure(receiver_, &CatChainReceiverInterface::debug_add_fork, std::move(payload), height,
                            std::vector<CatChainBlockHash>{});
  }

  void send_broadcast(td::BufferSlice data) override {
    td::actor::send_closure(receiver_, &CatChainReceiverInterface::send_fec_broadcast, std::move(data));
  }
  void send_message(const PublicKeyHash &dst, td::BufferSlice data) override {
    td::actor::send_closure(receiver_, &CatChainReceiverInterface::send_custom_message_data, dst, std::move(data));
  }
  void send_query(const PublicKeyHash &dst, std::string name, td::Promise<td::BufferSlice> promise,
                  td::Timestamp timeout, td::BufferSlice query) override {
    td::actor::send_closure(receiver_, &CatChainReceiverInterface::send_custom_query_data, dst, name,
                            std::move(promise), timeout, std::move(query));
  }
  void send_query_via(const PublicKeyHash &dst, std::string name, td::Promise<td::BufferSlice> promise,
                      td::Timestamp timeout, td::BufferSlice query, td::uint64 max_answer_size,
                      td::actor::ActorId<adnl::AdnlSenderInterface> via) override {
    td::actor::send_closure(receiver_, &CatChainReceiverInterface::send_custom_query_data_via, dst, name,
                            std::move(promise), timeout, std::move(query), max_answer_size, via);
  }
  void destroy() override;
  CatChainImpl(std::unique_ptr<Callback> callback, const CatChainOptions &opts,
               td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
               td::actor::ActorId<overlay::Overlays> overlay_manager, std::vector<CatChainNode> ids,
               const PublicKeyHash &local_id, const CatChainSessionId &unique_hash, std::string db_root,
               std::string db_suffix, bool allow_unsafe_self_blocks_resync);

  void alarm() override;
  void start_up() override;
};

}  // namespace catchain

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChainImpl *catchain) {
  sb << catchain->print_id();
  return sb;
}

}  // namespace td
