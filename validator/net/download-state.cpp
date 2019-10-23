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

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "download-state.hpp"
#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"
#include "td/utils/overloaded.h"
#include "full-node.h"

namespace ton {

namespace validator {

namespace fullnode {

DownloadState::DownloadState(BlockIdExt block_id, BlockIdExt masterchain_block_id, adnl::AdnlNodeIdShort local_id,
                             overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort download_from,
                             td::uint32 priority, td::Timestamp timeout,
                             td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                             td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
                             td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<adnl::AdnlExtClient> client,
                             td::Promise<td::BufferSlice> promise)
    : block_id_(block_id)
    , masterchain_block_id_(masterchain_block_id)
    , local_id_(local_id)
    , overlay_id_(overlay_id)
    , download_from_(download_from)
    , priority_(priority)
    , timeout_(timeout)
    , validator_manager_(validator_manager)
    , rldp_(rldp)
    , overlays_(overlays)
    , adnl_(adnl)
    , client_(client)
    , promise_(std::move(promise)) {
}

void DownloadState::abort_query(td::Status reason) {
  if (promise_) {
    if (reason.code() == ErrorCode::notready || reason.code() == ErrorCode::timeout) {
      VLOG(FULL_NODE_DEBUG) << "failed to download state " << block_id_ << "from " << download_from_ << ": " << reason;
    } else {
      VLOG(FULL_NODE_NOTICE) << "failed to download state " << block_id_ << " from " << download_from_ << ": "
                             << reason;
    }
    promise_.set_error(std::move(reason));
  }
  stop();
}

void DownloadState::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void DownloadState::finish_query() {
  if (promise_) {
    promise_.set_value(std::move(state_));
  }
  stop();
}

void DownloadState::start_up() {
  alarm_timestamp() = timeout_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadState::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadState::got_block_handle, R.move_as_ok());
    }
  });

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle, block_id_, true,
                          std::move(P));
}

void DownloadState::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);
  if (!download_from_.is_zero() || !client_.empty()) {
    got_node_to_download(download_from_);
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadState::abort_query, R.move_as_error());
      } else {
        auto vec = R.move_as_ok();
        if (vec.size() == 0) {
          td::actor::send_closure(SelfId, &DownloadState::abort_query,
                                  td::Status::Error(ErrorCode::notready, "no nodes"));
        } else {
          td::actor::send_closure(SelfId, &DownloadState::got_node_to_download, vec[0]);
        }
      }
    });

    td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_, 1,
                            std::move(P));
  }
}

void DownloadState::got_node_to_download(adnl::AdnlNodeIdShort node) {
  download_from_ = node;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadState::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadState::got_block_state_description, R.move_as_ok());
    }
  });

  td::BufferSlice query;
  if (masterchain_block_id_.is_valid()) {
    query = create_serialize_tl_object<ton_api::tonNode_preparePersistentState>(
        create_tl_block_id(block_id_), create_tl_block_id(masterchain_block_id_));
  } else {
    query = create_serialize_tl_object<ton_api::tonNode_prepareZeroState>(create_tl_block_id(block_id_));
  }

  if (client_.empty()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query, download_from_, local_id_, overlay_id_,
                            "get_prepare", std::move(P), td::Timestamp::in(1.0), std::move(query));
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_prepare",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(query)),
                            td::Timestamp::in(1.0), std::move(P));
  }
}

void DownloadState::got_block_state_description(td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::tonNode_PreparedState>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  ton_api::downcast_call(
      *F.move_as_ok().get(),
      td::overloaded(
          [&](ton_api::tonNode_notFoundState &f) {
            abort_query(td::Status::Error(ErrorCode::notready, "state not found"));
          },
          [&, self = this](ton_api::tonNode_preparedState &f) {
            if (masterchain_block_id_.is_valid()) {
              got_block_state_part(td::BufferSlice{}, 0);
              return;
            }
            auto P = td::PromiseCreator::lambda([SelfId = actor_id(self)](td::Result<td::BufferSlice> R) {
              if (R.is_error()) {
                td::actor::send_closure(SelfId, &DownloadState::abort_query, R.move_as_error());
              } else {
                td::actor::send_closure(SelfId, &DownloadState::got_block_state, R.move_as_ok());
              }
            });

            td::BufferSlice query =
                create_serialize_tl_object<ton_api::tonNode_downloadZeroState>(create_tl_block_id(block_id_));
            if (client_.empty()) {
              td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, download_from_, local_id_,
                                      overlay_id_, "download state", std::move(P), td::Timestamp::in(3.0),
                                      std::move(query), FullNode::max_state_size(), rldp_);
            } else {
              td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "download state",
                                      create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(query)),
                                      td::Timestamp::in(3.0), std::move(P));
            }
          }));
}

void DownloadState::got_block_state_part(td::BufferSlice data, td::uint32 requested_size) {
  bool last_part = data.size() < requested_size;
  sum_ += data.size();
  parts_.push_back(std::move(data));

  if (last_part) {
    td::BufferSlice res{td::narrow_cast<std::size_t>(sum_)};
    auto S = res.as_slice();
    for (auto &p : parts_) {
      S.copy_from(p.as_slice());
      S.remove_prefix(p.size());
    }
    parts_.clear();
    CHECK(!S.size());
    got_block_state(std::move(res));
    return;
  }

  td::uint32 part_size = 1 << 18;
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), part_size](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadState::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadState::got_block_state_part, R.move_as_ok(), part_size);
    }
  });

  td::BufferSlice query = create_serialize_tl_object<ton_api::tonNode_downloadPersistentStateSlice>(
      create_tl_block_id(block_id_), create_tl_block_id(masterchain_block_id_), sum_, part_size);
  if (client_.empty()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, download_from_, local_id_, overlay_id_,
                            "download state", std::move(P), td::Timestamp::in(10.0), std::move(query),
                            FullNode::max_state_size(), rldp_);
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "download state",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(query)),
                            td::Timestamp::in(10.0), std::move(P));
  }
}

void DownloadState::got_block_state(td::BufferSlice data) {
  state_ = std::move(data);
  finish_query();
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
