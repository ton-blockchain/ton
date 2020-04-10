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
#include "adnl-db.hpp"
#include "td/db/RocksDb.h"

namespace ton {

namespace adnl {

void AdnlDbImpl::update(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, AdnlDbItem item,
                        td::Promise<td::Unit> promise) {
  td::BufferSlice b{64};
  auto S = b.as_slice();
  S.copy_from(local_id.as_slice());
  S.remove_prefix(32);
  S.copy_from(peer_id.as_slice());

  auto obj = create_tl_object<ton_api::adnl_db_node_value>(static_cast<td::int32>(td::Clocks::system()), item.id.tl(),
                                                           item.addr_list.tl(), item.priority_addr_list.tl());

  kv_->begin_transaction().ensure();
  kv_->set(b.as_slice(), serialize_tl_object(obj, true).as_slice()).ensure();
  kv_->commit_transaction().ensure();
}

void AdnlDbImpl::get(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, td::Promise<AdnlDbItem> promise) {
  td::BufferSlice b{64};
  auto S = b.as_slice();
  S.copy_from(local_id.as_slice());
  S.remove_prefix(32);
  S.copy_from(peer_id.as_slice());

  std::string value;
  auto R = kv_->get(b.as_slice(), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in db"));
    return;
  }
  auto F = fetch_tl_object<ton_api::adnl_db_node_value>(td::BufferSlice{value}, true);
  F.ensure();
  auto f = F.move_as_ok();
  AdnlDbItem n;
  auto id = AdnlNodeIdFull::create(f->id_);
  id.ensure();
  n.id = id.move_as_ok();
  auto addr_list = AdnlAddressList::create(std::move(f->addr_list_));
  addr_list.ensure();
  n.addr_list = addr_list.move_as_ok();
  auto priority_addr_list = AdnlAddressList::create(std::move(f->priority_addr_list_));
  priority_addr_list.ensure();
  n.priority_addr_list = priority_addr_list.move_as_ok();
  promise.set_value(std::move(n));
}

void AdnlDbImpl::start_up() {
  kv_ = std::make_shared<td::RocksDb>(td::RocksDb::open(path_).move_as_ok());
}

td::actor::ActorOwn<AdnlDb> AdnlDb::create(std::string path) {
  return td::actor::create_actor<AdnlDbImpl>("adnldb", path);
}

}  // namespace adnl

}  // namespace ton
