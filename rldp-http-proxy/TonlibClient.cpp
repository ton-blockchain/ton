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
#include "TonlibClient.h"

TonlibClient::TonlibClient(ton::tl_object_ptr<tonlib_api::options> options) : options_(std::move(options)) {
}

void TonlibClient::start_up() {
  class Cb : public tonlib::TonlibCallback {
   public:
    explicit Cb(td::actor::ActorId<TonlibClient> self_id) : self_id_(self_id) {
    }
    void on_result(std::uint64_t id, tonlib_api::object_ptr<tonlib_api::Object> result) override {
      td::actor::send_closure(self_id_, &TonlibClient::receive_request_result, id, std::move(result));
    }
    void on_error(std::uint64_t id, tonlib_api::object_ptr<tonlib_api::error> error) override {
      td::actor::send_closure(self_id_, &TonlibClient::receive_request_result, id,
                              td::Status::Error(error->code_, std::move(error->message_)));
    }

   private:
    td::actor::ActorId<TonlibClient> self_id_;
  };

  tonlib_client_ = td::actor::create_actor<tonlib::TonlibClient>("tonlibclient", td::make_unique<Cb>(actor_id(this)));
  auto init = tonlib_api::make_object<tonlib_api::init>(std::move(options_));
  auto P = td::PromiseCreator::lambda([](td::Result<tonlib_api::object_ptr<tonlib_api::Object>> R) mutable {
    R.ensure();
  });
  send_request(std::move(init), std::move(P));
}

void TonlibClient::send_request(tonlib_api::object_ptr<tonlib_api::Function> obj,
                                td::Promise<tonlib_api::object_ptr<tonlib_api::Object>> promise) {
  auto id = next_request_id_++;
  CHECK(requests_.emplace(id, std::move(promise)).second);
  td::actor::send_closure(tonlib_client_, &tonlib::TonlibClient::request, id, std::move(obj));
}

void TonlibClient::receive_request_result(td::uint64 id, td::Result<tonlib_api::object_ptr<tonlib_api::Object>> R) {
  if (id == 0) {
    return;
  }
  auto it = requests_.find(id);
  CHECK(it != requests_.end());
  auto promise = std::move(it->second);
  requests_.erase(it);
  promise.set_result(std::move(R));
}