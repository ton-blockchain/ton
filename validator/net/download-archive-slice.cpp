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
#include <ton/ton-tl.hpp>

#include "td/utils/overloaded.h"
#include "td/utils/port/path.h"
#include "ton/ton-io.hpp"

#include "download-archive-slice.hpp"

namespace ton {

namespace validator {

namespace fullnode {

DownloadArchiveSlice::DownloadArchiveSlice(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                                           QuerySender query_sender, td::Timestamp timeout,
                                           td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                                           td::Promise<std::string> promise)
    : masterchain_seqno_(masterchain_seqno)
    , shard_prefix_(shard_prefix)
    , tmp_dir_(std::move(tmp_dir))
    , query_sender_(std::move(query_sender))
    , timeout_(timeout)
    , validator_manager_(validator_manager)
    , promise_(std::move(promise)) {
}

void DownloadArchiveSlice::abort_query(td::Status reason) {
  if (promise_) {
    VLOG(full_node, WARNING) << "failed to download archive slice #" << masterchain_seqno_ << " " << shard_prefix_
                             << " from " << query_sender_->to_str() << ": " << reason;
    promise_.set_error(std::move(reason));
    if (!fd_.empty()) {
      td::unlink(tmp_name_).ensure();
      fd_.close();
    }
  }
  stop();
}

void DownloadArchiveSlice::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void DownloadArchiveSlice::finish_query() {
  if (promise_) {
    promise_.set_value(std::move(tmp_name_));
    fd_.close();
  }
  stop();
}

void DownloadArchiveSlice::start_up() {
  alarm_timestamp() = timeout_;
  VLOG(full_node, INFO) << "downloading archive slice #" << masterchain_seqno_ << " " << shard_prefix_ << " from "
                        << query_sender_->to_str();

  auto R = td::mkstemp(tmp_dir_);
  if (R.is_error()) {
    abort_query(R.move_as_error_prefix("failed to open temp file: "));
    return;
  }
  auto r = R.move_as_ok();
  fd_ = std::move(r.first);
  tmp_name_ = std::move(r.second);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_archive_info, R.move_as_ok());
    }
  });

  td::BufferSlice q;
  if (shard_prefix_.is_masterchain()) {
    q = create_serialize_tl_object<ton_api::tonNode_getArchiveInfo>(masterchain_seqno_);
  } else {
    q = create_serialize_tl_object<ton_api::tonNode_getShardArchiveInfo>(masterchain_seqno_,
                                                                         create_tl_shard_id(shard_prefix_));
  }
  query_sender_->send_query(std::move(q), td::Timestamp::in(3.0), 1024, std::move(P));
}

void DownloadArchiveSlice::got_archive_info(td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::tonNode_ArchiveInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error_prefix("failed to parse ArchiveInfo answer"));
    return;
  }
  auto f = F.move_as_ok();

  bool fail = false;
  ton_api::downcast_call(*f.get(), td::overloaded(
                                       [&](const ton_api::tonNode_archiveNotFound &obj) {
                                         abort_query(td::Status::Error(ErrorCode::notready, "remote db not found"));
                                         fail = true;
                                       },
                                       [&](const ton_api::tonNode_archiveInfo &obj) { archive_id_ = obj.id_; }));
  if (fail) {
    return;
  }

  prev_logged_timer_ = td::Timer();
  get_archive_slice();
}

void DownloadArchiveSlice::get_archive_slice() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_archive_slice, R.move_as_ok());
    }
  });

  query_sender_->send_query(
      create_serialize_tl_object<ton_api::tonNode_getArchiveSlice>(archive_id_, offset_, slice_size()),
      td::Timestamp::in(15.0), slice_size() + 1024, std::move(P));
}

void DownloadArchiveSlice::got_archive_slice(td::BufferSlice data) {
  auto R = fd_.write(data.as_slice());
  if (R.is_error()) {
    abort_query(R.move_as_error_prefix("failed to write temp file: "));
    return;
  }
  if (R.move_as_ok() != data.size()) {
    abort_query(td::Status::Error(ErrorCode::error, "short write to temp file"));
    return;
  }

  offset_ += data.size();

  double elapsed = prev_logged_timer_.elapsed();
  if (elapsed > 10.0) {
    prev_logged_timer_ = td::Timer();
    VLOG(full_node, INFO) << "downloading archive slice #" << masterchain_seqno_ << " " << shard_prefix_
                          << ": total=" << offset_ << " ("
                          << td::format::as_size((td::uint64)(double(offset_ - prev_logged_sum_) / elapsed)) << "/s)";
    prev_logged_sum_ = offset_;
  }

  if (data.size() < slice_size()) {
    VLOG(full_node, INFO) << "finished downloading archive slice #" << masterchain_seqno_ << " " << shard_prefix_
                          << " from " << query_sender_->to_str() << ": total=" << offset_;
    finish_query();
  } else {
    get_archive_slice();
  }
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
