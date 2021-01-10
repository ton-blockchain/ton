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
#include "validator/interfaces/validator-manager.h"

#include <set>

namespace ton {

namespace validator {

class TokenManager : public td::actor::Actor {
 public:
  TokenManager() {
  }
  void alarm() override;

  void get_download_token(size_t download_size, td::uint32 priority, td::Timestamp timeout,
                          td::Promise<std::unique_ptr<DownloadToken>> promise);
  void download_token_cleared(size_t download_size, td::uint32 priority);

 private:
  std::unique_ptr<DownloadToken> gen_token(size_t download_size, td::uint32 priority);
  struct PendingPromiseKey {
    size_t download_size;
    td::uint32 priority;
    td::uint64 seqno;

    bool operator<(const PendingPromiseKey &with) const {
      return priority < with.priority || (priority == with.priority && seqno < with.seqno);
    }
  };
  struct PendingPromise {
    td::Timestamp timeout;
    td::Promise<std::unique_ptr<DownloadToken>> promise;
  };
  td::uint64 seqno_ = 0;
  std::map<PendingPromiseKey, PendingPromise> pending_;

  td::uint32 free_tokens_ = 16;
  td::uint32 free_priority_tokens_ = 16;
  td::uint32 max_priority_tokens_ = 16;
};

}  // namespace validator

}  // namespace ton
