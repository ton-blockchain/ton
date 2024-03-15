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
#include "adnl/adnl-query.h"

namespace ton {

namespace adnl {

namespace adnlmessage {

class AdnlMessageCreateChannel {
 public:
  AdnlMessageCreateChannel(pubkeys::Ed25519 key, td::int32 date) : key_(key), date_(date) {
  }
  const auto &key() const {
    return key_;
  }
  auto date() const {
    return date_;
  }
  td::uint32 size() const {
    return 40;
  }
  tl_object_ptr<ton_api::adnl_Message> tl() const {
    return create_tl_object<ton_api::adnl_message_createChannel>(key_.raw(), date_);
  }

 private:
  pubkeys::Ed25519 key_;
  td::int32 date_;
};

class AdnlMessageConfirmChannel {
 public:
  AdnlMessageConfirmChannel(pubkeys::Ed25519 key, pubkeys::Ed25519 peer_key, td::int32 date)
      : key_(key), peer_key_(peer_key), date_(date) {
  }
  const auto &key() const {
    return key_;
  }
  const auto &peer_key() const {
    return peer_key_;
  }
  auto date() const {
    return date_;
  }
  td::uint32 size() const {
    return 72;
  }
  tl_object_ptr<ton_api::adnl_Message> tl() const {
    return create_tl_object<ton_api::adnl_message_confirmChannel>(key_.raw(), peer_key_.raw(), date_);
  }

 private:
  pubkeys::Ed25519 key_;
  pubkeys::Ed25519 peer_key_;
  td::int32 date_;
};

class AdnlMessageCustom {
 public:
  AdnlMessageCustom(td::BufferSlice data) : data_(std::move(data)) {
  }
  auto data() const {
    return data_.clone();
  }
  td::uint32 size() const {
    return static_cast<td::uint32>(data_.size()) + 12;
  }
  tl_object_ptr<ton_api::adnl_Message> tl() const {
    return create_tl_object<ton_api::adnl_message_custom>(data_.clone());
  }

 private:
  td::BufferSlice data_;
};

class AdnlMessageNop {
 public:
  AdnlMessageNop() {
  }
  td::uint32 size() const {
    return 4;
  }
  tl_object_ptr<ton_api::adnl_Message> tl() const {
    return create_tl_object<ton_api::adnl_message_nop>();
  }

 private:
};

class AdnlMessageReinit {
 public:
  AdnlMessageReinit(td::int32 date) : date_(date) {
  }
  auto date() const {
    return date_;
  }
  td::uint32 size() const {
    return 8;
  }
  tl_object_ptr<ton_api::adnl_Message> tl() const {
    return create_tl_object<ton_api::adnl_message_reinit>(date_);
  }

 private:
  td::int32 date_;
};

class AdnlMessageQuery {
 public:
  AdnlMessageQuery(AdnlQueryId query_id, td::BufferSlice data) : query_id_(query_id), data_(std::move(data)) {
  }
  const auto &query_id() const {
    return query_id_;
  }
  auto data() const {
    return data_.clone();
  }
  td::uint32 size() const {
    return static_cast<td::uint32>(data_.size()) + 44;
  }
  tl_object_ptr<ton_api::adnl_Message> tl() const {
    return create_tl_object<ton_api::adnl_message_query>(query_id_, data_.clone());
  }

 private:
  AdnlQueryId query_id_;
  td::BufferSlice data_;
};

class AdnlMessageAnswer {
 public:
  AdnlMessageAnswer(AdnlQueryId query_id, td::BufferSlice data) : query_id_(query_id), data_(std::move(data)) {
  }
  const auto &query_id() const {
    return query_id_;
  }
  auto data() const {
    return data_.clone();
  }
  td::uint32 size() const {
    return static_cast<td::uint32>(data_.size()) + 44;
  }
  tl_object_ptr<ton_api::adnl_Message> tl() const {
    return create_tl_object<ton_api::adnl_message_answer>(query_id_, data_.clone());
  }

 private:
  AdnlQueryId query_id_;
  td::BufferSlice data_;
};

class AdnlMessagePart {
 public:
  AdnlMessagePart(td::Bits256 hash, td::uint32 total_size, td::uint32 offset, td::BufferSlice data)
      : hash_(hash), total_size_(total_size), offset_(offset), data_(std::move(data)) {
  }
  const auto &hash() const {
    return hash_;
  }
  auto offset() const {
    return offset_;
  }
  auto total_size() const {
    return total_size_;
  }
  auto data() const {
    return data_.clone();
  }
  td::uint32 size() const {
    return static_cast<td::uint32>(data_.size()) + 48;
  }
  tl_object_ptr<ton_api::adnl_Message> tl() const {
    return create_tl_object<ton_api::adnl_message_part>(hash_, total_size_, offset_, data_.clone());
  }

 private:
  td::Bits256 hash_;
  td::uint32 total_size_;
  td::uint32 offset_;
  td::BufferSlice data_;
};

}  // namespace adnlmessage

class AdnlMessage {
 public:
  class Empty {
   public:
    Empty() {
    }
    td::uint32 size() const {
      UNREACHABLE();
    }
    tl_object_ptr<ton_api::adnl_Message> tl() const {
      UNREACHABLE();
    }
  };

 private:
  td::Variant<Empty, adnlmessage::AdnlMessageCreateChannel, adnlmessage::AdnlMessageConfirmChannel,
              adnlmessage::AdnlMessageCustom, adnlmessage::AdnlMessageNop, adnlmessage::AdnlMessageReinit,
              adnlmessage::AdnlMessageQuery, adnlmessage::AdnlMessageAnswer, adnlmessage::AdnlMessagePart>
      message_{Empty{}};

 public:
  explicit AdnlMessage(tl_object_ptr<ton_api::adnl_Message> message);
  template <class T>
  AdnlMessage(T m) : message_(std::move(m)) {
  }

  tl_object_ptr<ton_api::adnl_Message> tl() const {
    tl_object_ptr<ton_api::adnl_Message> res;
    message_.visit([&](const auto &obj) { res = obj.tl(); });
    return res;
  }
  td::uint32 size() const {
    td::uint32 res;
    message_.visit([&](const auto &obj) { res = obj.size(); });
    return res;
  }
  template <class F>
  void visit(F &&f) {
    message_.visit(std::move(f));
  }
  template <class F>
  void visit(F &&f) const {
    message_.visit(std::move(f));
  }
};

class OutboundAdnlMessage {
 public:
  template <class T>
  OutboundAdnlMessage(T m, td::uint32 flags) : message_{std::move(m)}, flags_(flags) {
  }
  td::uint32 flags() const {
    return flags_;
  }
  void set_flags(td::uint32 f) {
    flags_ = f;
  }
  tl_object_ptr<ton_api::adnl_Message> tl() const {
    return message_.tl();
  }
  td::uint32 size() const {
    return message_.size();
  }
  template <class F>
  void visit(F &&f) {
    message_.visit(std::move(f));
  }
  template <class F>
  void visit(F &&f) const {
    message_.visit(std::move(f));
  }
  AdnlMessage release() {
    return std::move(message_);
  }

 private:
  AdnlMessage message_;
  td::uint32 flags_;
};

class AdnlMessageList {
 public:
  AdnlMessageList() {
  }
  AdnlMessageList(tl_object_ptr<ton_api::adnl_Message> message) {
    auto msg = AdnlMessage{std::move(message)};
    messages_.emplace_back(std::move(msg));
  }
  AdnlMessageList(std::vector<tl_object_ptr<ton_api::adnl_Message>> messages) {
    for (auto &message : messages) {
      messages_.push_back(AdnlMessage{std::move(message)});
    }
  }
  void push_back(AdnlMessage message) {
    messages_.push_back(std::move(message));
  }

  td::uint32 size() const {
    return static_cast<td::uint32>(messages_.size());
  }
  tl_object_ptr<ton_api::adnl_Message> one_message() const {
    CHECK(size() == 1);
    return messages_[0].tl();
  }
  std::vector<tl_object_ptr<ton_api::adnl_Message>> mult_messages() const {
    std::vector<tl_object_ptr<ton_api::adnl_Message>> vec;
    for (auto &m : messages_) {
      vec.emplace_back(m.tl());
    }
    return vec;
  }
  static std::vector<tl_object_ptr<ton_api::adnl_Message>> empty_vector() {
    return std::vector<tl_object_ptr<ton_api::adnl_Message>>{};
  }
  auto &vector() {
    return messages_;
  }

 private:
  std::vector<AdnlMessage> messages_;
};

class OutboundAdnlMessageList {
 public:
  OutboundAdnlMessageList() {
  }
  OutboundAdnlMessageList(tl_object_ptr<ton_api::adnl_Message> message, td::uint32 flags) {
    auto msg = OutboundAdnlMessage{std::move(message), flags};
    messages_.emplace_back(std::move(msg));
  }
  OutboundAdnlMessageList(std::vector<tl_object_ptr<ton_api::adnl_Message>> messages, td::uint32 flags) {
    for (auto &message : messages) {
      messages_.push_back(OutboundAdnlMessage{std::move(message), flags});
    }
  }
  void push_back(OutboundAdnlMessage message) {
    messages_.push_back(std::move(message));
  }

  td::uint32 size() const {
    return static_cast<td::uint32>(messages_.size());
  }
  tl_object_ptr<ton_api::adnl_Message> one_message() const {
    CHECK(size() == 1);
    return messages_[0].tl();
  }
  std::vector<tl_object_ptr<ton_api::adnl_Message>> mult_messages() const {
    std::vector<tl_object_ptr<ton_api::adnl_Message>> vec;
    for (auto &m : messages_) {
      vec.emplace_back(m.tl());
    }
    return vec;
  }
  static std::vector<tl_object_ptr<ton_api::adnl_Message>> empty_vector() {
    return std::vector<tl_object_ptr<ton_api::adnl_Message>>{};
  }
  auto &vector() {
    return messages_;
  }

 private:
  std::vector<OutboundAdnlMessage> messages_;
};

}  // namespace adnl

}  // namespace ton
