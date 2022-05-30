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

#include "td/actor/actor.h"
#include "td/utils/BufferedUdp.h"

#include "adnl.h"
#include "utils.hpp"
#include "adnl/adnl-query.h"
#include "adnl/adnl-db.h"
#include "common/io.hpp"

#include "adnl-packet.h"

#include "auto/tl/ton_api.h"

namespace ton {

namespace adnl {

constexpr int VERBOSITY_NAME(ADNL_ERROR) = verbosity_WARNING;
constexpr int VERBOSITY_NAME(ADNL_WARNING) = verbosity_INFO;
constexpr int VERBOSITY_NAME(ADNL_NOTICE) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(ADNL_INFO) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(ADNL_DEBUG) = verbosity_DEBUG + 1;
constexpr int VERBOSITY_NAME(ADNL_EXTRA_DEBUG) = verbosity_DEBUG + 10;

class AdnlChannelIdShortImpl {
 public:
  explicit AdnlChannelIdShortImpl(PublicKeyHash value) {
    value_ = value.bits256_value();
  }
  explicit AdnlChannelIdShortImpl(td::Bits256 value) {
    value_ = value;
  }
  AdnlChannelIdShortImpl() {
  }
  td::Bits256 bits256_value() const {
    return value_;
  }
  auto tl() const {
    return value_;
  }

  bool operator<(const AdnlChannelIdShortImpl &with) const {
    return value_ < with.value_;
  }
  bool operator==(const AdnlChannelIdShortImpl &with) const {
    return value_ == with.value_;
  }
  bool operator!=(const AdnlChannelIdShortImpl &with) const {
    return value_ != with.value_;
  }
  td::Slice as_slice() const {
    return td::as_slice(value_);
  }

 private:
  td::Bits256 value_;
};

using AdnlChannelIdShort = AdnlChannelIdShortImpl;

class AdnlLocalId;
class AdnlChannel;

class AdnlPeerTable : public Adnl {
 public:
  static constexpr double republish_addr_list_timeout() {
    return 60.0;
  }

  virtual void answer_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, AdnlQueryId query_id, td::BufferSlice data) = 0;

  virtual void receive_packet(td::IPAddress addr, AdnlCategoryMask cat_mask, td::BufferSlice data) = 0;
  virtual void receive_decrypted_packet(AdnlNodeIdShort dst, AdnlPacket packet) = 0;
  virtual void send_message_in(AdnlNodeIdShort src, AdnlNodeIdShort dst, AdnlMessage message, td::uint32 flags) = 0;

  virtual void register_channel(AdnlChannelIdShort id, AdnlNodeIdShort local_id,
                                td::actor::ActorId<AdnlChannel> channel) = 0;
  virtual void unregister_channel(AdnlChannelIdShort id) = 0;

  virtual void add_static_node(AdnlNode node) = 0;
  virtual void del_static_node(AdnlNodeIdShort id) = 0;
  virtual void get_static_node(AdnlNodeIdShort id, td::Promise<AdnlNode> promise) = 0;

  virtual void write_new_addr_list_to_db(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, AdnlDbItem node,
                                         td::Promise<td::Unit> promise) = 0;
  virtual void get_addr_list_from_db(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id,
                                     td::Promise<AdnlDbItem> promise) = 0;

  virtual void deliver(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data) = 0;
  virtual void deliver_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data,
                             td::Promise<td::BufferSlice> promise) = 0;
  virtual void decrypt_message(AdnlNodeIdShort dst, td::BufferSlice data, td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_conn_ip_str(AdnlNodeIdShort l_id, AdnlNodeIdShort p_id, td::Promise<td::string> promise) = 0;
};

}  // namespace adnl

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &stream, const ton::adnl::AdnlChannelIdShort &value) {
  return stream << value.bits256_value();
}

}  // namespace td
