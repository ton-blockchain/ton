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
#include "catchain-receiver-source.hpp"
#include "common/errorlog.h"

namespace ton {

namespace catchain {

td::uint32 CatChainReceiverSourceImpl::add_fork() {
  if (!fork_ids_.empty()) {
    blame();
  }
  td::uint32 F = chain_->add_fork();
  CHECK(F > 0);

  fork_ids_.push_back(F);

  VLOG(CATCHAIN_INFO) << this << ": adding new fork " << F << " of source " << id_;

  if (fork_ids_.size() > 1) {
    CHECK(blamed());
  }

  return F;
}

CatChainReceiverSourceImpl::CatChainReceiverSourceImpl(CatChainReceiver *chain, PublicKey source,
                                                       adnl::AdnlNodeIdShort adnl_id, td::uint32 id)
    : chain_(chain), id_(id), adnl_id_(adnl_id) {
  src_ = source.compute_short_id();

  encryptor_ = source.create_encryptor_async().move_as_ok();
  encryptor_sync_ = source.create_encryptor().move_as_ok();
  full_id_ = std::move(source);
}

td::Result<std::unique_ptr<CatChainReceiverSource>> CatChainReceiverSource::create(CatChainReceiver *chain,
                                                                                   PublicKey source,
                                                                                   adnl::AdnlNodeIdShort adnl_id,
                                                                                   td::uint32 id) {
  return std::make_unique<CatChainReceiverSourceImpl>(chain, std::move(source), adnl_id, id);
}

void CatChainReceiverSourceImpl::blame(td::uint32 fork, CatChainBlockHeight height) {
  blame();
  if (!blamed_heights_.empty()) {
    if (blamed_heights_.size() <= fork) {
      blamed_heights_.resize(fork + 1, 0);
    }
    if (blamed_heights_[fork] == 0 || blamed_heights_[fork] > height) {
      VLOG(CATCHAIN_INFO) << this << ": blamed at " << fork << " " << height;
      blamed_heights_[fork] = height;
    }
  }
}

void CatChainReceiverSourceImpl::blame() {
  if (!blamed_) {
    LOG(ERROR) << this << ": CATCHAIN: blaming source " << id_;
    blocks_.clear();
    delivered_height_ = 0;
    chain_->on_blame(id_);
  }
  blamed_ = true;
}

CatChainReceivedBlock *CatChainReceiverSourceImpl::get_block(CatChainBlockHeight height) const {
  auto it = blocks_.find(height);
  if (it != blocks_.end()) {
    return it->second;
  } else {
    return nullptr;
  }
}

void CatChainReceiverSourceImpl::block_received(CatChainBlockHeight height) {
  if (blamed()) {
    return;
  }

  if (received_height_ + 1 == height) {
    received_height_ = height;
  }
  while (true) {
    auto it = blocks_.find(received_height_ + 1);
    if (it == blocks_.end()) {
      return;
    }
    if (!it->second->initialized()) {
      return;
    }
    received_height_++;
  }
}

void CatChainReceiverSourceImpl::block_delivered(CatChainBlockHeight height) {
  if (blamed()) {
    return;
  }

  if (delivered_height_ + 1 == height) {
    delivered_height_ = height;
  }
  while (true) {
    auto it = blocks_.find(delivered_height_ + 1);
    if (it == blocks_.end()) {
      return;
    }
    if (!it->second->delivered()) {
      return;
    }
    delivered_height_++;
  }
}

void CatChainReceiverSourceImpl::on_new_block(CatChainReceivedBlock *block) {
  if (fork_is_found()) {
    return;
  }

  CHECK(block->get_source_id() == id_);
  auto it = blocks_.find(block->get_height());
  if (it != blocks_.end()) {
    CHECK(block->get_hash() != it->second->get_hash());
    VLOG(CATCHAIN_WARNING) << this << ": found fork on height " << block->get_height();
    if (!fork_is_found()) {
      on_found_fork_proof(create_serialize_tl_object<ton_api::catchain_block_data_fork>(block->export_tl_dep(),
                                                                                        it->second->export_tl_dep())
                              .as_slice());
      chain_->add_prepared_event(fork_proof());
    }
    blame();
    return;
  }
  blocks_[block->get_height()] = block;
}

void CatChainReceiverSourceImpl::on_found_fork_proof(const td::Slice &proof) {
  if (!fork_is_found()) {
    fetch_tl_object<ton_api::catchain_block_data_fork>(proof, true).ensure();
    fork_proof_ = td::SharedSlice{proof};
    errorlog::ErrorLog::log(PSTRING() << "catchain " << chain_->get_incarnation() << " source " << id_
                                      << " found fork. hash=" << sha256_bits256(fork_proof_.as_slice()).to_hex());
    errorlog::ErrorLog::log_file(fork_proof_.clone_as_buffer_slice());
  }
}

}  // namespace catchain

}  // namespace ton
