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

#include "adnl/adnl.h"
#include "td/utils/Random.h"

#include <set>

namespace ton {

namespace adnl {

class TestLoopbackNetworkManager : public ton::adnl::AdnlNetworkManager {
 public:
  void install_callback(std::unique_ptr<Callback> callback) override {
    CHECK(!callback_);
    callback_ = std::move(callback);
  }

  void add_self_addr(td::IPAddress addr, AdnlCategoryMask cat_mask, td::uint32 priority) override {
  }
  void add_proxy_addr(td::IPAddress addr, td::uint16 local_port, std::shared_ptr<AdnlProxy> proxy,
                      AdnlCategoryMask cat_mask, td::uint32 priority) override {
  }
  void send_udp_packet(ton::adnl::AdnlNodeIdShort src_id, ton::adnl::AdnlNodeIdShort dst_id, td::IPAddress dst_addr,
                       td::uint32 priority, td::BufferSlice data) override {
    if (allowed_sources_.count(src_id) == 0 || allowed_destinations_.count(dst_id) == 0) {
      // just drop
      return;
    }
    if (loss_probability_ > 0 && td::Random::fast(0, 10000) < loss_probability_ * 10000) {
      return;
    }
    CHECK(callback_);
    AdnlCategoryMask m;
    m[0] = true;
    callback_->receive_packet(dst_addr, std::move(m), std::move(data));
  }

  void add_node_id(AdnlNodeIdShort id, bool allow_send, bool allow_receive) {
    if (allow_send) {
      allowed_sources_.insert(id);
    } else {
      allowed_sources_.erase(id);
    }
    if (allow_receive) {
      allowed_destinations_.insert(id);
    } else {
      allowed_destinations_.erase(id);
    }
  }

  void set_loss_probability(double p) {
    CHECK(p >= 0 && p <= 1);
    loss_probability_ = p;
  }
  void set_local_id_category(AdnlNodeIdShort id, td::uint8 cat) override {
  }

  TestLoopbackNetworkManager() {
  }

  static AdnlAddressList generate_dummy_addr_list(bool empty = false);

 private:
  std::set<AdnlNodeIdShort> allowed_sources_;
  std::set<AdnlNodeIdShort> allowed_destinations_;
  std::unique_ptr<Callback> callback_;
  double loss_probability_ = 0.0;
};

}  // namespace adnl

}  // namespace ton
