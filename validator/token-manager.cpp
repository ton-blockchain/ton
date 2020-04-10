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
#include "token-manager.h"

namespace ton {

namespace validator {

void TokenManager::get_download_token(size_t download_size, td::uint32 priority, td::Timestamp timeout,
                                      td::Promise<std::unique_ptr<DownloadToken>> promise) {
  if (free_priority_tokens_ > 0 && priority > 0) {
    --free_priority_tokens_;
    promise.set_value(gen_token(download_size, priority));
    return;
  }
  if (free_tokens_ > 0) {
    --free_tokens_;
    promise.set_value(gen_token(download_size, priority));
    return;
  }

  pending_.emplace(PendingPromiseKey{download_size, priority, seqno_++}, PendingPromise{timeout, std::move(promise)});
}

void TokenManager::download_token_cleared(size_t download_size, td::uint32 priority) {
  (priority ? free_priority_tokens_ : free_tokens_)++;
  if (free_priority_tokens_ > max_priority_tokens_) {
    free_priority_tokens_--;
    free_tokens_++;
  }

  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->first.priority && (free_tokens_ || free_priority_tokens_)) {
      it->second.promise.set_value(gen_token(download_size, priority));
      auto it2 = it++;
      pending_.erase(it2);
      if (free_priority_tokens_ > 0) {
        free_priority_tokens_--;
      } else {
        free_tokens_--;
      }
    } else if (!it->first.priority && free_tokens_) {
      it->second.promise.set_value(gen_token(download_size, priority));
      auto it2 = it++;
      pending_.erase(it2);
      free_tokens_--;
    } else {
      break;
    }
  }
}

void TokenManager::alarm() {
  for (auto it = pending_.begin(); it != pending_.end(); it++) {
    if (it->second.timeout.is_in_past()) {
      it->second.promise.set_error(td::Status::Error(ErrorCode::timeout, "timeout in wait download token"));
      auto it2 = it++;
      pending_.erase(it2);
    } else {
      it++;
    }
  }
}

std::unique_ptr<DownloadToken> TokenManager::gen_token(size_t download_size, td::uint32 priority) {
  class Token : public DownloadToken {
   public:
    Token(size_t download_size, td::uint32 priority, td::actor::ActorId<TokenManager> manager)
        : download_size_(download_size), priority_(priority), manager_(manager) {
    }
    ~Token() override {
      td::actor::send_closure(manager_, &TokenManager::download_token_cleared, download_size_, priority_);
    }

   private:
    size_t download_size_;
    td::uint32 priority_;
    td::actor::ActorId<TokenManager> manager_;
  };

  return std::make_unique<Token>(download_size, priority, actor_id(this));
}

}  // namespace validator

}  // namespace ton
