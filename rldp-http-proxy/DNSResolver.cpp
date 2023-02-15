/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission
    to link the code of portions of this program with the OpenSSL library.
    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the file(s),
    but you are not obligated to do so. If you do not wish to do so, delete this
    exception statement from your version. If you delete this exception statement
    from all source files in the program, then also delete it here.
*/
#include "DNSResolver.h"
#include "td/utils/overloaded.h"
#include "common/delay.h"

static const double CACHE_TIMEOUT_HARD = 300.0;
static const double CACHE_TIMEOUT_SOFT = 270.0;

DNSResolver::DNSResolver(td::actor::ActorId<tonlib::TonlibClientWrapper> tonlib_client)
    : tonlib_client_(std::move(tonlib_client)) {
}

void DNSResolver::start_up() {
  sync();
}

void DNSResolver::sync() {
  auto obj = tonlib_api::make_object<tonlib_api::sync>();
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](
                                          td::Result<tonlib_api::object_ptr<tonlib_api::ton_blockIdExt>> R) {
    if (R.is_error()) {
      LOG(WARNING) << "Sync error: " << R.move_as_error();
      ton::delay_action([SelfId]() { td::actor::send_closure(SelfId, &DNSResolver::sync); }, td::Timestamp::in(5.0));
    }
  });
  td::actor::send_closure(tonlib_client_, &tonlib::TonlibClientWrapper::send_request<tonlib_api::sync>, std::move(obj),
                          std::move(P));
}

void DNSResolver::resolve(std::string host, td::Promise<std::string> promise) {
  auto it = cache_.find(host);
  if (it != cache_.end()) {
    const CacheEntry &entry = it->second;
    double now = td::Time::now();
    if (now < entry.created_at_ + CACHE_TIMEOUT_HARD) {
      promise.set_result(entry.address_);
      promise.reset();
      if (now < entry.created_at_ + CACHE_TIMEOUT_SOFT) {
        return;
      }
    }
  }

  td::Bits256 category = td::sha256_bits256(td::Slice("site", 4));
  auto obj = tonlib_api::make_object<tonlib_api::dns_resolve>(nullptr, host, category, 16);
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), promise = std::move(promise), host = std::move(host)](
                                          td::Result<tonlib_api::object_ptr<tonlib_api::dns_resolved>> R) mutable {
    if (R.is_error()) {
      if (promise) {
        promise.set_result(R.move_as_error());
      }
      return;
    }
    auto obj = R.move_as_ok();
    std::string result;
    if (!obj->entries_.empty()) {
      tonlib_api::downcast_call(*obj->entries_[0]->entry_,
                                td::overloaded(
                                    [&](tonlib_api::dns_entryDataAdnlAddress &x) {
                                      auto R = ton::adnl::AdnlNodeIdShort::parse(x.adnl_address_->adnl_address_);
                                      if (R.is_ok()) {
                                        ton::adnl::AdnlNodeIdShort id = R.move_as_ok();
                                        result = id.serialize() + ".adnl";
                                      }
                                    },
                                    [&](tonlib_api::dns_entryDataStorageAddress &x) {
                                      result = td::to_lower(x.bag_id_.to_hex()) + ".bag";
                                    },
                                    [&](auto &x) {}));
    }
    if (result.empty()) {
      if (promise) {
        promise.set_error(td::Status::Error("no DNS entries"));
      }
      return;
    }
    td::actor::send_closure(SelfId, &DNSResolver::save_to_cache, std::move(host), result);
    if (promise) {
      promise.set_result(std::move(result));
    }
  });
  td::actor::send_closure(tonlib_client_, &tonlib::TonlibClientWrapper::send_request<tonlib_api::dns_resolve>,
                          std::move(obj), std::move(P));
}

void DNSResolver::save_to_cache(std::string host, std::string address) {
  CacheEntry &entry = cache_[host];
  entry.address_ = address;
  entry.created_at_ = td::Time::now();
}
