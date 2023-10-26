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

#include "PeerState.h"
#include "Bitset.h"

#include "td/utils/Random.h"
#include "td/utils/Status.h"

namespace ton {
struct PartsHelper {
 public:
  explicit PartsHelper(size_t parts_count = 0) : parts_(parts_count), peers_(64) {
    peers_[0].is_valid = true;
  }
  using PartId = size_t;
  using PeerToken = size_t;

  void init_parts_count(size_t parts_count) {
    CHECK(parts_.empty());
    parts_.resize(parts_count);
  }
  PeerToken register_self() {
    return self_token_;
  }
  PeerToken register_peer(PeerId peer_id) {
    PeerToken new_peer_token{free_peer_tokens_.empty() ? next_peer_token_ : free_peer_tokens_.back()};
    auto it = peer_id_to_token_.emplace(peer_id, new_peer_token);
    if (it.second) {
      if (free_peer_tokens_.empty()) {
        next_peer_token_++;
        peers_.resize(next_peer_token_);
      } else {
        free_peer_tokens_.pop_back();
      }
      auto peer = get_peer(new_peer_token, true);
      peer->is_valid = true;
      peer->peer_id = peer_id;
      peer->want_download_count = 0;
    }
    return it.first->second;
  }
  void forget_peer(PeerToken peer_token) {
    CHECK(peer_token != self_token_);
    free_peer_tokens_.push_back(peer_token);
    auto *peer = get_peer(peer_token);
    peer_id_to_token_.erase(get_peer(peer_token)->peer_id);
    *peer = Peer{};
    peer->rnd = td::Random::fast_uint32();
  }

  void set_peer_limit(PeerToken peer, td::uint32 limit) {
    get_peer(peer)->limit = limit;
  }

  void on_peer_part_ready(PeerToken peer_token, PartId part_id) {
    auto peer = get_peer(peer_token);
    if (!peer->ready_parts.set_one(part_id)) {
      return;
    }
    auto part = get_part(part_id);
    if (part->is_ready) {
      return;
    }
    peer->want_download_count++;
    if (!part->rnd) {
      part->rnd = td::Random::fast_uint32();
    }
    change_key(part_id, part->rnd, part->peers_count, part->peers_count + 1, part->priority, part->priority);
    part->peers_count++;
  }

  void lock_part(PartId part_id) {
    auto *part = get_part(part_id);
    CHECK(!part->is_locked);
    part->is_locked = true;
  }
  void unlock_part(PartId part_id) {
    auto *part = get_part(part_id);
    CHECK(part->is_locked);
    part->is_locked = false;
  }

  void set_part_priority(PartId part_id, td::uint8 priority) {
    auto *part = get_part(part_id);
    if (part->is_ready) {
      return;
    }
    change_key(part_id, part->rnd, part->peers_count, part->peers_count, part->priority, priority);
    part->priority = priority;
  }

  td::uint8 get_part_priority(PartId part_id) {
    auto *part = get_part(part_id);
    return part->priority;
  }

  void on_self_part_ready(PartId part_id) {
    auto peer = get_peer(self_token_);
    if (!peer->ready_parts.set_one(part_id)) {
      return;
    }
    auto part = get_part(part_id);
    CHECK(!part->is_ready);
    part->is_ready = true;
    for (auto &peer : peers_) {
      if (peer.ready_parts.get(part_id)) {
        peer.want_download_count--;
      }
    }
    change_key(part_id, part->rnd, part->peers_count, 0, part->priority, part->priority);
  }

  void on_self_part_not_ready(PartId part_id) {
    auto peer = get_peer(self_token_);
    if (!peer->ready_parts.set_zero(part_id)) {
      return;
    }
    auto part = get_part(part_id);
    CHECK(part->is_ready);
    part->is_ready = false;
    for (auto &peer : peers_) {
      if (peer.ready_parts.get(part_id)) {
        peer.want_download_count++;
      }
    }
    change_key(part_id, part->rnd, 0, part->peers_count, part->priority, part->priority);
  }

  struct RarePart {
    PartId part_id;
    PeerId peer_id;
  };
  std::vector<RarePart> get_rarest_parts(size_t max_count) {
    struct It {
      std::set<Peer::Key>::iterator begin, end;
      PeerId peer_id;
      td::uint32 limit{0};
      bool empty() const {
        return begin == end || limit == 0;
      }
      auto key() const {
        return std::tie(*begin, peer_id);
      }
      bool operator<(const It &other) const {
        return key() < other.key();
      }
    };
    std::set<It> its;

    for (auto &peer : peers_) {
      if (!peer.is_valid || peer.limit == 0) {
        continue;
      }
      It it;
      it.begin = peer.rarest_parts.begin();
      it.end = peer.rarest_parts.end();
      it.peer_id = peer.peer_id;
      it.limit = peer.limit;
      if (it.empty()) {
        continue;
      }
      its.insert(it);
    }

    std::vector<RarePart> res;
    while (res.size() < max_count && !its.empty()) {
      auto it = *its.begin();
      its.erase(its.begin());
      auto part_id = it.begin->part_id;
      if ((res.empty() || res.back().part_id != part_id) && !get_part(part_id)->is_locked) {
        res.push_back({part_id, it.peer_id});
        CHECK(get_peer(register_peer(it.peer_id))->ready_parts.get(part_id));
        it.limit--;
      }
      it.begin++;
      if (it.empty()) {
        continue;
      }
      its.insert(it);
    }
    return res;
  }

  td::uint32 get_want_download_count(PeerToken peer_token) {
    return get_peer(peer_token, false)->want_download_count;
  }
  const td::Bitset &get_ready_parts(PeerToken peer_token) {
    return get_peer(peer_token, false)->ready_parts;
  }

 private:
  PeerToken self_token_{0};
  size_t parts_count_;
  struct Part {
    bool is_locked{false};
    bool is_ready{false};
    td::uint8 priority{1};
    td::uint32 rnd{0};
    td::uint32 peers_count{0};  // invalid for ready parts
  };
  struct Peer {
    PeerId peer_id{0};
    bool is_valid{false};
    td::uint32 rnd{0};
    td::uint32 limit{0};
    td::Bitset ready_parts;
    // sum_i (peer.ready_parts[i] && !node.ready_parts[i])
    td::uint32 want_download_count{0};

    // peers_count - count of peers which has this part
    // key_count = !is_ready * peers_count;
    struct Key {
      td::uint8 priority{0};
      td::uint32 count{0};
      PartId part_id{0};
      td::uint32 rnd{0};
      auto key() const {
        return std::make_tuple(255 - priority, count, rnd, part_id);
      }
      bool operator<(const Key &other) const {
        return key() < other.key();
      }
    };
    std::set<Key> rarest_parts;  // TODO: use vector instead of set
  };

  std::vector<Part> parts_;
  std::vector<Peer> peers_;
  td::uint32 next_peer_token_{1};
  std::map<PeerId, PeerToken> peer_id_to_token_;
  std::vector<PeerToken> free_peer_tokens_;

  Part *get_part(PartId part_id) {
    CHECK(part_id < parts_.size());
    return &parts_[part_id];
  }
  Peer *get_peer(PeerToken peer_token, bool can_be_uninited = false) {
    CHECK(peer_token < peers_.size());
    auto res = &peers_[peer_token];
    CHECK(res->is_valid || can_be_uninited);
    return res;
  }

  void change_key(PartId part_id, td::uint32 rnd, td::uint32 from_count, td::uint32 to_count, td::uint8 from_priority,
                  td::uint8 to_priority) {
    if (from_count == 0 && to_count == 0) {
      return;
    }
    if (from_count == to_count && from_priority == to_priority) {
      return;
    }
    for (auto &peer : peers_) {
      if (!peer.is_valid) {
        continue;
      }
      if (peer.peer_id == 0) {
        continue;
      }
      if (!peer.ready_parts.get(part_id)) {
        continue;
      }
      //TODO: xor is not a perfect solution as it keeps a lot order between part_ids
      Peer::Key key;
      key.part_id = part_id;
      key.rnd = rnd ^ peer.rnd;

      if (from_count != 0 && from_priority != 0) {
        key.count = from_count;
        key.priority = from_priority;
        peer.rarest_parts.erase(key);
      }

      if (to_count != 0 && to_priority != 0) {
        key.count = to_count;
        key.priority = to_priority;
        peer.rarest_parts.insert(key);
      }
    }
  }
};
}  // namespace ton
