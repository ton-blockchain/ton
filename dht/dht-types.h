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

#include "td/utils/int_types.h"
#include "td/utils/SharedSlice.h"
#include "adnl/adnl-node-id.hpp"
#include "tl-utils/tl-utils.hpp"
#include "common/io.hpp"

namespace ton {

namespace dht {

using DhtKeyName = std::string;

class DhtKeyId {
 public:
  explicit DhtKeyId(td::Bits256 value) : value_(value) {
  }
  explicit DhtKeyId(adnl::AdnlNodeIdShort value) : value_(value.bits256_value()) {
  }
  DhtKeyId() {
  }
  td::Bits256 tl() const {
    return value_;
  }

  bool operator<(const DhtKeyId &with) const {
    return value_ < with.value_;
  }
  bool operator==(const DhtKeyId &with) const {
    return value_ == with.value_;
  }
  bool operator!=(const DhtKeyId &with) const {
    return value_ != with.value_;
  }
  DhtKeyId operator^(const DhtKeyId &with) const {
    return DhtKeyId{value_ ^ with.value_};
  }
  DhtKeyId operator^(const adnl::AdnlNodeIdShort &with) const {
    return DhtKeyId{value_ ^ with.bits256_value()};
  }

  bool get_bit(td::uint32 bit) const {
    return value_[bit];
  }

  td::uint32 count_leading_zeroes() const {
    return value_.count_leading_zeroes();
  }
  adnl::AdnlNodeIdShort to_adnl() const {
    return adnl::AdnlNodeIdShort{value_};
  }

  static DhtKeyId zero() {
    return DhtKeyId{td::Bits256::zero()};
  }

 private:
  td::Bits256 value_;
};

using DhtXoredKeyId = DhtKeyId;

class DhtKey {
 public:
  static constexpr td::uint32 max_name_length() {
    return 127;
  }
  static constexpr td::uint32 max_index() {
    return 15;
  }
  DhtKey(PublicKeyHash id, DhtKeyName namestr, td::uint32 idx)
      : id_(std::move(id)), namestr_(std::move(namestr)), idx_(idx) {
  }
  static td::Result<DhtKey> create(tl_object_ptr<ton_api::dht_key> key);
  td::Status check() const;
  const auto &public_key_hash() const {
    return id_;
  }
  const auto &name() const {
    return namestr_;
  }
  td::uint32 idx() const {
    return idx_;
  }
  tl_object_ptr<ton_api::dht_key> tl() const;
  DhtKeyId compute_key_id() const;
  DhtKey clone() const;

 private:
  PublicKeyHash id_;
  DhtKeyName namestr_;
  td::uint32 idx_;
};

class DhtValue;

class DhtUpdateRule {
 public:
  virtual ~DhtUpdateRule() = default;
  virtual td::Status check_value(const DhtValue &value) = 0;
  virtual td::Status update_value(DhtValue &value, DhtValue &&new_value) = 0;
  virtual bool need_republish() const = 0;
  virtual bool check_is_acceptable(const DhtValue &value) {
    return true;
  }
  virtual tl_object_ptr<ton_api::dht_UpdateRule> tl() const = 0;
  static td::Result<std::shared_ptr<DhtUpdateRule>> create(tl_object_ptr<ton_api::dht_UpdateRule> obj);
};

class DhtKeyDescription {
 public:
  DhtKeyDescription(DhtKey key, PublicKey public_key, std::shared_ptr<DhtUpdateRule> update_rule,
                    td::BufferSlice signature)
      : key_(std::move(key))
      , public_key_(std::move(public_key))
      , update_rule_(std::move(update_rule))
      , signature_(signature.as_slice()) {
  }
  DhtKeyDescription(DhtKey key, PublicKey public_key, std::shared_ptr<DhtUpdateRule> update_rule,
                    td::SharedSlice signature)
      : key_(std::move(key))
      , public_key_(std::move(public_key))
      , update_rule_(std::move(update_rule))
      , signature_(std::move(signature)) {
  }
  const auto &key() const {
    return key_;
  }
  const auto &public_key() const {
    return public_key_;
  }
  const auto &update_rule() const {
    return update_rule_;
  }
  void update_signature(td::BufferSlice signature);
  void update_signature(td::SharedSlice signature);
  td::BufferSlice to_sign() const;
  td::Status check() const;
  tl_object_ptr<ton_api::dht_keyDescription> tl() const;
  DhtKeyDescription clone() const;
  static td::Result<DhtKeyDescription> create(tl_object_ptr<ton_api::dht_keyDescription> desc, bool check_signature);
  static td::Result<DhtKeyDescription> create(DhtKey key, PublicKey public_key,
                                              std::shared_ptr<DhtUpdateRule> update_rule, td::BufferSlice signature);
  static td::Result<DhtKeyDescription> create(DhtKey key, PublicKey public_key,
                                              std::shared_ptr<DhtUpdateRule> update_rule, td::SharedSlice signature);

 private:
  DhtKey key_;
  PublicKey public_key_;
  std::shared_ptr<DhtUpdateRule> update_rule_;
  td::SharedSlice signature_;
};

class DhtValue {
 public:
  static constexpr td::uint32 max_value_size() {
    return 768;
  }

  DhtValue(DhtKeyDescription key, td::BufferSlice value, td::uint32 ttl, td::BufferSlice signature)
      : key_(std::move(key)), value_(value.as_slice()), ttl_(ttl), signature_(signature.as_slice()) {
  }
  DhtValue(DhtKeyDescription key, td::SharedSlice value, td::uint32 ttl, td::SharedSlice signature)
      : key_(std::move(key)), value_(std::move(value)), ttl_(ttl), signature_(std::move(signature)) {
  }

  static td::Result<DhtValue> create(tl_object_ptr<ton_api::dht_value> obj, bool check_signature);
  static td::Result<DhtValue> create(DhtKeyDescription key, td::BufferSlice value, td::uint32 ttl,
                                     td::BufferSlice signature);
  static td::Result<DhtValue> create(DhtKeyDescription key, td::SharedSlice value, td::uint32 ttl,
                                     td::SharedSlice signature);
  const auto &key() const {
    return key_;
  }
  const auto &value() const {
    return value_;
  }
  const auto &signature() const {
    return signature_;
  }
  td::uint32 ttl() const {
    return ttl_;
  }
  bool expired() const {
    return ttl_ < td::Clocks::system();
  }
  DhtValue clone() const;

  tl_object_ptr<ton_api::dht_value> tl() const;
  td::BufferSlice to_sign() const;
  td::Status update(DhtValue &&value);
  void set(td::BufferSlice value, td::uint32 ttl, td::BufferSlice signature);
  void set(td::SharedSlice value, td::uint32 ttl, td::SharedSlice signature);
  void update_signature(td::BufferSlice signature);
  void update_signature(td::SharedSlice signature);
  td::Status check() const;
  bool check_is_acceptable() const;

  DhtKeyId key_id() const;

 private:
  DhtKeyDescription key_;
  td::SharedSlice value_;
  td::uint32 ttl_;
  td::SharedSlice signature_;
};

class DhtUpdateRuleSignature : public DhtUpdateRule {
 public:
  td::Status check_value(const DhtValue &value) override;
  td::Status update_value(DhtValue &value, DhtValue &&new_value) override;
  bool need_republish() const override {
    return true;
  }
  tl_object_ptr<ton_api::dht_UpdateRule> tl() const override;
  static td::Result<std::shared_ptr<DhtUpdateRule>> create();
};

class DhtUpdateRuleAnybody : public DhtUpdateRule {
 public:
  td::Status check_value(const DhtValue &value) override;
  td::Status update_value(DhtValue &value, DhtValue &&new_value) override;
  bool need_republish() const override {
    return false;
  }
  tl_object_ptr<ton_api::dht_UpdateRule> tl() const override;
  static td::Result<std::shared_ptr<DhtUpdateRule>> create();
};

class DhtUpdateRuleOverlayNodes : public DhtUpdateRule {
 public:
  td::Status check_value(const DhtValue &value) override;
  td::Status update_value(DhtValue &value, DhtValue &&new_value) override;
  bool need_republish() const override {
    return false;
  }
  bool check_is_acceptable(const DhtValue &value) override;
  tl_object_ptr<ton_api::dht_UpdateRule> tl() const override;
  static td::Result<std::shared_ptr<DhtUpdateRule>> create();
};

}  // namespace dht

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::dht::DhtKeyId &dht) {
  sb << dht.tl();
  return sb;
}

}  // namespace td
