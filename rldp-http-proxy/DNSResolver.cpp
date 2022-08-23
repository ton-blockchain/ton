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

static const double CACHE_TIMEOUT_HARD = 300.0;
static const double CACHE_TIMEOUT_SOFT = 270.0;

DNSResolver::DNSResolver(td::actor::ActorId<TonlibClient> tonlib_client) : tonlib_client_(std::move(tonlib_client)) {
}

void DNSResolver::start_up() {
  auto obj = tonlib_api::make_object<tonlib_api::sync>();
  auto P = td::PromiseCreator::lambda([](td::Result<tonlib_api::object_ptr<tonlib_api::Object>>) {});
  td::actor::send_closure(tonlib_client_, &TonlibClient::send_request, std::move(obj), std::move(P));
}

void DNSResolver::resolve(std::string host, td::Promise<ton::adnl::AdnlNodeIdShort> promise) {
  auto it = cache_.find(host);
  if (it != cache_.end()) {
    const CacheEntry &entry = it->second;
    double now = td::Time::now();
    if (now < entry.created_at_ + CACHE_TIMEOUT_HARD) {
      promise.set_result(entry.id_);
      promise.reset();
      if (now < entry.created_at_ + CACHE_TIMEOUT_SOFT) {
        return;
      }
    }
  }

  td::Bits256 category = td::sha256_bits256(td::Slice("site", 4));
  auto obj = tonlib_api::make_object<tonlib_api::dns_resolve>(nullptr, host, category, 16);
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), promise = std::move(promise), host = std::move(host)](
                                          td::Result<tonlib_api::object_ptr<tonlib_api::Object>> R) mutable {
    if (R.is_error()) {
      if (promise) {
        promise.set_result(R.move_as_error());
      }
    } else {
      auto v = R.move_as_ok();
      auto obj = dynamic_cast<tonlib_api::dns_resolved *>(v.get());
      if (obj == nullptr) {
        promise.set_result(td::Status::Error("invalid response from tonlib"));
        return;
      }
      ton::adnl::AdnlNodeIdShort id;
      td::uint32 cnt = 0;
      for (auto &e : obj->entries_) {
        tonlib_api::downcast_call(*e->entry_.get(),
                                  td::overloaded(
                                      [&](tonlib_api::dns_entryDataAdnlAddress &x) {
                                        if (td::Random::fast(0, cnt) == 0) {
                                          auto R = ton::adnl::AdnlNodeIdShort::parse(x.adnl_address_->adnl_address_);
                                          if (R.is_ok()) {
                                            id = R.move_as_ok();
                                            cnt++;
                                          }
                                        }
                                      },
                                      [&](auto &x) {}));
      }
      if (cnt == 0) {
        if (promise) {
          promise.set_error(td::Status::Error("no DNS entries"));
        }
      } else {
        td::actor::send_closure(SelfId, &DNSResolver::save_to_cache, std::move(host), id);
        if (promise) {
          promise.set_result(id);
        }
      }
    }
  });
  td::actor::send_closure(tonlib_client_, &TonlibClient::send_request, std::move(obj), std::move(P));
}

void DNSResolver::save_to_cache(std::string host, ton::adnl::AdnlNodeIdShort id) {
  CacheEntry &entry = cache_[host];
  entry.id_ = id;
  entry.created_at_ = td::Time::now();
}
