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
#include "dht-types.h"
#include "td/utils/Random.h"
#include "td/utils/overloaded.h"
#include "keys/encryptor.h"

#include "auto/tl/ton_api.hpp"

#include <map>

namespace ton {

namespace dht {

td::Result<DhtKey> DhtKey::create(tl_object_ptr<ton_api::dht_key> key) {
  if (key->name_.length() > max_name_length()) {
    return td::Status::Error(ErrorCode::error, PSTRING() << "too big name length. length=" << key->name_.length());
  }
  if (!key->name_.length()) {
    return td::Status::Error(ErrorCode::error, PSTRING() << "empty dht key name");
  }
  if (key->idx_ < 0 || static_cast<td::uint32>(key->idx_) > max_index()) {
    return td::Status::Error(ErrorCode::error, PSTRING() << "bad dht key index " << key->idx_);
  }

  return DhtKey{PublicKeyHash{key->id_}, key->name_.as_slice().str(), static_cast<td::uint32>(key->idx_)};
}

tl_object_ptr<ton_api::dht_key> DhtKey::tl() const {
  return create_tl_object<ton_api::dht_key>(id_.tl(), td::BufferSlice{namestr_}, idx_);
}

td::Status DhtKey::check() const {
  if (namestr_.length() > max_name_length()) {
    return td::Status::Error(ErrorCode::error, PSTRING() << "too big name length. length=" << namestr_.length());
  }
  if (namestr_.length() == 0) {
    return td::Status::Error(ErrorCode::error, PSTRING() << "empty dht key name");
  }
  if (static_cast<td::uint32>(idx_) > max_index()) {
    return td::Status::Error(ErrorCode::error, PSTRING() << "bad dht key index " << idx_);
  }
  return td::Status::OK();
}

DhtKeyId DhtKey::compute_key_id() const {
  return DhtKeyId{get_tl_object_sha_bits256(tl())};
}

DhtKey DhtKey::clone() const {
  return DhtKey{id_, namestr_, idx_};
}

void DhtKeyDescription::update_signature(td::BufferSlice signature) {
  signature_ = td::SharedSlice{signature.as_slice()};
}

void DhtKeyDescription::update_signature(td::SharedSlice signature) {
  signature_ = std::move(signature);
}

td::Status DhtKeyDescription::check() const {
  TRY_STATUS(key_.check());
  if (public_key_.compute_short_id() != key_.public_key_hash()) {
    return td::Status::Error(ErrorCode::protoviolation, "key hash mismatch");
  }
  auto obj = tl();
  obj->signature_ = td::BufferSlice{};

  auto B = serialize_tl_object(obj, true);
  TRY_RESULT(E, public_key_.create_encryptor());
  TRY_STATUS(E->check_signature(B.as_slice(), signature_.as_slice()));
  return td::Status::OK();
}

tl_object_ptr<ton_api::dht_keyDescription> DhtKeyDescription::tl() const {
  return create_tl_object<ton_api::dht_keyDescription>(key_.tl(), public_key_.tl(), update_rule_->tl(),
                                                       signature_.clone_as_buffer_slice());
}

td::BufferSlice DhtKeyDescription::to_sign() const {
  return create_serialize_tl_object<ton_api::dht_keyDescription>(key_.tl(), public_key_.tl(), update_rule_->tl(),
                                                                 td::BufferSlice());
}

DhtKeyDescription DhtKeyDescription::clone() const {
  return DhtKeyDescription{key_.clone(), public_key_, update_rule_, signature_.clone_as_buffer_slice()};
}

td::Result<DhtKeyDescription> DhtKeyDescription::create(DhtKey key, PublicKey public_key,
                                                        std::shared_ptr<DhtUpdateRule> update_rule,
                                                        td::BufferSlice signature) {
  DhtKeyDescription desc{std::move(key), std::move(public_key), std::move(update_rule), std::move(signature)};
  TRY_STATUS(desc.check());
  return std::move(desc);
}

td::Result<DhtKeyDescription> DhtKeyDescription::create(DhtKey key, PublicKey public_key,
                                                        std::shared_ptr<DhtUpdateRule> update_rule,
                                                        td::SharedSlice signature) {
  DhtKeyDescription desc{std::move(key), std::move(public_key), std::move(update_rule), std::move(signature)};
  TRY_STATUS(desc.check());
  return std::move(desc);
}

td::Result<DhtKeyDescription> DhtKeyDescription::create(tl_object_ptr<ton_api::dht_keyDescription> desc,
                                                        bool check_signature) {
  auto signature = std::move(desc->signature_);
  td::BufferSlice to_sign;
  if (check_signature) {
    to_sign = serialize_tl_object(desc, true);
  }
  auto public_key = PublicKey{desc->id_};

  TRY_RESULT(key, DhtKey::create(std::move(desc->key_)));
  if (key.public_key_hash() != public_key.compute_short_id()) {
    return td::Status::Error(ErrorCode::error, "inconsistent dht key description");
  }
  TRY_RESULT(update_rule, DhtUpdateRule::create(std::move(desc->update_rule_)));

  if (check_signature) {
    TRY_RESULT(E, public_key.create_encryptor());
    TRY_STATUS(E->check_signature(to_sign.as_slice(), signature.as_slice()));
  }

  return DhtKeyDescription{std::move(key), std::move(public_key), std::move(update_rule), std::move(signature)};
}

td::Result<DhtValue> DhtValue::create(tl_object_ptr<ton_api::dht_value> obj, bool check_signature) {
  TRY_RESULT(desc, DhtKeyDescription::create(std::move(obj->key_), check_signature));

  return create(std::move(desc), std::move(obj->value_), obj->ttl_, std::move(obj->signature_));
}

td::Result<DhtValue> DhtValue::create(DhtKeyDescription key, td::BufferSlice value, td::uint32 ttl,
                                      td::BufferSlice signature) {
  TRY_STATUS(key.check());
  DhtValue v{std::move(key), std::move(value), ttl, std::move(signature)};
  TRY_STATUS(v.key().update_rule()->check_value(v));
  return std::move(v);
}

td::Result<DhtValue> DhtValue::create(DhtKeyDescription key, td::SharedSlice value, td::uint32 ttl,
                                      td::SharedSlice signature) {
  TRY_STATUS(key.check());
  DhtValue v{std::move(key), std::move(value), ttl, std::move(signature)};
  TRY_STATUS(v.key().update_rule()->check_value(v));
  return std::move(v);
}

DhtValue DhtValue::clone() const {
  return DhtValue{key_.clone(), value_.clone(), ttl_, signature_.clone()};
}

tl_object_ptr<ton_api::dht_value> DhtValue::tl() const {
  return create_tl_object<ton_api::dht_value>(key_.tl(), value_.clone_as_buffer_slice(), ttl_,
                                              signature_.clone_as_buffer_slice());
}

td::BufferSlice DhtValue::to_sign() const {
  return create_serialize_tl_object<ton_api::dht_value>(key_.tl(), value_.clone_as_buffer_slice(), ttl_,
                                                        td::BufferSlice());
}

td::Status DhtValue::update(DhtValue &&value) {
  TRY_STATUS(value.check());
  return key_.update_rule()->update_value(*this, std::move(value));
}

void DhtValue::set(td::BufferSlice value, td::uint32 ttl, td::BufferSlice signature) {
  value_ = td::SharedSlice{value.as_slice()};
  ttl_ = ttl;
  signature_ = td::SharedSlice{signature.as_slice()};
}

void DhtValue::set(td::SharedSlice value, td::uint32 ttl, td::SharedSlice signature) {
  value_ = std::move(value);
  ttl_ = ttl;
  signature_ = std::move(signature);
}

void DhtValue::update_signature(td::BufferSlice signature) {
  signature_ = td::SharedSlice{signature.as_slice()};
}

void DhtValue::update_signature(td::SharedSlice signature) {
  signature_ = std::move(signature);
}

td::Status DhtValue::check() const {
  TRY_STATUS(key_.check());
  return key_.update_rule()->check_value(*this);
}

bool DhtValue::check_is_acceptable() const {
  return key_.update_rule()->check_is_acceptable(*this);
}

DhtKeyId DhtValue::key_id() const {
  return key_.key().compute_key_id();
}

td::Status DhtUpdateRuleSignature::check_value(const DhtValue &value) {
  if (value.value().size() > DhtValue::max_value_size()) {
    return td::Status::Error(ErrorCode::protoviolation, "too big value");
  }
  TRY_RESULT(E, value.key().public_key().create_encryptor());
  auto tl = value.tl();
  auto sig = std::move(tl->signature_);
  auto B = serialize_tl_object(tl, true);
  return E->check_signature(B.as_slice(), sig.as_slice());
}

td::Status DhtUpdateRuleSignature::update_value(DhtValue &value, DhtValue &&new_value) {
  TRY_STATUS(new_value.check());
  CHECK(value.key_id() == new_value.key_id());
  if (new_value.ttl() > value.ttl()) {
    value.set(new_value.value().clone(), new_value.ttl(), new_value.signature().clone());
    value.check().ensure();
  }
  return td::Status::OK();
}

tl_object_ptr<ton_api::dht_UpdateRule> DhtUpdateRuleSignature::tl() const {
  return create_tl_object<ton_api::dht_updateRule_signature>();
}

td::Result<std::shared_ptr<DhtUpdateRule>> DhtUpdateRuleSignature::create() {
  return std::make_shared<DhtUpdateRuleSignature>();
}

td::Status DhtUpdateRuleAnybody::check_value(const DhtValue &value) {
  if (value.value().size() > DhtValue::max_value_size()) {
    return td::Status::Error(ErrorCode::protoviolation, "too big value");
  }
  if (value.signature().size() > 0) {
    return td::Status::Error(ErrorCode::protoviolation, "cannot have signature in DhtUpdateRuleAnybody");
  }
  return td::Status::OK();
}

td::Status DhtUpdateRuleAnybody::update_value(DhtValue &value, DhtValue &&new_value) {
  CHECK(value.key_id() == new_value.key_id());
  value.set(new_value.value().clone(), new_value.ttl(), new_value.signature().clone());
  return td::Status::OK();
}

tl_object_ptr<ton_api::dht_UpdateRule> DhtUpdateRuleAnybody::tl() const {
  return create_tl_object<ton_api::dht_updateRule_anybody>();
}

td::Result<std::shared_ptr<DhtUpdateRule>> DhtUpdateRuleAnybody::create() {
  return std::make_shared<DhtUpdateRuleAnybody>();
}

td::Status DhtUpdateRuleOverlayNodes::check_value(const DhtValue &value) {
  if (value.value().size() > DhtValue::max_value_size()) {
    return td::Status::Error(ErrorCode::protoviolation, "too big value");
  }
  if (value.signature().size() > 0) {
    return td::Status::Error(ErrorCode::protoviolation, "cannot have signature in DhtUpdateRuleOverlayNodes");
  }
  auto F = fetch_tl_object<ton_api::overlay_nodes>(value.value().clone_as_buffer_slice(), true);
  if (F.is_error()) {
    return td::Status::Error(ErrorCode::protoviolation, "bad overlay nodes value");
  }
  auto L = F.move_as_ok();
  for (auto &node : L->nodes_) {
    TRY_RESULT(pub, adnl::AdnlNodeIdFull::create(node->id_));
    auto sig = std::move(node->signature_);
    auto obj =
        create_tl_object<ton_api::overlay_node_toSign>(pub.compute_short_id().tl(), node->overlay_, node->version_);
    if (node->overlay_ != value.key().key().public_key_hash().bits256_value()) {
      return td::Status::Error(ErrorCode::protoviolation, "bad overlay id");
    }
    auto B = serialize_tl_object(obj, true);
    TRY_RESULT(E, pub.pubkey().create_encryptor());
    TRY_STATUS(E->check_signature(B.as_slice(), sig.as_slice()));
  }
  return td::Status::OK();
}

td::Status DhtUpdateRuleOverlayNodes::update_value(DhtValue &value, DhtValue &&new_value) {
  TRY_RESULT_PREFIX(N, fetch_tl_object<ton_api::overlay_nodes>(value.value().clone_as_buffer_slice(), true),
                    "bad dht value in updateRule.overlayNodes: ");
  TRY_RESULT_PREFIX(L, fetch_tl_object<ton_api::overlay_nodes>(new_value.value().clone_as_buffer_slice(), true),
                    "bad dht value in updateRule.overlayNodes: ");

  std::vector<tl_object_ptr<ton_api::overlay_node>> res;

  std::map<adnl::AdnlNodeIdShort, size_t> S;
  for (auto &n : N->nodes_) {
    TRY_RESULT(pub, adnl::AdnlNodeIdFull::create(n->id_));
    auto id = pub.compute_short_id();
    auto it = S.find(id);
    if (it != S.end()) {
      auto &m = res[it->second];
      if (m->version_ < n->version_) {
        m = std::move(n);
      }
    } else {
      S.emplace(id, res.size());
      res.emplace_back(std::move(n));
    }
  }
  for (auto &n : L->nodes_) {
    TRY_RESULT(pub, adnl::AdnlNodeIdFull::create(n->id_));
    auto id = pub.compute_short_id();
    auto it = S.find(id);
    if (it != S.end()) {
      auto &m = res[it->second];
      if (m->version_ < n->version_) {
        m = std::move(n);
      }
    } else {
      S.emplace(id, res.size());
      res.emplace_back(std::move(n));
    }
  }

  size_t size = 8;  // magic + size
  std::vector<std::pair<td::uint32, size_t>> v;
  for (td::uint32 i = 0; i < res.size(); i++) {
    v.emplace_back(i, serialize_tl_object(res[i], false).size());
    size += v[i].second;
  }

  while (size > DhtValue::max_value_size()) {
    CHECK(v.size() > 0);
    auto idx = td::Random::fast(0, static_cast<td::int32>(v.size() - 1));
    size -= v[idx].second;
    v[idx] = v[v.size() - 1];
    v.resize(v.size() - 1);
  }

  std::vector<tl_object_ptr<ton_api::overlay_node>> vec;
  for (auto &p : v) {
    vec.push_back(std::move(res[p.first]));
  }
  auto nodes = create_serialize_tl_object<ton_api::overlay_nodes>(std::move(vec));
  CHECK(nodes.size() == size);
  CHECK(nodes.size() <= DhtValue::max_value_size());

  value.set(std::move(nodes), std::max(value.ttl(), new_value.ttl()), td::BufferSlice{});
  value.check().ensure();

  return td::Status::OK();
}

bool DhtUpdateRuleOverlayNodes::check_is_acceptable(const ton::dht::DhtValue &value) {
  auto F = fetch_tl_object<ton_api::overlay_nodes>(value.value().clone_as_buffer_slice(), true);
  if (F.is_error()) {
    return false;
  }
  auto L = F.move_as_ok();
  auto now = td::Clocks::system();
  for (auto &node : L->nodes_) {
    if (node->version_ + 600 > now) {
      return true;
    }
  }
  return false;
}

tl_object_ptr<ton_api::dht_UpdateRule> DhtUpdateRuleOverlayNodes::tl() const {
  return create_tl_object<ton_api::dht_updateRule_overlayNodes>();
}

td::Result<std::shared_ptr<DhtUpdateRule>> DhtUpdateRuleOverlayNodes::create() {
  return std::make_shared<DhtUpdateRuleOverlayNodes>();
}

td::Result<std::shared_ptr<DhtUpdateRule>> DhtUpdateRule::create(tl_object_ptr<ton_api::dht_UpdateRule> obj) {
  td::Result<std::shared_ptr<DhtUpdateRule>> R;
  ton_api::downcast_call(
      *obj.get(),
      td::overloaded([&](ton_api::dht_updateRule_signature &obj) { R = DhtUpdateRuleSignature::create(); },
                     [&](ton_api::dht_updateRule_anybody &obj) { R = DhtUpdateRuleAnybody::create(); },
                     [&](ton_api::dht_updateRule_overlayNodes &obj) { R = DhtUpdateRuleOverlayNodes::create(); }));
  return R;
}

}  // namespace dht

}  // namespace ton
