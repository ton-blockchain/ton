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
#include "adnl-static-nodes.h"
#include "adnl-static-nodes.hpp"

#include "utils.hpp"

namespace ton {

namespace adnl {

void AdnlStaticNodesManagerImpl::add_node(AdnlNode node) {
  auto id_short = node.compute_short_id();
  VLOG(ADNL_INFO) << "[staticnodes] adding static node " << id_short;

  nodes_.emplace(id_short, std::move(node));
}

void AdnlStaticNodesManagerImpl::del_node(AdnlNodeIdShort id) {
  nodes_.erase(id);
}

td::Result<AdnlNode> AdnlStaticNodesManagerImpl::get_node(AdnlNodeIdShort id) {
  auto it = nodes_.find(id);
  if (it == nodes_.end()) {
    return td::Status::Error(ErrorCode::notready, "static node not found");
  }

  return it->second;
}

td::actor::ActorOwn<AdnlStaticNodesManager> AdnlStaticNodesManager::create() {
  auto X = td::actor::create_actor<AdnlStaticNodesManagerImpl>("staticnodesmanager");
  return td::actor::ActorOwn<AdnlStaticNodesManager>(std::move(X));
}

}  // namespace adnl

}  // namespace ton
