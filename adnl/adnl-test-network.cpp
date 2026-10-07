/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "adnl-test-network.h"

#include "adnl-test-loopback-implementation.h"
#include "td/utils/port/path.h"

namespace ton::adnl {

void TestNetworkNode::install_callback(std::unique_ptr<Callback> callback) {
  CHECK(!callback_);
  callback_ = std::move(callback);
}

void TestNetworkNode::add_self_addr(td::IPAddress, AdnlCategoryMask, td::uint32) {
}

void TestNetworkNode::add_proxy_addr(td::IPAddress, td::uint16, std::shared_ptr<AdnlProxy>, AdnlCategoryMask,
                                     td::uint32) {
}

void TestNetworkNode::send_udp_packet(AdnlNodeIdShort, AdnlNodeIdShort dst_id, td::IPAddress dst_addr, td::uint32,
                                      td::BufferSlice data) {
  router_->route_packet(dst_id, dst_addr, std::move(data));
}

void TestNetworkNode::set_local_id_category(AdnlNodeIdShort, td::uint8) {
}

void TestNetworkNode::deliver(td::IPAddress addr, td::BufferSlice data) {
  CHECK(callback_);
  AdnlCategoryMask m;
  m[0] = true;
  callback_->receive_packet(addr, std::move(m), std::move(data));
}

TestNetwork::TestNetwork(const std::string& db_root) : db_root_(db_root) {
  td::rmrf(db_root_).ignore();
  td::mkdir(db_root_).ensure();
}

TestNetwork::~TestNetwork() {
  nodes_.clear();
  td::rmrf(db_root_).ignore();
}

TestNetwork::Node& TestNetwork::add_node() {
  PrivateKey pk{privkeys::Ed25519::random()};
  auto node_db = db_root_ + "/node-" + std::to_string(nodes_.size());
  td::mkdir(node_db).ensure();

  auto node = std::make_unique<Node>(pk.compute_public_key(), node_db);

  node->keyring = keyring::Keyring::create(node_db);
  td::actor::send_closure(node->keyring, &keyring::Keyring::add_key, std::move(pk), true, [](td::Result<>) {});

  node->network_manager = td::actor::create_actor<TestNetworkNode>("net", this);
  node->adnl = Adnl::create(node_db, node->keyring.get());
  td::actor::send_closure(node->adnl, &Adnl::register_network_manager, node->network_manager.get());

  auto addr = generate_dummy_addr_list();
  td::actor::send_closure(node->adnl, &Adnl::add_id, node->adnl_full, addr, static_cast<td::uint8>(0));

  routing_table_[node->adnl_short] = node->network_manager.get();

  auto& ref = *node;
  nodes_.push_back(std::move(node));
  return ref;
}

void TestNetwork::connect(Node& a, Node& b) {
  auto addr = generate_dummy_addr_list();
  td::actor::send_closure(a.adnl, &Adnl::add_peer, a.adnl_short, b.adnl_full, addr);
  td::actor::send_closure(b.adnl, &Adnl::add_peer, b.adnl_short, a.adnl_full, addr);
}

void TestNetwork::route_packet(AdnlNodeIdShort dst_id, td::IPAddress dst_addr, td::BufferSlice data) {
  auto it = routing_table_.find(dst_id);
  if (it != routing_table_.end()) {
    td::actor::send_closure(it->second, &TestNetworkNode::deliver, dst_addr, std::move(data));
  }
}

AdnlAddressList TestNetwork::generate_dummy_addr_list() {
  return TestLoopbackNetworkManager::generate_dummy_addr_list();
}

}  // namespace ton::adnl
