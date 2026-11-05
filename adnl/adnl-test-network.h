/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include <map>

#include "adnl/adnl-network-manager.h"
#include "adnl/adnl.h"

namespace ton::adnl {

class TestNetworkNode : public AdnlNetworkManager {
 public:
  class Router {
   public:
    virtual ~Router() = default;
    virtual void route_packet(AdnlNodeIdShort dst_id, td::IPAddress dst_addr, td::BufferSlice data) = 0;
  };

  explicit TestNetworkNode(Router* router) : router_(router) {
  }

  void install_callback(std::unique_ptr<Callback> callback) override;
  void add_self_addr(td::IPAddress addr, AdnlCategoryMask cat_mask, td::uint32 priority) override;
  void add_proxy_addr(td::IPAddress addr, td::uint16 local_port, std::shared_ptr<AdnlProxy> proxy,
                      AdnlCategoryMask cat_mask, td::uint32 priority) override;
  void send_udp_packet(AdnlNodeIdShort src_id, AdnlNodeIdShort dst_id, td::IPAddress dst_addr, td::uint32 priority,
                       td::BufferSlice data) override;
  void set_local_id_category(AdnlNodeIdShort id, td::uint8 cat) override;

  void deliver(td::IPAddress addr, td::BufferSlice data);

 private:
  Router* router_;
  std::unique_ptr<Callback> callback_;
};

// A test network that connects multiple independent ADNL nodes.
// Each call to add_node() creates a fresh AdnlNetworkManager + Adnl pair.
class TestNetwork : public TestNetworkNode::Router {
 public:
  struct Node {
    td::actor::ActorOwn<keyring::Keyring> keyring;
    td::actor::ActorOwn<TestNetworkNode> network_manager;
    td::actor::ActorOwn<Adnl> adnl;
    PublicKey pub;
    AdnlNodeIdFull adnl_full;
    AdnlNodeIdShort adnl_short;
    std::string db_root;

    explicit Node(PublicKey pub_key, std::string db)
        : pub(std::move(pub_key)), adnl_full(pub), adnl_short(pub.compute_short_id()), db_root(std::move(db)) {
    }
  };

  explicit TestNetwork(const std::string& db_root);
  ~TestNetwork();

  Node& add_node();

  // Introduce two nodes so they know each other's ADNL addresses.
  void connect(Node& a, Node& b);

  void route_packet(AdnlNodeIdShort dst_id, td::IPAddress dst_addr, td::BufferSlice data) override;

  static AdnlAddressList generate_dummy_addr_list();

 private:
  std::string db_root_;
  std::vector<std::unique_ptr<Node>> nodes_;
  std::map<AdnlNodeIdShort, td::actor::ActorId<TestNetworkNode>> routing_table_;
};

}  // namespace ton::adnl
